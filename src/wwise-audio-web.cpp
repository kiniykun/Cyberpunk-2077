// Browser platform output for the unchanged Wwise software mixer and graph.
// [audio] The Wwise sink writes real original-mixer PCM (CAkSinkBase gain/interleave) into a lock-free
// ring buffer that lives in wasm shared memory. An AudioWorkletNode on the browser audio thread reads the
// ring directly through the SharedArrayBuffer, so the Wwise audio thread never calls into the main thread
// per buffer. A pacer pthread wakes the Wwise audio thread whenever the worklet has drained a buffer (the
// role a hardware callback plays on other platforms) and, while the AudioContext is not running (autoplay
// policy before the first click, background tab, no device), consumes the ring at wall-clock rate so Wwise
// time keeps moving. The context resumes on the first pointer/key/touch event (e.g. the pointer-lock click).
// Previous AudioBufferSourceNode implementation: src/wwise-audio-web.cpp.audio-bak.
#include "AkLEngine.h"
#include "AkSinkBase.h"
#include "AkSettings.h"
#include <AK/Tools/Common/AkObject.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <thread>

extern AkInitSettings g_settings;
extern AkPlatformInitSettings g_PDSettings;

namespace {
// Shared header, read and written with Atomics by the AudioWorklet (Int32Array view on wasm memory).
enum RingSlot : int {
    kWrite = 0,          // frames written by the sink (wrapping uint32)
    kRead = 1,           // frames consumed (worklet, or pacer while emulating); futex/notify word
    kUnderruns = 2,      // worklet underrun events after priming
    kHeartbeat = 3,      // worklet process() calls
    kWorkletFrames = 4,  // frames played by the worklet
    kBufferFrames = 5,   // Wwise buffer size (frames)
    kCapacity = 6,       // ring capacity (frames, power of two)
    kNodeState = 7,      // 0 none, 1 loading, 2 node connected, -1 failed
    kOutRms = 8,         // float bits: worklet output RMS over the last ~1 s
    kOutPeak = 9,        // float bits: worklet output peak over the last ~1 s
    kContextState = 10,  // 0 suspended, 1 running, 2 closed/interrupted, -1 none
    kPrefill = 11,       // frames the worklet waits for before (re)starting
    kUnderrunFrames = 12,// zero frames emitted by underruns
    kTestFlags = 13,     // ?audiotest=: 1 tone, 2 wwise sample bank
    kEmulatedFrames = 14,// frames dropped by the pacer while the context was not consuming
    kResumes = 15,       // successful AudioContext resumes
    kSlots = 16
};
constexpr uint32_t kRingFrames = 16384; // power of two, 341 ms at 48 kHz
alignas(64) std::atomic<int32_t> g_ring[kSlots];
alignas(64) float g_ringData[kRingFrames * 2];
static_assert(sizeof(std::atomic<int32_t>) == 4, "ring header must be Int32Array compatible");
}

