#!/usr/bin/env bash
# wasm32: archive + link -> post-link stagers -> package engine-versions/<sha> -> upload to R2. Prints the sha last.
set -e
PY="/d/Cyber punk/tools/emsdk/python/3.13.3_64bit/python.exe"
cd /d/2077
"$PY" work/w32/link-w32.py > build-w32/link-run.log 2>&1 || { tail -40 build-w32/link-run.log; exit 1; }
grep -q '"Unresolved": 0' build-w32/link-result.json || { echo "unresolved symbols"; head -40 build-w32/unresolved-symbols.json; exit 1; }
"$PY" work/w32/post-link-w32.py > build-w32/post-link.log 2>&1 || { cat build-w32/post-link.log | head -20; exit 1; }
# [w32-tablecompact] hot (profiled) table entries first, via the link relocations; drops linking/reloc sections
node --max-old-space-size=12000 work/w32/table-compact-w32.mjs build-w32/game/cp2077.wasm build-w32/opt/compact.wasm work/w32/hot-names.txt > build-w32/opt/table-compact.log 2>&1   || { cat build-w32/opt/table-compact.log; exit 1; }
cp build-w32/opt/compact.wasm build-w32/game/cp2077.wasm
# [w32-opt] wasm-opt -Os (static ctors never inlined, one-caller inlining capped: keeps every function small for JSC)
if [ "${CPWEB_NO_WASMOPT:-0}" != "1" ]; then
  mkdir -p build-w32/opt && cp build-w32/game/cp2077.wasm build-w32/opt/in.wasm
  BINARYEN_CORES=3 "/d/Cyber punk/tools/emsdk/upstream/bin/wasm-opt.exe" build-w32/opt/in.wasm --no-inline='_GLOBAL__*' --no-inline='__cxx_global_var_init*'     --no-inline='__wasm_call_ctors' -Oz -ocimfs=400 -g --enable-threads --enable-simd --enable-bulk-memory --enable-nontrapping-float-to-int     --enable-sign-ext --enable-mutable-globals --enable-multivalue --enable-reference-types -o build-w32/opt/out.wasm > build-w32/opt/wasm-opt.log 2>&1     || { cat build-w32/opt/wasm-opt.log | head; exit 1; }
  cp build-w32/opt/out.wasm build-w32/game/cp2077.wasm
fi
# [w32-lazytable] empty funcref table, filled per instance on first use (JSC per-instance wrapper cost)
node --max-old-space-size=8192 work/w32/lazy-table-w32.mjs build-w32/game/cp2077.wasm build-w32/opt/lazy.wasm "$(cat build-w32/opt/compact.wasm.hotend)" > build-w32/opt/lazy-table.log 2>&1   || { cat build-w32/opt/lazy-table.log; exit 1; }
cp build-w32/opt/lazy.wasm build-w32/game/cp2077.wasm
# [w32-nonames] iOS bundle without the function-name section (set CPWEB_KEEP_NAMES=1 for a diagnostic build)
if [ "${CPWEB_KEEP_NAMES:-0}" != "1" ]; then
  "$PY" work/w32/strip-names-w32.py build-w32/game/cp2077.wasm build-w32/opt/nonames.wasm > build-w32/opt/strip-names.log 2>&1 || { cat build-w32/opt/strip-names.log; exit 1; }
  cp build-w32/opt/nonames.wasm build-w32/game/cp2077.wasm
fi
SHA=$("$PY" work/w32/package-w32-bundle.py | tail -1)
(cd cloudflare && node scripts/upload-bundle.mjs "$SHA" | tail -2)
echo "SHA $SHA"
