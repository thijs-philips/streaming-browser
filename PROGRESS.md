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
- Portable sandboxed package validated from the package directory. Archive size: approximately 175 MiB.
- Known limitation: the select control receives viewer input, but CEF M150 on this machine does not emit a popup-show callback. Popup composition remains implemented for delivered `PET_POPUP` callbacks.

## 2026-07-22 — Performance regression fix: publication feedback loop

- Symptom: laptop heat, and viewer latency growing to seconds over time; producer frame counter advanced smoothly while viewer frame numbers skipped chunks.
- Measured (idle stress page, Release): ~1,320 FrameReady messages/sec on the pipe instead of the expected ~30; viewer burned 7.6 CPU-seconds in under a minute.
- Root cause: `PublishLatest()` was invoked both on every new CEF frame and from `ReleaseOutputSlot()` on every viewer release. Because publication always republished `latest_metadata_` even when that frame ID had already been sent, each release triggered a new publication of the same frame, whose release triggered another, saturating all four keyed-mutex slots with duplicate 4K copies + pipe messages. The loop amplified until GPU/pipe queues backed up, which surfaced as growing input-to-photon latency.
- Fix: track `published_frame_id_` in `D3DFramePipeline` and skip publication when `latest_metadata_.frame_id` has already been delivered; reset it whenever a fresh output ring/generation is created so reconnects still receive the latest frame.
- Validated: ~30 msgs/sec after the fix, viewer CPU 0.19 s over the same window, unit + IPC + E2E tests pass, and a 90 s Release soak held 30.0 fps (producer peak 153 MiB, viewer peak 68 MiB).

## 2026-07-22 — Idle CPU/GPU reduction in the viewer

- Profiled per-process GPU engine counters and CPU deltas with the animated stress page: viewer GPU was the largest app consumer after DWM, and per-frame `SetWindowText` title updates forced non-client repaints 30×/sec.
- Change 1: the viewer 16 ms timer now presents only when a new frame arrived or the view changed (dirty flag covers frames, resize, pan, 1:1 toggle, ring reopen, visibility restore); `WM_PAINT` still renders on demand so occlusion/uncover stays correct.
- Change 2: title-bar frame counter updates are throttled to ~1 Hz (every 30th frame), preserving the `frame N` pattern the E2E/soak scripts poll.
- Change 3: a hidden viewer window skips presents entirely and re-renders on the next show.
- Result: static-page idle now measures 0.00 viewer CPU-seconds per 10 s and no measurable viewer GPU engine utilization; animated 4K page still holds 30.0 fps in a 60 s soak and E2E passes.
