# CP2077 native browser engine port

Original sources and assets are referenced from `D:\cp2077SourceCode\extracted\root\R6.Root\Mainline`. Adaptations are staged through `engine-overlay` and a Clang filesystem overlay; extracted original files remain intact.

The original launcher and game state machine are being compiled for WebAssembly. The game does not yet link or run in a browser. See MODLOG.md for measured outcomes, UTF-16 limitations and remaining platform work.

Build with `./build-browser.ps1`. The strict link step currently fails on unfinished engine dependencies. Compiled source/library artifacts are not proof of a playable game or 1:1 behavior.

The selected source-build outcomes are listed in MODLOG.md. The UTF-16 adapter preserves the two-byte game text layout and avoids the selected incompatible wide libc buffer calls. Original protocol and type-registration tools are built from source. Original grammar parser generation, archive, renderData and FreeType compilation are included in the build chain.

`serve-engine-assets.py --depot PATH --port 8078` serves original archive/engine/r6 resources under /game/ and compiled files under /runtime/. It does not start the game. Browser filesystem integration, cooked runtime assets and additional engine/platform dependencies are still required.