EM_JS(int, BrowserAudioEnsure, (int sampleRate, double headerAddress, double dataAddress, int capacity), {
    const H = Number(headerAddress), D = Number(dataAddress);
    const header = () => new Int32Array(wasmMemory.buffer, H, 16);
    try {
        const params = new URLSearchParams(globalThis.location ? globalThis.location.search : "");
        const test = params.get('audiotest') || "";
        Atomics.store(header(), 13, (test.indexOf('tone') >= 0 ? 1 : 0) | (test.indexOf('wwise') >= 0 ? 2 : 0));
        const previous = Module.cp2077Audio;
        if (previous && previous.context.state !== 'closed') return previous.context.sampleRate;
        const Audio = globalThis.AudioContext || globalThis.webkitAudioContext;
        if (!Audio) { Atomics.store(header(), 10, -1); return 0; }
        const context = new Audio({sampleRate, latencyHint: 'interactive'});
        const runtime = {context, node: null, failed: false, resumes: 0};
        const stateCode = () => context.state === 'running' ? 1 : (context.state === 'suspended' ? 0 : 2);
        context.onstatechange = () => {
            Atomics.store(header(), 10, stateCode());
            console.log('CP2077 Web Audio state: ' + context.state);
        };
        Atomics.store(header(), 10, stateCode());
        runtime.resume = () => {
            if (context.state === 'suspended' || context.state === 'interrupted')
                context.resume().then(() => { Atomics.add(header(), 15, 1); }).catch(error => {
                    console.error('CP2077 Web Audio resume failed', error);
                });
        };
        for (const type of ['pointerdown', 'mousedown', 'keydown', 'touchend', 'click'])
            globalThis.addEventListener(type, runtime.resume, true);
        Module.cp2077Audio = runtime;
        Module.cp2077AudioResume = runtime.resume;
        Module.cp2077AudioStats = () => {
            const h = header(), f = new Float32Array(wasmMemory.buffer, H, 16);
            return {state: context.state, node: h[7], write: h[0] >>> 0, read: h[1] >>> 0,
                    queued: (h[0] - h[1]) >>> 0, workletFrames: h[4] >>> 0, underruns: h[2], underrunFrames: h[12],
                    emulatedFrames: h[14] >>> 0, heartbeat: h[3] >>> 0, outRms: f[8], outPeak: f[9], resumes: h[15],
                    sampleRate: context.sampleRate, baseLatency: context.baseLatency, outputLatency: context.outputLatency};
        };
        const processor = function() {
            class CP2077RingProcessor extends AudioWorkletProcessor {
                constructor(options) {
                    super();
                    const o = options.processorOptions;
                    this.h = new Int32Array(o.memory, o.header, 16);
                    this.f = new Float32Array(o.memory, o.header, 16);
                    this.d = new Float32Array(o.memory, o.data, o.capacity * 2);
                    this.mask = o.capacity - 1;
                    this.primed = false; this.sum = 0; this.peak = 0; this.count = 0;
                    this.window = Math.max(128, o.sampleRate | 0);
                }
                process(inputs, outputs) {
                    const out = outputs[0], h = this.h, d = this.d, mask = this.mask;
                    const left = out[0], right = out.length > 1 ? out[1] : null, n = left.length;
                    Atomics.add(h, 3, 1);
                    const write = Atomics.load(h, 0), read = Atomics.load(h, 1);
                    let available = (write - read) >>> 0;
                    if (available > mask + 1) available = 0;
                    if (!this.primed) {
                        if (available < Atomics.load(h, 11)) return true;
                        this.primed = true;
                    }
                    const take = Math.min(available, n);
                    let sum = this.sum, peak = this.peak;
                    for (let i = 0; i < take; ++i) {
                        const j = ((read + i) & mask) * 2, l = d[j], r = d[j + 1];
                        left[i] = l; if (right) right[i] = r;
                        sum += l * l + r * r;
                        const a = Math.max(Math.abs(l), Math.abs(r)); if (a > peak) peak = a;
                    }
                    if (take < n) {
                        left.fill(0, take); if (right) right.fill(0, take);
                        Atomics.add(h, 2, 1); Atomics.add(h, 12, n - take);
                        this.primed = false;
                    }
                    if (take) {
                        Atomics.compareExchange(h, 1, read, (read + take) | 0);
                        Atomics.add(h, 4, take);
                        Atomics.notify(h, 1);
                    }
                    this.count += n; this.sum = sum; this.peak = peak;
                    if (this.count >= this.window) {
                        this.f[8] = Math.sqrt(this.sum / (2 * this.count)); this.f[9] = this.peak;
                        this.sum = 0; this.peak = 0; this.count = 0;
                    }
                    return true;
                }
            }
            registerProcessor('cp2077-wwise-ring', CP2077RingProcessor);
        };
        Atomics.store(header(), 7, 1);
        const url = URL.createObjectURL(new Blob(['(' + processor.toString() + ')();'], {type: 'text/javascript'}));
        context.audioWorklet.addModule(url).then(() => {
            URL.revokeObjectURL(url);
            const node = new AudioWorkletNode(context, 'cp2077-wwise-ring', {
                numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
                processorOptions: {memory: wasmMemory.buffer, header: H, data: D, capacity, sampleRate: context.sampleRate}});
            node.onprocessorerror = event => { Atomics.store(header(), 7, -1); console.error('CP2077 Web Audio worklet error', event); };
            node.connect(context.destination);
            runtime.node = node;
            Atomics.store(header(), 7, 2);
            console.log('CP2077 Web Audio worklet ready: rate=' + context.sampleRate + ' state=' + context.state);
        }).catch(error => {
            Atomics.store(header(), 7, -1);
            runtime.failed = true;
            console.error('CP2077 Web Audio worklet failed', error);
        });
        runtime.resume();
        return context.sampleRate;
    } catch (error) { console.error('CP2077 Web Audio creation failed', error); Atomics.store(header(), 10, -1); return 0; }
});
EM_JS(void, BrowserAudioClose, (), {
    const runtime = Module.cp2077Audio;
    if (!runtime) return;
    for (const type of ['pointerdown', 'mousedown', 'keydown', 'touchend', 'click'])
        globalThis.removeEventListener(type, runtime.resume, true);
    if (runtime.node) { try { runtime.node.disconnect(); } catch (_) {} }
    Module.cp2077Audio = null;
    runtime.context.close().catch(error => console.error('CP2077 Web Audio close failed', error));
});

