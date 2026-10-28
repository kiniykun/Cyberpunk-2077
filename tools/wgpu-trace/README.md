# wgpu-trace

Minimal critical-path tracing for a multithreaded C++/WebAssembly renderer whose workers proxy WebGPU calls to the main thread.

It records IDs and timestamps at every existing hand-off, changes no rendering or error behavior, and reconstructs per frame which chain of dependencies actually determined the frame time.

```
runtime/wgtrace.h         worker side (Emscripten, header only)
runtime/wgtrace_main.js   main-thread side (wraps the existing proxy dispatcher, scopes, submits, presentation)
analyzer/critical_path.js offline analyzer and CLI
analyzer/critical_path.test.js  synthetic tests (paths, waits, presentation)
analyzer/counters.test.js       counter waits, cooperative execution, preserved records 16-20
analyzer/generations.test.js    counter-generation races and ambiguity reporting
runtime/wgtrace_main.test.js    main-thread runtime (bounds, MEMORY64 pointers, site names)
runtime/test/                   deterministic emsdk race test (two TUs, MEMORY64, pthreads)
```

`npm test` runs all four test files. The race test is built and run with `runtime/test/build_and_run.ps1`, which uses the engine's emsdk at `D:\Cyber punk\tools\emsdk`. The run writes `runtime/test/out/race_trace.json`, and `generations.test.js` analyzes that file when it exists.

Run the tests with `npm test`. Analyze an export with `node analyzer/critical_path.js trace.json --frames`.

## Port integration copy

This copy was staged from `C:/Users/Brady/Documents/JB/wgpu-trace`. The original
folder was preserved. The C++ runtime has now compiled and executed under actual
Emscripten MEMORY64 with four workers and two translation units. Concurrent reset
and emission were exercised. The 10 analyzer tests and 3 added JS runtime tests pass.

Changes in this copy: bounded main-thread records (65,536 by default), explicit
drop counts, optional current-heap provider, checked MEMORY64 pointer conversion,
atomic publication of worker rings, bounds-checked accessors, and serialized reset
versus emit. `now()` uses the SDK clock instead of defining an EM_JS function in
every translation unit. Reset/export must be serialized on the main thread; this
does not support another thread resetting while an export copies records.

For the engine's unexported heap, supply `heapProvider` from a closure installed
inside its generated runtime. Do not access unexported Module.HEAPU8 getters.
An unpublished ring pointer is skipped; future exports will include it.

This tool is not yet wired to the engine's job dependencies and wait sites.
Passing these tests is not evidence of a measured game critical path or improved FPS.

### Counter support merged into this copy

The counter hooks, site registry, and loss counters were merged into this header without changing any of the
fixes above:

- `now()` is still `emscripten_get_now()`. With pthreads, emsdk 6.0.12 defines it as
  `performance.timeOrigin + performance.now()`, the same clock as the main thread.
- The ring writer flag, atomic publication, and reset lock are untouched. Unregistered emits are counted before
  the ring is touched.
- All new exports are inside `WGTRACE_IMPLEMENTATION`. They are bounds-checked and acquire-loaded. Pointers are
  returned as `uintptr_t`.
- `wgtrace_reset` zeroes the loss counters but never the counter-instance table, because clearing the table would
  split a generation that is in flight.

Verified with emsdk 6.0.12 (`-std=c++17 -pthread -sMEMORY64 -Wall -Wextra`, two translation units, node 24):

- Two threads racing to create the implicit instance produce exactly one ARM, and both threads use it.
- A contributor and a decrementer read the previous instance while the new generation's publisher is parked.
  The stamps prove the race, and the analyzer reports both records as ambiguous, with the stale instance and the
  next generation as candidates.
- Re-arms publish distinct explicit instances.
- A decrementer is parked after its `fetch_sub` reaches zero but before its end stamp. The waiter observes zero
  first, and the analyzer ends the zero window at the wake, with no inconsistency.
- Site names publish across translation units.
- Emits from unregistered threads are counted.
- On the trace this run writes, the analyzer reports exactly the forced race (job 103's `add_after_zero` and
  `dec_after_zero`) and verifies every other zero.

`npm test` does not build the emsdk test. Its trace check is skipped when `runtime/test/out/race_trace.json` is
missing, so run `build_and_run.ps1` first. No engine build or browser capture has been made with these hooks.

Do not re-stage from `C:/Users/Brady/Documents/JB/wgpu-trace`. That copy predates both these fixes and the counter
merge: it still has `EM_JS`, single-call `ctr_add`/`ctr_dec`, and non-atomic site names.

## What is recorded

