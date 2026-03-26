This package is the Qt and Rust source tree for Matrix Media Share Client, trimmed for ARM build work.

Included entry point:
- `build-linux-arm64.command`

What the script does:
- runs natively on Arch Linux arm64/aarch64
- installs the needed Arch packages with `pacman`
- sets up Rust with `rustup`
- builds bundled Kubo/IPFS with Go when a prebuilt binary is not provided
- builds the Qt app plus Rust backend
- packages a Linux `aarch64` AppImage zip into `out/`

Expected output after a successful run:
- `out/MatrixMediaShareClientQt-2026.3.25.6-linux-aarch64-appimage.zip`

What is not automated here:
- Windows ARM64 packaging from this Mac host

Notes:
- Linux ARM64 packaging also needs a normalized VLC runtime root with `lib/` and `plugins/` if you want in-app playback bundled into the AppImage.
- The GitHub workflow prepares that runtime automatically on Ubuntu ARM runners.