namespace {
#include "wwise-audio-test-bank.inc"

uint32_t Load(RingSlot slot) { return uint32_t(g_ring[slot].load(std::memory_order_acquire)); }
float LoadFloat(RingSlot slot) { int32_t bits = g_ring[slot].load(std::memory_order_relaxed); float v; std::memcpy(&v, &bits, 4); return v; }

struct EnsureRequest { int sampleRate = 0; int result = 0; };
bool RunEnsure(EnsureRequest& request) {
    auto invoke = [](void* pointer) {
        auto& r = *static_cast<EnsureRequest*>(pointer);
        r.result = BrowserAudioEnsure(r.sampleRate, double(uintptr_t(&g_ring[0])), double(uintptr_t(&g_ringData[0])), int(kRingFrames));
    };
    if (emscripten_is_main_runtime_thread()) { invoke(&request); return true; }
    static auto* queue = em_proxying_queue_create();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), invoke, &request);
}
void RunClose() {
    if (emscripten_is_main_runtime_thread()) { BrowserAudioClose(); return; }
    static auto* queue = em_proxying_queue_create();
    if (queue) emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), [](void*) { BrowserAudioClose(); }, nullptr);
}

std::mutex g_contextMutex;
AK::IAkSinkPluginContext* g_sinkContext = nullptr; // guarded by g_contextMutex
std::atomic<uint32_t> g_sampleRate{48000};
std::atomic<uint32_t> g_bufferFrames{1024};
std::atomic<uint32_t> g_targetFrames{4096};
std::atomic<bool> g_pacerStarted{false};

// Wakes the Wwise audio thread when a buffer of space is free; emulates consumption while the
// AudioContext/worklet is not running so that voices, durations and end-of-event callbacks advance.
void PacerLoop() {
    uint32_t lastHeartbeat = Load(kHeartbeat);
    double lastHeartbeatChange = emscripten_get_now(), lastEmulation = lastHeartbeatChange, carry = 0;
    bool emulating = false;
    unsigned long long wakeups = 0;
    uint32_t lastSignalWrite = ~0u; double lastSignal = 0;
    for (;;) {
        const uint32_t frames = std::max<uint32_t>(64, g_bufferFrames.load());
        const double periodMs = 1000.0 * frames / std::max<uint32_t>(8000, g_sampleRate.load());
        emscripten_futex_wait(reinterpret_cast<volatile void*>(&g_ring[kRead]), Load(kRead), std::max(2.0, periodMs * 0.5));
        const double now = emscripten_get_now();
        const uint32_t heartbeat = Load(kHeartbeat);
        if (heartbeat != lastHeartbeat) { lastHeartbeat = heartbeat; lastHeartbeatChange = now; }
        const bool consuming = now - lastHeartbeatChange < 250.0;
        if (!consuming) {
            if (!emulating) { emulating = true; lastEmulation = now; carry = 0; std::fprintf(stderr, "CP2077 audio ring: worklet not consuming (context suspended/none) -> wall-clock pacing\n"); }
            carry += (now - lastEmulation) * g_sampleRate.load() / 1000.0;
            lastEmulation = now;
            const uint32_t read = Load(kRead), queued = Load(kWrite) - read;
            const uint32_t drop = std::min<uint32_t>(queued, uint32_t(std::min(carry, double(kRingFrames))));
            if (drop) {
                int32_t expected = int32_t(read);
                if (g_ring[kRead].compare_exchange_strong(expected, int32_t(read + drop), std::memory_order_acq_rel))
                    g_ring[kEmulatedFrames].fetch_add(int32_t(drop), std::memory_order_relaxed);
                carry -= drop;
            }
            if (carry > frames * 4.0) carry = frames * 4.0;
        } else if (emulating) {
            emulating = false;
            std::fprintf(stderr, "CP2077 audio ring: worklet consuming -> device pacing\n");
        }
        // One wake per rendered buffer (or per period if the audio thread has not caught up yet).
        const uint32_t write = Load(kWrite), queued = write - Load(kRead);
        if (queued + frames <= g_targetFrames.load() && (write != lastSignalWrite || now - lastSignal >= periodMs)) {
            std::lock_guard<std::mutex> lock(g_contextMutex);
            if (g_sinkContext) { g_sinkContext->SignalAudioThread(); ++wakeups; lastSignalWrite = write; lastSignal = now; }
        }
    }
}