Every record is `{type, frameId, a, b, c, tid, t0, t1}`. All CPU stamps are `performance.timeOrigin + performance.now()` on both sides. No offset correction is applied; ordering violations are reported by the analyzer as inconsistencies, not corrected.

| type | a | b | c | t0 | t1 | emitted by |
|---|---|---|---|---|---|---|
| FRAME | | | | frame start | frame end | orchestrator |
| JOB_RUN | jobId | workerId | | start | end | JobScope |
| JOB_DEP | jobId | depJobId | | | | job system, when a job waits for a dep before starting |
| JOB_SPAWN | jobId | parentJobId | | spawn time | | job system |
| WAIT | jobId | kind | targetId | block start | block end | WaitScope |
| PROXY_DEQ | reqId | | | | dequeued | main |
| PROXY_CALL | reqId | callKind | objId | call start | call end | main |
| SCOPE | scopeId | wrappedId | reqId | push | pop | main |
| SETTLE | id | 1 scope / 2 submit | result | settle time | | main, promise handler |
| SUBMIT | submitId | reqId | nBuffers | call start | call end | main |
| SUBMIT_ENC | submitId | encoderId | | | | main |
| GPU_PASS | submitId | passIndex | | duration ms | | optional, GPU clock |
| PRESENT | presentId | reqId | copyScopeId | raf scheduled | raf fired | main |
| GCT | presentId | | | start | end | main |
| PRESENT_DONE | presentId | copySubmitId | | copy submit returned | | main |

| JOB_META (16) | jobId | counter address (low 32) | name id | run start | run end | existing engine integration |
| COUNTER_ACTIVITY (17) | jobId | counter address (low 32) | site id | start | end | existing engine integration |
| MUTEX_WAIT (18) / MUTEX_HOLD (19) | integration-defined | integration-defined | integration-defined | start | end | existing engine integration |
| RENDER_PHASE (20) | integration-defined | integration-defined | phase | start | end | existing engine integration |
| CTR_ARM (21) | instance | address lo32 | address hi32 | stamp after publish | 1 if implicit | `ctr_add_end` (fetch_add returned 0) or first sighting |
| CTR_CONTRIB (22) | instance | contributing jobId | delta | before engine fetch_add | after instance read | `ctr_add_begin` / `ctr_add_end` |
| CTR_DEC (23) | instance | decrementing jobId | value after (0 = zero, 0xFFFFFFFF = negative) | before instance read and fetch_sub | after fetch_sub | `ctr_dec_begin` / `ctr_dec_end` |
| CTR_WAIT (24) | instance | waiting jobId | site id | block start | resume | `CounterWaitScope` |
| CTR_WAKE (25) | instance | waiting jobId | site id | zero observed | resume | `CounterWaitScope::woke()` |

Wait kinds: 1 proxy (target reqId), 2 scope (scopeId), 3 submit_done (submitId), 4 job (jobId), 5 present (presentId), 6 other. CTR_WAIT records become wait kind 7 (counter) inside the analyzer.

Records 16–20 are preserved verbatim and counted in `preserved`. They never become dependency edges. Counters are reused at the same address, so a JOB_META address match is reported only as an "address association". Other record types are kept in `index.unknown` and noted once per type.

## Counter waits

Counter waits are where the job system blocks until N jobs have decremented a shared counter. Every counter generation at an address gets an instance id. The hooks bracket the engine's own atomics and change nothing about them:

```cpp
// enqueue: end runs BEFORE the job becomes visible to other workers
auto at = wgtrace::ctr_add_begin();
int64_t before = counter.value.fetch_add(1);                  // unchanged engine code
wgtrace::ctr_add_end(at, &counter, job->id, 1, before);
PushJob(job);                                                  // unchanged engine code

// completion
auto dt = wgtrace::ctr_dec_begin(&counter);
int64_t after = counter.value.fetch_sub(1) - 1;                // unchanged engine code
wgtrace::ctr_dec_end(dt, job->id, after);

// wait
{
  wgtrace::CounterWaitScope w(&counter, WGTRACE_SITE("RenderFrame/WaitPreviousCommands"));
  while (counter.value.load() != 0) { if (!TryRunOneJob()) WaitOnCounter(counter); }   // unchanged engine loop
  w.woke();   // first point at which this thread has observed zero
}
```

Generation rules in the runtime:

- The caller whose `fetch_add` returned 0 publishes a new instance with an exchange. A CAS from 0 would fail on every re-arm after the first.
- A first sighting with no instance yet creates an implicit one by CAS from 0. A losing thread adopts the winner's id and emits nothing under its own.
- The table is maintained while tracing is disabled, so generations are correct the moment tracing is enabled. It holds 4096 addresses. Overflow is counted in `meta.ctrTableFull`.
- `ctr_add_end` also emits `JOB_SPAWN(job, tl_job)` with `c = 1`. The analyzer labels the enqueue-to-start gap `unresolved:enqueue_to_start`, never `sched`.

