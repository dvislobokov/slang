# macOS stub SDK

Minimal declarations of the macOS C headers that the Zell runtime uses,
written from Apple's public SDK headers (macOS 12+, arm64 and x86_64). They
exist only so that `make -C runtime CROSS=aarch64-apple-darwin` (or
`x86_64-apple-darwin`) can **compile** every runtime file for macOS on a
Linux machine without the real SDK, catching compile errors in the kqueue
poller and the `__APPLE__` branches of the platform layer.

- Objects built against these headers by the plain compile check are
  **never linked**: the build puts them in `build/<triple>-stubcheck/` and
  does not create the `build/<triple>/libzell_rt.a` that `zell build
  --target macos-*` looks for. Struct layouts and constant values follow
  the SDK (including the x86_64 `$INODE64` entry points of `stat` and
  `dirent`), but the compile check does not verify them.
- `make CROSS=<cpu>-apple-darwin STUBSDK=yes` builds a real runtime archive
  from them (`build/<triple>-stubsdk/`): `scripts/bundle-targets.sh` uses it
  for the toolchain bundle when it runs without an SDK (Linux), and CI runs
  executables cross-linked with it on the macOS runners. A release builds
  the macOS runtimes on a Mac against the real SDK instead.
- On a Mac (or with `SDKROOT` pointing at a `MacOSX.sdk`), the same make
  command uses the real SDK instead; `scripts/macos-verify.sh` builds and
  tests everything there.
- Only declarations the runtime needs are here. When a runtime change
  needs a new one, add it with the SDK's spelling and type.
