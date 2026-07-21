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

## Remaining work

1. Compose main view and `PET_POPUP` into final shared textures.
2. Implement discovery/session pipes and handle duplication.
3. Implement keyed-mutex shared-ring state machine and reconnect generations.
4. Display producer frames in the viewer.
5. Add browser toolbar, input forwarding, IME, visibility controls, and diagnostics.
6. Expand unit/integration/end-to-end tests and run soak validation.
7. Package a clean portable release and produce the final report.
