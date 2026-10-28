(() => {
  if (globalThis.cp2077GpuUploadStats || !globalThis.GPUQueue) return;
  // [warmup] Opt-in (?uploadStats=1): wrapping every writeBuffer (~180/frame) costs main-thread time and allocation.
  try { if (new URLSearchParams(location.search).get('uploadStats') !== '1') return; } catch (e) { return; }
  const stats = globalThis.cp2077GpuUploadStats = {
    calls: 0, bytes: 0, largestWriteBytes: 0, sampledCalls: 0,
    sampledCpuMs: 0, startedAt: performance.now(), labels: Object.create(null)
  };
  const original = GPUQueue.prototype.writeBuffer;
  const names = new WeakMap();
  GPUQueue.prototype.writeBuffer = function(buffer, bufferOffset, data, dataOffset, size) {
    const sampled = stats.calls % 16 === 0, start = sampled ? performance.now() : 0;
    // Preserve the exact argument list and original native implementation.
    const result = original.apply(this, arguments);
    const unit = data.BYTES_PER_ELEMENT || 1;
    const bytes = size === undefined ? data.byteLength - (dataOffset || 0) * unit : size * unit;
    stats.calls++;
    if (Number.isFinite(bytes) && bytes >= 0) {
      stats.bytes += bytes; stats.largestWriteBytes = Math.max(stats.largestWriteBytes, bytes);
      let name = names.get(buffer);
      if (name === undefined) { name = String(buffer.label || '(unlabelled)').slice(0, 120); names.set(buffer, name); }
      if (!(name in stats.labels) && Object.keys(stats.labels).length >= 64) name = '(other labels)';
      const record = stats.labels[name] ||= {calls:0,bytes:0,largestWriteBytes:0};
      record.calls++; record.bytes += bytes; record.largestWriteBytes = Math.max(record.largestWriteBytes, bytes);
    }
    if (sampled) { stats.sampledCalls++; stats.sampledCpuMs += performance.now() - start; }
    return result;
  };
})();
