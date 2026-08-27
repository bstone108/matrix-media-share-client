# Matrix Media Share Client

Matrix Media Share Client is a desktop Matrix media browser, sharer, and downloader for Linux, Windows, and macOS.

For a plain-English walkthrough of the app, see [docs/USER_GUIDE.md](docs/USER_GUIDE.md).

It is seeded from Matrix Media Archiver, but this app is now oriented around:

- browsing room media instead of blindly archiving everything
- sharing files through Matrix plus IPFS
- publishing IPFS landing pages with alternate public gateway links
- keeping managed shared files plus a visible transfer queue
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

Build dependencies:

- Qt 6.10 with `Core`, `Gui`, `Widgets`, `Network`, `Sql`, `Multimedia`, `MultimediaWidgets`, and `Test`
- Rust 1.93 for the backend
- Go 1.25 if you want the build to compile the bundled Kubo/IPFS binary for you
- a bundled VLC runtime for in-app media playback

The build can discover a local VLC install on macOS automatically. On Linux and Windows, or in CI, pass a normalized VLC runtime root through `MATRIX_MEDIA_SHARE_CLIENT_BUNDLED_VLC_ROOT`. That root should contain:

- `lib/`
- `plugins/`

Packaging helpers:

- `scripts/package-macos.sh`
- `scripts/package-appimage.sh`
- `scripts/package-windows.ps1`

These scripts now expect the same bundled-VLC layout the GitHub workflow prepares for each platform.

## Versioning

`VERSION.txt` is the last **shipped** app version: `year.month.day.build` in America/Chicago (Central Time), with unpadded month and day. The first release on a given date is `.1`; later releases that same day increment the last component. A new date always starts over at `.1` and never continues the previous release's date.

Only bump `VERSION.txt` when cutting a GitHub Release. Pull requests, Desktop CI test packaging, and other non-release runs keep the last shipped version and must not consume a build number.

When cutting a release:

```bash
./scripts/next-version.py          # preview the next release version
./scripts/next-version.py --write  # update VERSION.txt, then tag v{version}
```

Release tags are `v{version}`, for example `v2026.8.24.1`. The Rust backend `Cargo.toml` version is independent and can stay `0.1.0`.

## GitHub Actions

The desktop workflow builds on pull requests that touch desktop sources, on `v*` tags, and on `workflow_dispatch`. Pull-request and other non-release compiles stay unsigned: they do not import the Developer ID certificate, do not set `MACOS_CODESIGN_IDENTITY`, and do not call `notarytool` or stapler.

It publishes GitHub releases from `v*` tags or `workflow_dispatch` after every platform build succeeds. Only those real releases Developer ID sign (hardened runtime + timestamp), notarize, and staple the macOS `.app`, the `.dmg`, and the zip. The zip of the `.app` is the notary vehicle (`ditto -c -k --keepParent`); the ticket is stapled onto the `.app`, and that stapled `.app` is what ships in the release zip. If the run was started from a `v*` tag, publish fails unless that tag matches `VERSION.txt`.

The workflow currently prepares platform runtimes like this:

- macOS: downloads the official VLC DMG and bundles the app runtime
- Windows x64/arm64: downloads the official VLC zip for each architecture and normalizes it into `lib/` + `plugins/`
- Linux x64/arm64: installs distro VLC packages and normalizes the runtime into `lib/` + `plugins/`

If you change the media viewer runtime layout, update both the packaging scripts and `.github/workflows/desktop-ci.yml` together.

## Notes

- The IPFS node is treated as part of the app from the user point of view.
- `dweb.link` is the bootstrap landing-page gateway.
- Alternate public gateways are rendered into each landing page.
- The browser/download workflow is in place, with deeper archive-dedupe and self-healing work still intended for later phases.