// ?audiotest=wwise: end-to-end Wwise check without game banks. Loads the embedded Audiokinetic sample bank
// (footsteps, ADPCM) from memory and posts Play_Footsteps on a self-listening test object.
void WwiseSampleBankTest() {
    for (int i = 0; i < 600 && !AK::SoundEngine::IsInitialized(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::seconds(8));
    AkBankID bank = 0;
    const AKRESULT loaded = AK::SoundEngine::LoadBankMemoryView(kCpAudioTestBank, kCpAudioTestBankSize, bank);
    const AkGameObjectID object = 0x7E57A0D10C0FFEEull;
    const AKRESULT registered = AK::SoundEngine::RegisterGameObj(object);
    const AKRESULT listeners = AK::SoundEngine::SetListeners(object, &object, 1);
    AkSoundPosition position; position.SetPosition(0, 0, 0); position.SetOrientation(0, 0, 1, 0, 1, 0);
    const AKRESULT positioned = AK::SoundEngine::SetPosition(object, position);
    const AKRESULT switched = AK::SoundEngine::SetSwitch(AkSwitchGroupID(1834394558u), AkSwitchStateID(2473969246u), object);
    std::fprintf(stderr, "CP2077 audio test bank: load=%d bank=%u register=%d listeners=%d position=%d switch=%d\n",
        int(loaded), unsigned(bank), int(registered), int(listeners), int(positioned), int(switched));
    unsigned ok = 0, failed = 0;
    for (int i = 0; i < 3000; ++i) { // ~15 min
        const AkPlayingID id = AK::SoundEngine::PostEvent(AkUniqueID(3854155799u), object);
        (id == AK_INVALID_PLAYING_ID ? failed : ok)++;
        if (i < 3 || (i % 50) == 0) std::fprintf(stderr, "CP2077 audio test event: Play_Footsteps n=%d playing=%u ok=%u failed=%u\n", i, unsigned(id), ok, failed);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

class BrowserPlatformContext final : public AK::IAkPlatformContext {};
class BrowserWwiseSink final : public CAkSinkBase {
    AkAudioBuffer buffer;
    float* samples = nullptr;
    AkUInt16 frames = 0;
    uint32_t seenUnderruns = 0;
    // write-side statistics
    double sum = 0; float peak = 0; uint32_t statFrames = 0; double tonePhase = 0;
    bool reportedSound = false;
    static inline unsigned long long passes = 0;
public:
    AKRESULT Init(AK::IAkPluginMemAlloc*, AK::IAkSinkPluginContext* context, AK::IAkPluginParam*, AkAudioFormat& format) override {
        if (samples || !g_settings.uNumSamplesPerFrame || g_settings.uNumSamplesPerFrame > kRingFrames / 4 ||
            !g_PDSettings.uSampleRate) return AK_InvalidParameter;
        frames = AkUInt16(g_settings.uNumSamplesPerFrame);
        AkChannelConfig channels;
        channels.SetStandard(AK_SPEAKER_SETUP_STEREO);
        format.SetAll(g_PDSettings.uSampleRate, channels, 32, 8, AK_FLOAT, AK_INTERLEAVED);
        samples = static_cast<float*>(AkMalign(AkMemID_Processing, size_t(frames) * 2 * sizeof(float), AK_SIMD_ALIGNMENT));
        if (!samples) return AK_InsufficientMemory;
        buffer.AttachInterleavedData(samples, frames, 0, channels);
        // Queue target: 4 Wwise buffers (85 ms at 1024/48k), at least 3x the refill count, within the ring.
        const uint32_t target = std::min<uint32_t>(kRingFrames - frames, uint32_t(frames) * std::max<uint32_t>(4, 2u * std::max<uint32_t>(1, g_PDSettings.uNumRefillsInVoice)));
        g_bufferFrames.store(frames); g_sampleRate.store(g_PDSettings.uSampleRate); g_targetFrames.store(target);
        g_ring[kBufferFrames].store(frames); g_ring[kCapacity].store(int32_t(kRingFrames));
        g_ring[kPrefill].store(int32_t(std::min<uint32_t>(target, uint32_t(frames) * 2)));
        seenUnderruns = Load(kUnderruns);
        { std::lock_guard<std::mutex> lock(g_contextMutex); g_sinkContext = context; }
        bool expected = false;
        if (g_pacerStarted.compare_exchange_strong(expected, true)) {
            std::thread(PacerLoop).detach();
            if (Load(kTestFlags) & 2) std::thread(WwiseSampleBankTest).detach();
        }
        std::fprintf(stderr, "CP2077 audio sink init: rate=%u frames=%u refills=%u target=%u ring=%u node=%d ctx=%d test=%u (worklet ring)\n",
            unsigned(g_PDSettings.uSampleRate), unsigned(frames), unsigned(g_PDSettings.uNumRefillsInVoice), target,
            unsigned(kRingFrames), int(g_ring[kNodeState].load()), int(g_ring[kContextState].load()), Load(kTestFlags));
        return AK_Success;
    }
    AKRESULT Term(AK::IAkPluginMemAlloc* allocator) override {
        { std::lock_guard<std::mutex> lock(g_contextMutex); g_sinkContext = nullptr; }
        std::fprintf(stderr, "CP2077 audio sink term: passes=%llu written=%u read=%u\n", passes, Load(kWrite), Load(kRead));
        if (samples) { AkFalign(AkMemID_Processing, samples); samples = nullptr; }
        return CAkSinkBase::Term(allocator);
    }
    AKRESULT Reset() override {
        m_bDataReady = false; buffer.uValidFrames = 0;
        seenUnderruns = Load(kUnderruns);
        return AK_Success;
    }
    AKRESULT IsDataNeeded(AkUInt32& needed) override {
        const uint32_t queued = Load(kWrite) - Load(kRead), target = g_targetFrames.load();
        needed = queued >= target ? 0 : (target - queued) / frames;
        return AK_Success;
    }
    bool IsStarved() override { return Load(kUnderruns) != seenUnderruns; }
    void ResetStarved() override { seenUnderruns = Load(kUnderruns); }
    AkAudioBuffer& NextWriteBuffer() override { buffer.uValidFrames = 0; return buffer; }
    void PassData() override {
        if (!samples) return;
        const uint32_t valid = std::min<uint32_t>(buffer.uValidFrames, frames);
        if (valid < frames) std::memset(samples + size_t(valid) * 2, 0, size_t(frames - valid) * 2 * sizeof(float));
        if (Load(kTestFlags) & 1) { // ?audiotest=tone: 440 Hz at -20 dBFS mixed in, proves ring -> worklet -> speakers
            const double step = 2.0 * 3.14159265358979 * 440.0 / std::max<uint32_t>(8000, g_sampleRate.load());
            for (uint32_t i = 0; i < frames; ++i) { const float s = float(0.1 * std::sin(tonePhase)); tonePhase += step; samples[i * 2] += s; samples[i * 2 + 1] += s; }
            tonePhase = std::fmod(tonePhase, 2.0 * 3.14159265358979);
        }
        const uint32_t write = Load(kWrite), read = Load(kRead);
        if (write - read + frames > kRingFrames) { // cannot happen when IsDataNeeded is honoured; drop rather than overwrite
            static unsigned overflows = 0;
            if (++overflows <= 4) std::fprintf(stderr, "CP2077 audio ring overflow: queued=%u frames=%u\n", write - read, unsigned(frames));
            return;
        }
        const uint32_t start = write & (kRingFrames - 1), first = std::min<uint32_t>(frames, kRingFrames - start);
        std::memcpy(g_ringData + size_t(start) * 2, samples, size_t(first) * 2 * sizeof(float));
        if (first < frames) std::memcpy(g_ringData, samples + size_t(first) * 2, size_t(frames - first) * 2 * sizeof(float));
        g_ring[kWrite].store(int32_t(write + frames), std::memory_order_release);
        for (uint32_t i = 0; i < uint32_t(frames) * 2; ++i) { const float v = samples[i]; sum += double(v) * v; peak = std::max(peak, std::fabs(v)); }
        statFrames += frames; ++passes;
        if (!reportedSound && peak > 1e-4f) {
            reportedSound = true;
            std::fprintf(stderr, "CP2077 audio ring: first non-silent Wwise output at pass=%llu peak=%.5f\n", passes, peak);
        }
        if (passes <= 2 || statFrames >= g_sampleRate.load() * 5) {
            std::fprintf(stderr, "CP2077 audio ring probe: passes=%llu written=%u read=%u queued=%u writeRms=%.6f writePeak=%.5f workletFrames=%u outRms=%.6f outPeak=%.5f underruns=%u underrunFrames=%u emulated=%u node=%d ctx=%d resumes=%d\n",
                passes, Load(kWrite), Load(kRead), Load(kWrite) - Load(kRead), statFrames ? std::sqrt(sum / (2.0 * statFrames)) : 0.0, peak,
                Load(kWorkletFrames), LoadFloat(kOutRms), LoadFloat(kOutPeak), Load(kUnderruns), Load(kUnderrunFrames), Load(kEmulatedFrames),
                int(g_ring[kNodeState].load()), int(g_ring[kContextState].load()), int(g_ring[kResumes].load()));
            sum = 0; peak = 0; statFrames = 0;
        }
    }
    void PassSilence() override {
        if (!samples) return;
        std::memset(samples, 0, size_t(frames) * 2 * sizeof(float));
        buffer.uValidFrames = frames; PassData();
    }
};
}

AK::IAkPlugin* AkCreateDefaultSink(AK::IAkPluginMemAlloc* allocator) {
    return AK_PLUGIN_NEW(allocator, BrowserWwiseSink());
}
AkEvent CAkLEngine::m_EventStop;
AK::IAkPlatformContext* CAkLEngine::m_pPlatformContext = nullptr;
void CAkLEngine::GetDefaultPlatformInitSettings(AkPlatformInitSettings& settings) {
    std::memset(&settings, 0, sizeof(settings));
    GetDefaultPlatformThreadInitSettings(settings);
    settings.uNumRefillsInVoice = AK_DEFAULT_NUM_REFILLS_IN_VOICE_BUFFER;
    settings.uSampleRate = DEFAULT_NATIVE_FREQUENCY;
}
void CAkLEngine::GetDefaultOutputSettings(AkOutputSettings& settings) { GetDefaultOutputSettingsCommon(settings); }
bool CAkLEngine::PlatformSupportsHwVoices() { return false; } // Original software engine also has no hardware voices.
void CAkLEngine::PlatformWaitForHwVoices() {}
AKRESULT CAkLEngine::Init() {
    for (auto& slot : g_ring) slot.store(0);
    g_ring[kContextState].store(-1);
    EnsureRequest context{int(g_PDSettings.uSampleRate)};
    const bool ensured = RunEnsure(context) && context.result > 0;
    std::fprintf(stderr, "CP2077 audio engine init: requestedRate=%u contextRate=%d ok=%u output=AudioWorklet ring (SharedArrayBuffer)\n",
        unsigned(g_PDSettings.uSampleRate), context.result, unsigned(ensured));
    // Without a Web Audio context the engine still runs; the pacer consumes the ring at wall-clock rate.
    if (ensured) g_PDSettings.uSampleRate = AkUInt32(context.result);
    AkAudioLibSettings::SetAudioBufferSettings(g_PDSettings.uSampleRate, g_settings.uNumSamplesPerFrame);
    return SoftwareInit();
}
AKRESULT CAkLEngine::InitPlatformContext() {
    if (m_pPlatformContext) return AK_Fail;
    m_pPlatformContext = AkNew(AkMemID_SoundEngine, BrowserPlatformContext());
    return m_pPlatformContext ? AK_Success : AK_InsufficientMemory;
}
void CAkLEngine::Term() { SoftwareTerm(); }
void CAkLEngine::TermPlatformContext() {
    AkDelete(AkMemID_SoundEngine, m_pPlatformContext); m_pPlatformContext = nullptr;
    RunClose();
}
void CAkLEngine::Perform() { SoftwarePerform(); }
AKRESULT CAkLEngine::GetPlatformDeviceList(AkPluginID plugin, AkUInt32&, AkDeviceDescription*) {
    // Web Audio supplies its default destination, without exposing OS device
    // enumeration through this backend. The original API defines this status.
    return plugin == AKPLUGINID_DEFAULT_SINK ? AK_NotImplemented : AK_NotCompatible;
}
