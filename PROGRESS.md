# Implementation progress

## 2026-07-21 — Baseline and Phase 1

- Initialized the Windows x64 C++20/CMake project around pinned CEF `150.0.14+g7c1aa68+chromium-150.0.7871.129`.
- Verified the standard CEF archive against published SHA-1 `626fb887a04e9c5668f13eae8a39c37880cb818e`.
- Added sandbox/bootstrap producer packaging and per-monitor-v2 manifests.
- Added typed protocol framing, bounded parsers, named-pipe I/O helpers, and logon-SID security helpers.
- Implemented accelerated CEF OSR import with adapter discovery, a four-slot private 4K texture ring, and callback-scoped `D3D11_QUERY_EVENT` completion.
- Added a D3D11 flip-model viewer shell and deterministic alpha/frame-stress fixtures.
- Debug and Release builds pass. Protocol unit tests pass.
- Runtime capture spike reached accelerated frame 270 at 3840×2160 while the launcher stayed responsive.

## 2026-07-21 — Phases 2 and 3

- Implemented a logon-SID-protected, local-only full-duplex named pipe using overlapped I/O.
- Added process-verified NT handle duplication, adapter-LUID selection, keyed-mutex texture slots, and fresh rings on reconnect.
- Added latest-frame publication, shared texture consumption, premultiplied-alpha checkerboard display, aspect-fit scaling, and local viewer copies.
- Added viewer URL/Back/Forward/Reload/Stop controls, mouse/wheel/keyboard/focus forwarding, toolbar toggle, and producer-controlled viewer visibility.
- Added main-view + `PET_POPUP` compositor code with premultiplied blending and clipped popup geometry.
- Added protocol, IPC, continuous frame, navigation, mouse-input, and reconnect tests.
- Debug E2E result: browser frames advanced in the viewer; viewer navigation reached CEF; viewer mouse click reached the test page; viewer kill/restart reconnected with a fresh shared ring.
- Known pinned-build result: the HTML select received the test click, but CEF M150 on this machine emitted only popup-hide and no popup-show callback. This is reported as a capability warning; popup composition code remains active for callbacks that are delivered.

## 2026-07-21 — Phase 4 and final validation

- Added basic Win32 IME composition/commit/finish forwarding and standard CEF cursor propagation.
- Added viewer aspect-fit, 1:1 pixel mode with Ctrl+Arrow panning, F11 fullscreen, and hideable toolbar.
- Added deterministic GPU alpha readback validation for opaque, 50%-alpha, and transparent pixels.
- Added DXGI local-memory budget diagnostics, Release soak testing, package generation, sandbox preparation, and packaged-binary validation.
- Final Debug and Release unit/IPC suites pass: protocol and concurrent overlapped pipe tests.
- Final Release E2E passes: continuous frames, producer visibility toggle, viewer navigation, webpage mouse input, viewer reconnect with a fresh ring.
- Final Release alpha probe passes.
- Final soak: 120 seconds at 30.1 observed fps (3,618 frames), then a final 60-second run at 30.4 fps (1,825 frames). No runtime error diagnostics; processes remained responsive.
- GPU budget on the validation machine: 5,234 MiB local budget, approximately 6 MiB used before workload allocation.
- Portable sandboxed package validated from the package directory. Archive size: 183,529,598 bytes.
- Known limitation: the select control receives viewer input, but CEF M150 on this machine does not emit a popup-show callback. Popup composition remains implemented for delivered `PET_POPUP` callbacks.
