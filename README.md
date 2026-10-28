# CP2077 browser port

Current browser-port source and Cloudflare streaming configuration.

## Main files

- native-browser.html: browser entry page.
- src/: current browser adapters and rendering integration.
- engine-overlay/ and generated/: engine adaptations and generated source needed by the build.
- Root build, compile and stage scripts: build pipeline.
- streaming/: streaming integration.
- cloudflare/: Worker, R2 delivery scripts, configuration and website files.
- runtime/: small runtime configuration and shader-cache inputs.

The Cloudflare Worker serves engine bundles and game assets from R2. The repository does not bundle the compiled engine Wasm/data, game asset depot, SDK installations, node_modules, test browser profiles, logs or old .bak copies. No Cloudflare deployment or R2 objects were changed by this repository cleanup.

A few large current generated source headers and website files have an extra .gz extension. Run `node restore-compressed.cjs` after cloning to restore their original filenames and verify their hashes. This does not download game assets.

SOURCE-MANIFEST.json records the preserved files and original SHA-256 hashes. The historical README and handoff notes contain older measurements; they do not establish current runtime behavior.

## Preservation and provenance

The full earlier snapshot remains locally preserved separately. Deletion of the original D: project had already begun before it was stopped; this repository cannot guarantee completeness against a pre-deletion state. Further deletion remains paused.

Original source attribution is retained. This project incorporates adaptations to proprietary engine code; repository access does not grant rights to that code or the game assets. This repository alone is not a self-contained playable game or build environment.
