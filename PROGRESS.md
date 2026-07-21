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

## Remaining hardening

1. Add basic IME commit/composition forwarding and cursor state display.
2. Run Release unit/E2E and sustained soak tests with resource measurements.
3. Add portable package generation and verify from a clean extracted directory.
4. Produce final validation report and commit the remaining milestones.
