# Streaming Browser — project context

## What this repository is for

This project prototypes a replacement for part of the Philips FlexVision system,
using JavaScript web apps for fast iteration. Background on the real system:

- The compositor PC contains **AVCC cards**: hardware that decodes incoming
  video-over-IP (VoIP) streams (HDMI outputs encoded onto Ethernet).
- One AVCC card runs in **compositor mode**: it takes a subset of the streams
  (max 8 of 16) and arranges them into one composited output stream.
- The composited stream is sent to the large 4K display, where VoIP is decoded
  back to HDMI.
- The composite itself is mostly bare video, so it receives an **overlay UI**
  (top bar, viewport headers, dose info, etc.). The viewport-manager web app in
  [tests/fixtures/viewport-manager.html](../tests/fixtures/viewport-manager.html)
  is a minimal version of that overlay.
- The overlay app is also responsible for sending **scaling/location data to
  the AVCC cards** (from presets and from users dragging viewport separators).
- The overlay app additionally routes **mouse/keyboard input** to the connected
  sources (KVM-like functionality through the AVCC cards).

## How the repo pieces map to that system

| Repo piece | Role in the FlexVision prototype |
| --- | --- |
| `src/producer/` (headless CEF) | Renders the overlay web app and streams it (premultiplied-alpha GPU frames) to the compositor |
| `tests/fixtures/viewport-manager.html` | The overlay web app (viewport layout, separators, presets) |
| `src/viewer/` | Generic debug viewer for the raw CEF stream |
| Compositor stub (planned, see [docs/compositor.md](../docs/compositor.md)) | Stand-in for the AVCC compositor: draws labeled source rectangles, then alpha-blends the CEF overlay on top |
| `experiments/usb-routing/`, `src/input_routing/`, `src/producer/network_input_server.*` | Active per-device USB HID routing experiment/ingress; not ready for compositor integration |

## Architectural rule: generic CEF, specific compositor

- Changes to the CEF producer/viewer/protocol must stay **generic and
  reusable** (nothing FlexVision-specific in `src/producer`, `src/common`, or
  the stream protocol).
- FlexVision-specific behavior (viewport layout semantics, AVCC emulation,
  source naming, KVM routing policy) belongs in the **compositor stub** and in
  the **web app / JS helper library**.
- The web app communicates layout to the compositor via a **WebSocket opened
  from the page itself** (not through the CEF stream protocol), which keeps the
  browser side untouched. See [docs/compositor.md](../docs/compositor.md).
- HID routing is an active but separate workstream. Follow
  [docs/device-routing.md](../docs/device-routing.md) and keep it out of the
  compositor implementation until the experiment's safety and transport gates
  pass. Layout JSON and HID binary routing use separate endpoints/protocols.

## Practical conventions

- Windows x64 / D3D11 only; build with `.\scripts\configure.ps1`,
  `.\scripts\build.ps1`, test with `.\scripts\test.ps1` and
  `.\scripts\test-e2e.ps1`.
- Web apps are static HTML/JS fixtures in `tests/fixtures/` — no bundler, no
  Node build step. They run at 3840×2160 design resolution with
  `color-scheme: dark` and (when overlaid) transparent backgrounds.
- The viewer renders D3D into a dedicated child HWND so native toolbar
  controls composite correctly above the flip-model swapchain.
