// [hitch] Non-blocking stdout/stderr for pthreads (pre-js).
// Emscripten's fd_write on a pthread is a SYNCHRONOUS proxy to the main browser thread
// (proxyToMainThread(..., sync=1)), so every fprintf(stderr, ...) on the game thread or a render/job worker
// waits until the main thread is idle enough to run printErr. Any long main-thread task (GPU encoding, GC,
// a page script) then also stalls every logging worker. Here worker writes to fd 1/2 are decoded in the worker,
// split into lines and forwarded with the asynchronous print/printErr handler (postMessage), never blocking.
// Other fds and main-thread writes keep the original path. ?syncStdio=1 restores the original behaviour.
(function () {
  if (typeof _fd_write !== 'function') return;
  const original = _fd_write;
  let pending = ['', '', ''];
  let enabled;
  const decoder = typeof TextDecoder !== 'undefined' ? new TextDecoder() : null;
  _fd_write = function (fd, iov, iovcnt, pnum) {
    if (!ENVIRONMENT_IS_PTHREAD || (fd !== 1 && fd !== 2) || !decoder) return original(fd, iov, iovcnt, pnum);
    if (enabled === undefined) {
      try { enabled = !(typeof location !== 'undefined' && new URLSearchParams(location.search).get('syncStdio') === '1'); }
      catch (e) { enabled = true; }
    }
    if (!enabled) return original(fd, iov, iovcnt, pnum);
    growMemViews();
    const base = Number(iov) / 8, count = Number(iovcnt);
    let total = 0, text = '';
    for (let i = 0; i < count; ++i) {
      const ptr = Number(HEAPU64[base + 2 * i]), len = Number(HEAPU64[base + 2 * i + 1]);
      if (!len) continue;
      text += decoder.decode(HEAPU8.slice(ptr, ptr + len)); // slice: TextDecoder rejects shared views
      total += len;
    }
    let buffered = pending[fd] + text, newline;
    const sink = fd === 1 ? out : err;
    while ((newline = buffered.indexOf('\n')) >= 0) {
      sink(buffered.slice(0, newline));
      buffered = buffered.slice(newline + 1);
    }
    if (buffered.length > 65536) { sink(buffered); buffered = ''; }
    pending[fd] = buffered;
    growMemViews();
    HEAPU64[Number(pnum) / 8] = BigInt(total);
    return 0;
  };
})();