The remaining race cannot be closed without changing engine synchronization. Another contributor or decrementer can read the previous instance after the counter returned to zero but before the zero-to-positive contributor publishes the next generation. The runtime does not hide this. The begin stamps let the analyzer detect it.

### Generation audit (analyzer)

The audit runs per instance, against the zero window `[dec begin, dec end]` of the instance's first zero:

| condition | result |
|---|---|
| add began after the zero window ended | ambiguous `add_after_zero` |
| add interval overlaps the zero window | ambiguous `add_overlaps_zero` |
| non-zero decrement began after the zero window, including a second zero | ambiguous `dec_after_zero` |
| decrement overlaps the zero window | ambiguous `dec_overlaps_zero` |
| instance armed before the previous generation at the address could have reached zero | `generation_overlap` (zero not verified) |
| contributed delta cannot equal the decrement count, even allowing every overlapping item either way | `count_mismatch` (zero not verified) |
| instance has no ARM in the capture | `generation_started_before_capture` (zero not verified) |
| wait began after its instance's zero, and a later generation at the address was open during the wait | `counter_wait_generation_ambiguous` |

Every ambiguity lists its candidate instances. A candidate is the tagged instance, plus each later generation at the same address whose arming add began before the item ended and which had not closed before the item began. Publication can lag, so the arm stamp alone is not the bound. Ambiguous items and unverified zeros never become edges. Expect `*_overlaps_zero` when many workers decrement at the same moment. It means the order is unknown, not that a race happened; only the `*_after_zero` reasons show a stale tag. An implicit instance with no contributions or decrements (a wait on a never-incremented counter) does not count as a predecessor for `generation_overlap`. The count check assumes each decrement is a single unit. An engine `fetch_sub(N)` reads as `count_mismatch`. The wait is still split into blocked segments and cooperative jobs, and only the blocked segments count as unresolved. These rules assume every add and decrement on the counter is instrumented. The count check is what catches the cases where that is not true.

On the path, a verified zero becomes `unresolved:counter_zero_window` from the decrement's begin stamp to `min(end stamp, waiter wake)`. The transition happened somewhere inside that window, and preemption before the atomic is part of it. Next comes `wake:counter` (window end, or the last cooperative job, up to the observed zero), then `counter_resume` (observed zero to scope exit). The gap from the contributor's last run segment to the decrement's begin stamp is `counter_signal`.

`--counters` prints every counter wait. Each entry shows:

- contributions with add and decrement intervals and ambiguity flags
- the zero status and count check
- the cooperative jobs, with `cooperativeMs` against `notCooperativeMs`
- the wake latency and resume time
- the full ambiguity list, which notes that no edges are created for these items

Unresolved reasons:

- `counter_zero_not_recorded`
- `counter_zero_<status>`
- `counter_wait_without_instance`
- `counter_wait_generation_ambiguous`
- `counter_contributor_without_run`
- `counter_decrement_outside_job`, with the tid of the decrementing thread
- `counter_wake_not_stamped`
- `counter_wait_untyped_release_unknown`, for a legacy kind-6 wait matched to COUNTER_ACTIVITY

### Coverage requirements for the counter capture

- Call `thread_register` on every native worker. The overlay reports 26 workers, and the existing trace contains 14 thread ids. Emits from unregistered threads are counted in `meta.unregisteredEmits`, and nothing else records them.
- Wrap any thread that does engine work outside jobs in a per-iteration `JobScope`. In the existing trace, tid 2003238920 issues proxy waits as job 0 with no JOB_RUN. Its `BufferDestroyOrFinalRelease` at 54.2 ms is the last recorded activity before the 62.8 ms release.
- Instrument every add and decrement on the counters being waited on. Otherwise the audit reports `count_mismatch` rather than a dependency.
- Capture with `createMainTrace({ module, heapProvider, submitCompletion: true })`. Site names are decoded from `heapProvider()` bytes, so `UTF8ToString` does not need to be exported.

GPU_PASS carries a duration only. Timestamp queries run on the GPU clock, and the analyzer never places them on the CPU timeline. They are reported beside the `gpu_completion` interval of the same submit so the reader can compare, not subtract.

SETTLE records are attributed by the ID captured in each promise handler's closure. Arrival order is never used.

## Integration, smallest patch first

Worker side, in this order:

