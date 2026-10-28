// Real local file bytes exposed to the native libc filesystem, fetched by range.
// No reconstructed resources, replacement menus or fabricated directory entries.
Module['preRun'] = [].concat(Module['preRun'] || [], function () {
  if (typeof ENVIRONMENT_IS_PTHREAD !== 'undefined' && ENVIRONMENT_IS_PTHREAD) return;
  const dependency = 'cp2077-resource-file-metadata';
  addRunDependency(dependency);
  const blockSize = 1024 * 1024;
  const cacheLimit = 64 * 1024 * 1024;
  const cache = new Map();
  let cachedBytes = 0;
  const metrics = {files: 0, requests: 0, bytesFetched: 0, cacheBytes: 0,
    asyncRequests: 0, asyncHits: 0, asyncErrors: 0, pendingAsyncBytes: 0, syncReadMs: 0};
  const pending = new Map(), lastDemandBlock = new Map(), asyncBlocks = new Set();
  Module['cp2077ResourceFileMetrics'] = metrics;
  // Emscripten libc uses WASI errno numbers (SDK wasi/api.h).
  const error = code => { throw new FS.ErrnoError(code); };
  function storeBlock(key, bytes, asynchronous) {
    if (cache.has(key)) { cachedBytes -= cache.get(key).length; cache.delete(key); }
    while (cachedBytes + bytes.length > cacheLimit && cache.size) {
      const oldest = cache.keys().next().value;
      cachedBytes -= cache.get(oldest).length;
      cache.delete(oldest); asyncBlocks.delete(oldest);
    }
    cache.set(key, bytes); cachedBytes += bytes.length;
    if (asynchronous) asyncBlocks.add(key); else asyncBlocks.delete(key);
    metrics.cacheBytes = cachedBytes;
  }
  function prefetchSequentialBlock(entry, number) {
    const key = entry.path + ':' + number, start = number * blockSize;
    const end = Math.min(start + blockSize, entry.size) - 1;
    if (end < start || cache.has(key) || pending.has(key) || pending.size >= 2) return;
    const controller = new AbortController(), size = end - start + 1;
    pending.set(key, controller); metrics.pendingAsyncBytes += size;
    ++metrics.asyncRequests;
    fetch(entry.path.split('/').map(encodeURIComponent).join('/'), {
      headers: {'Range': 'bytes=' + start + '-' + end, 'If-Match': entry.etag},
      signal: controller.signal, cache: 'no-store'
    }).then(async response => {
      if (response.status !== 206 || response.headers.get('ETag') !== entry.etag ||
          response.headers.get('Content-Range') !== 'bytes ' + start + '-' + end + '/' + entry.size)
        throw new Error('Original range identity mismatch');
      const bytes = new Uint8Array(await response.arrayBuffer());
      if (bytes.length !== size) throw new Error('Original range length mismatch');
      if (controller.signal.aborted) return;
      ++metrics.requests; metrics.bytesFetched += bytes.length;
      // A synchronous fallback may already have supplied the exact same bytes.
      if (!cache.has(key)) storeBlock(key, bytes, true);
    }).catch(failure => {
      if (failure.name !== 'AbortError') ++metrics.asyncErrors;
      // Prefetch failure never fabricates data or changes native read errors.
    }).finally(() => {
      if (pending.get(key) === controller) pending.delete(key);
      metrics.pendingAsyncBytes -= size;
    });
  }
  function range(entry, number) {
    const key = entry.path + ':' + number;
    const previous = lastDemandBlock.get(entry.path);
    if (previous !== number) {
      lastDemandBlock.set(entry.path, number);
      // Read ahead one block only after genuine sequential demand is observed.
      if (previous !== undefined && previous + 1 === number) prefetchSequentialBlock(entry, number + 1);
    }
    if (cache.has(key)) {
      const value = cache.get(key);
      cache.delete(key); cache.set(key, value);
      if (asyncBlocks.delete(key)) ++metrics.asyncHits;
      return value;
    }
    const start = number * blockSize;
    const end = Math.min(start + blockSize, entry.size) - 1;
    if (start < 0 || end < start) return error(28);
    if (pending.has(key)) pending.get(key).abort();
    const began = performance.now();
    let responseText, reason = '';
    // [rangeretry] 2026-10-24 phone probes: one failed sync range read (mobile network hiccup) became EIO and the
    // engine aborted ("RedIO: Failed to read ... bytes"). Retry the exact same range before reporting the error.
    for (let attempt = 0; attempt < 4 && responseText === undefined; ++attempt) {
      const request = new XMLHttpRequest();
      try {
        // libc filesystem calls may be proxied to the browser main thread.
        // Text XHR preserves binary bytes without unsupported synchronous arraybuffer.
        request.open('GET', entry.path.split('/').map(encodeURIComponent).join('/'), false);
        request.overrideMimeType('text/plain; charset=x-user-defined');
        request.setRequestHeader('Range', 'bytes=' + start + '-' + end);
        request.setRequestHeader('If-Match', entry.etag);
        request.send(null);
        const text = request.responseText;
        if (request.status !== 206 || request.getResponseHeader('ETag') !== entry.etag ||
            request.getResponseHeader('Content-Range') !== 'bytes ' + start + '-' + end + '/' + entry.size ||
            text.length !== end - start + 1) reason = 'status ' + request.status + ' length ' + text.length;
        else responseText = text;
      } catch (failure) { reason = String(failure && (failure.name + ': ' + failure.message) || failure); }
      if (responseText === undefined) console.error('CP2077 resource range read failed:', entry.path, start, end, 'attempt', attempt + 1, reason);
    }
    if (responseText === undefined) return error(29);
    const bytes = new Uint8Array(end - start + 1);
    for (let i = 0; i < bytes.length; ++i) bytes[i] = responseText.charCodeAt(i) & 255;
    storeBlock(key, bytes, false);
    ++metrics.requests; metrics.bytesFetched += bytes.length;
    metrics.syncReadMs += performance.now() - began;
    return bytes;
  }
  function mount(entry) {
    if (typeof entry.path !== 'string' || !/^\/game\/(archive\/pc\/|r6\/cache\/|engine\/shader_final\.cache$)/.test(entry.path) ||
        entry.path.includes('\\') || entry.path.includes(':') ||
        entry.path.split('/').some(part => part === '.' || part === '..') ||
        !Number.isSafeInteger(entry.size) || entry.size < 0 ||
        !Number.isFinite(entry.mtime) || typeof entry.etag !== 'string') {
      throw new Error('Invalid original resource file metadata');
    }
    const split = entry.path.lastIndexOf('/');
    const directory = entry.path.slice(0, split);
    FS.mkdirTree(directory);
    FS.createDataFile(directory, entry.path.slice(split + 1), new Uint8Array(0), true, false);
    const node = FS.lookupPath(entry.path).node;
    node.usedBytes = entry.size;
    node.atime = node.mtime = node.ctime = entry.mtime;
    node.node_ops = Object.assign({}, node.node_ops, {setattr: () => error(69)});
    node.stream_ops = Object.assign({}, node.stream_ops, {
      read(stream, destination, offset, length, position) {
        if (![offset, length, position].every(Number.isSafeInteger) || offset < 0 || length < 0 ||
            position < 0 || offset + length > destination.length) return error(28);
        const count = Math.min(length, Math.max(0, entry.size - position));
        let copied = 0;
        while (copied < count) {
          const at = position + copied;
          const bytes = range(entry, Math.floor(at / blockSize));
          const within = at % blockSize;
          const take = Math.min(count - copied, bytes.length - within);
          if (take <= 0) return error(29);
          destination.set(bytes.subarray(within, within + take), offset + copied);
          copied += take;
        }
        return copied;
      },
      write: () => error(69),
      allocate: () => error(69),
      mmap: () => error(43),
      msync: () => error(69)
    });
    ++metrics.files;
  }
  fetch('/runtime/game-files.json', {cache: 'no-store'})
    .then(response => {
      if (!response.ok) throw new Error('Original file manifest unavailable: ' + response.status);
      return response.json();
    })
    .then(manifest => {
      if (manifest.version !== 1 || !Array.isArray(manifest.files)) throw new Error('Invalid resource manifest');
      FS.mkdirTree('/game/archive/pc');
      FS.symlink('/game/archive', '/archive');
      for (const entry of manifest.files) mount(entry);
      console.log('CP2077 native resource filesystem mounted ' + metrics.files + ' actual files.');
      removeRunDependency(dependency);
    })
    .catch(failure => abort('Could not mount original game resources: ' + failure));
});
