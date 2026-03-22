# Matrix Media Share Client

Matrix Media Share Client is a desktop Matrix media browser, sharer, and downloader for Linux, Windows, and macOS.

It is seeded from Matrix Media Archiver, but this app is now oriented around:

- browsing room media instead of blindly archiving everything
- sharing files through Matrix plus IPFS
- publishing IPFS landing pages with alternate public gateway links
- keeping a local library and a visible transfer queue
- managing a bundled, app-owned Kubo node from inside the client

## Current Implementation

- Qt desktop shell renamed to `MatrixMediaShareClientQt`
- Rust backend renamed to `matrix_media_share_client_backend`
- bundled-Kubo lifecycle manager with startup, readiness checks, and shutdown
- public gateway registry and gateway health probing
- Matrix upload-limit detection through the homeserver media config API
- first-pass share flow:
  - publish file to IPFS
  - generate a landing page
  - send Matrix upload when the file fits the detected limit
  - fall back to preview-plus-link behavior when it does not
- `Rooms`, `Browser`, `Library`, `Transfers`, `Settings`, and `Verification` pages

## Build

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Backend tests can also be run directly with:

```bash
PATH=/opt/homebrew/opt/rustup/bin:$PATH /opt/homebrew/opt/rustup/bin/cargo test --manifest-path backend/Cargo.toml
```

## Notes

- The IPFS node is treated as part of the app from the user point of view.
- `dweb.link` is the bootstrap landing-page gateway.
- Alternate public gateways are rendered into each landing page.
- The browser/download workflow is in place, with deeper archive-dedupe and self-healing work still intended for later phases.