1. Call `wgtrace::thread_register(tid)` once at the start of every pthread and on the main thread. Define `WGTRACE_IMPLEMENTATION` in exactly one translation unit.
2. Wrap each job's body in `wgtrace::JobScope job(frameId, jobId, workerId)`. Wrap the frame orchestrator's per-frame work in a JobScope too, and emit `frame_end(frameId, t0, t1)` from it.
3. Where a job records a dependency before starting, emit `job_dep`. Where a job is spawned from a running job, emit `job_spawn`.
4. Wrap the existing blocking sites with `wgtrace::WaitScope w(kind, target)`: the synchronous proxy wait, any wait on an error-scope result, any wait on submission completion, joins on other jobs, and the presentation wait. Allocate `reqId` with `next_id(ID_REQ)` and send it with the proxied request.

Main side, using `createMainTrace({ module })`:

1. In the proxy dispatcher call `proxyDeq(reqId, frameId)` when a request is taken off the queue and run the handler inside `proxyCall(reqId, frameId, kindName, objId, fn)`.
2. Replace each `pushErrorScope … popErrorScope` pair with `scope(device, {scopeId, wrappedId, reqId, frameId, filter}, fn)`. It returns the same `popErrorScope` promise, so the existing await stays exactly as it is. Pass the scopeId back to the worker if the worker later blocks on it.
3. Route `queue.submit` through `submit(queue, {submitId, reqId, frameId, encoderIds}, buffers)`.
4. In the rAF presentation path call `presentScheduled` where the worker's request arrives, `presentRafFired` at the top of the callback, `getCurrentTexture` in place of the direct call, and `presentDone` after the copy's submit returns.
5. On a key press call `download()`.

Optional, off by default: `submitCompletion: true` attaches `onSubmittedWorkDone` to every submit, and `gpuPass` records timestamp-query durations. Both add work, and the trace shows their cost.

The one invariant the patch relies on: a proxied call does its push, operation, and pop inside one main-thread handler, so scopes never interleave across requests.

## How the analyzer reconstructs the path

Per frame it builds a graph of activities. Each job is split into run segments and wait segments by its WAIT records, so a blocked interval is never counted as both. Each wait is replaced by the chain that actually completed it:

- proxy: delivery (enqueue to dequeue), dispatch (dequeue to call start), the main-thread call, and a wake segment from the moment the call returned, or from the scope settle if the handler awaited one, to the moment the worker resumed
- scope: the settle interval from pop to settle, then wake
- submit_done: the main-thread submit, the completion interval from submit return to settle, then wake
- job: wake from the target job's end
- present: rAF wait, getCurrentTexture, copy, optional copy-scope settle, then wake

Edges run from prerequisite completion to dependent start. A spawned job's edge comes from the point inside its parent where the spawn happened. A joined job's edge comes from the target job's last segment. The blocked portion of a wait has no node of its own, because the time is covered by the chain it waited on.

The critical path is the backward walk from frame end, always following the prerequisite that completed last. Its entries sum to the frame length exactly. Gaps between a prerequisite and a dependent are labelled `sched` when both are job segments, and `unresolved:*` otherwise. Waits whose target has no records become `unresolved:wait:<kind>` and are listed with their target ID. Time carried in from a prior frame is `prev_frame_carry`.

## Slack versus predicted improvement

Both are reported for every wait on the critical path, and they are different numbers.

- Slack is conventional total float: how far the activity could slip before the frame end moves. It is zero for everything on the path.
- Predicted improvement if zero is the frame reduction from re-scheduling the graph with that one activity's duration set to zero while every other duration and ordering constraint keeps its measured value. It is bounded by the next-longest chain, which is often the same worker's own earlier work. The tests include a case where a 27.5 ms settle wait on the path yields only 22 ms because the worker did not begin waiting until 8 ms in.

The model holds everything else fixed. It does not predict second-order effects such as a less busy main thread serving other proxies sooner.

## Reading the summary

`critical path covers` should be 100%. If it is lower, look at the per-frame inconsistency list.

`unresolved mean` is time the trace could not attribute. The per-frame output names each unresolved item and why: a wait on a scope with no SETTLE, a job with no recorded dependency, a settle with no SCOPE record.

The category table says where critical-path time went. The improvements table says which wait to attack first and how much it would actually buy. Decide on the asynchronous presentation rewrite only after `raf_wait`, `present_copy`, or a copy-scope `scope_settle` shows up at the top of that table.

## What this trace cannot tell you

`scope_settle` and `gpu_completion` are measured on the CPU side as pop-to-settle and submit-to-settle. From JavaScript alone they cannot be split into GPU-process backlog, validation time, and callback delivery. If either dominates, the next step is one short run with Chrome tracing categories `gpu`, `gpu.dawn`, and `toplevel`, matched against the SCOPE and SUBMIT stamps in this trace.
