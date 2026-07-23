# FlexVision compositor and input-routing prototype — July 2026

## What was added

The repository now includes a demoable native stand-in for the AVCC compositor:

```text
viewport-manager.html
  ├─ CEF GPU overlay ─► named-pipe shared texture ring ─► streaming_compositor.exe
  └─ layout JSON ─────► ws://127.0.0.1:8765/layout/v1 ──► source block state

streaming_compositor.exe renders:
  black output → six labeled simulated sources → premultiplied-alpha CEF overlay
```

The generic CEF graphics protocol remains free of FlexVision semantics. Source
IDs, destination rectangles, the eight-viewport limit, and labels live in the
compositor/layout layer and JavaScript helper.

The existing custom-device routing spike is also wired into a demoable but
experimental path:

```text
Raw Input capture probe
  → binary streaming-browser-input.v1 WebSocket on 127.0.0.1:17831
  → producer route-state/safety adapter
  → existing CEF input APIs
```

This path is disabled by default, loopback-only, and remains separate from the
JSON layout protocol.

## Run the compositor demo

Build once:

```powershell
.\scripts\configure.ps1
.\scripts\build.ps1 -Configuration Release
```

Run:

```powershell
.\scripts\run-compositor-demo.ps1 -Configuration Release
```

The launcher starts the compositor first, then CEF with the viewport manager in
transparent overlay mode. Drag any viewport separator in the compositor window.
The six simulated source blocks move beneath the overlay. Press F11 to toggle
borderless fullscreen. Press F12 to toggle between client scaling (fixed
3840×2160 CEF rendering, scaled by the compositor) and server scaling (CEF
renders at the compositor's exact current client size for 1:1 pixels).

In server scaling, every compositor-window resize is reported to the producer
immediately. The producer keeps its shared graphics ring permanently allocated
at the 3840×2160 maximum and only resizes the CEF view; each frame is copied
into the top-left sub-rectangle of the ring and per-frame metadata carries the
live content size. Consumers sample just that sub-rectangle, so resizing never
tears the ring down, never resets the stream, and tracks the window as smoothly
as resizing a regular browser. Until the matching CEF frame arrives, the
compositor stretches the previous frame independently in X and Y to fill the
new client area; it deliberately does not preserve the stale aspect ratio.
The page's `ResizeObserver` reports fresh
normalized DOM rectangles over the separate layout WebSocket, so source blocks
continue to track headers, margins, and separators after the responsive page
resize. Press F12 again to restore the fixed 3840×2160 client-scaling view.

Run the automated compositor checks:

```powershell
.\scripts\test-compositor-e2e.ps1 -Configuration Release
.\scripts\test-compositor-soak.ps1 -Configuration Release -DurationSeconds 60
```

## Run the experimental custom-input demo

Build the capture experiment:

```powershell
.\experiments\usb-routing\run.ps1 -Configuration Release
```

That command starts an interactive capture probe after building/testing it. Use
Q to stop it. To run the dedicated routing fixture later:

```powershell
.\scripts\run-input-routing.ps1 -Configuration Release
```

The probe prompts for activation of the intended physical USB keyboard/mouse.
The emergency stop is Ctrl+Alt+Shift+F12, entirely on the routed keyboard.
For a hardware-independent smoke test:

```powershell
.\scripts\run-input-routing.ps1 -Configuration Release -Synthetic
```

To expose the same optional input endpoint while running the compositor:

```powershell
.\scripts\run-compositor-demo.ps1 -Configuration Release -EnableInputRouting
.\build\usb-routing\bin\Release\usb_routing_probe.exe --synthetic
```

Physical device qualification, suppression, KVM destination selection, and
per-viewport route ownership are not claimed complete. See docs/device-routing.md.

## Configuration guide

### config/producer.compositor.yaml

- `url`: overridden by the launcher with the local viewport-manager file URL.
- `force_transparency`: ensures transparent page backgrounds reach the GPU ring.
- `viewer_visible`: currently acts as the generic stream-publication gate.
- `viewport.width/height`: fixed 3840×2160 compositor coordinate space.
- `frame_rate`: CEF's maximum accelerated frame rate.
- `network_input`: disabled for the compositor-only demo.

### config/producer.compositor-input.yaml

Same producer settings, with the experimental binary input endpoint enabled at
`127.0.0.1:17831`.

### config/compositor.example.yaml

- `websocket.bind_address/port/path`: loopback JSON layout endpoint, default
  `127.0.0.1:8765/layout/v1`.
- `output.width/height`: logical output and required overlay-ring dimensions.
- `window.width/height`: restored development-window size.
- `window.maximized`: start with normal bordered window maximized.
- `window.fullscreen`: start borderless fullscreen; mutually exclusive with
  maximized.
- `window.monitor`: reserved monitor index for deployment selection.

## Layout API

The viewport page loads `tests/fixtures/compositor-client.js` and makes one call
per logical viewport change:

```js
compositor.setViewport({
  viewportId: 'lower-left',
  sourceId: 'live-xray',
  label: 'Live X-ray',
  rect: { x, y, width, height }
});
```

Coordinates are normalized against the complete CEF document, so toolbar and
system-rail offsets are represented without native constants. The helper sends
one atomic full layout snapshot per animation frame, retries with bounded
backoff, and replaces disconnected updates instead of accumulating them.

## Validation completed

- Debug build and seven Debug tests passed.
- Release build and seven Release tests passed.
- Existing generic viewer E2E passed, including navigation/input/reconnect.
- Compositor E2E passed: a native-window divider drag advanced the applied
  layout revision.
- Synthetic custom-input path delivered key/mouse events and released route
  state through the complete WebSocket → producer → CEF path.
- Portable package 0.2.0 built and passed its packaged-demo test.
- Short 30-second compositor soak passed; compositor working set remained near
  71 MiB. A longer target-hardware soak is still required before deployment.

## Important boundaries

- One producer graphics consumer is supported: use either the debug viewer or
  the compositor, not both.
- Layout JSON and HID binary input use different ports and protocols.
- The compositor draws placeholders; it does not decode VoIP or wrap AVCC yet.
- Real AVCC source mapping, crop/fill/rotation semantics, KVM routing, and
  hardware safety policy remain future adapter work.
- Input capture is non-exclusive; selected devices still affect Windows.

## Original streaming-browser implementation report

## Result

The Windows x64 implementation is operational end to end:

1. CEF renders a transparent 3840×2160 windowless browser at a maximum of 30 fps.
2. The producer imports CEF's temporary D3D11 resource, copies it into owned textures, and waits for callback-scoped GPU completion.
3. Final premultiplied-alpha frames are published through a four-slot, keyed-mutex D3D11 shared ring.
4. A separate viewer discovers the producer through a logon-SID-protected local pipe, opens the ring on the matching DXGI adapter, copies each frame locally, and presents over a checkerboard.
5. Browser navigation and page input travel back to CEF over the duplex control protocol.

## Implemented capabilities

- Pinned and hash-verified CEF M150 distribution.
- Sandboxed CEF bootstrap/DLL packaging with LPAC ACL preparation.
- 4K accelerated OSR with transparent background and optional root CSS forcing.
- Callback-safe cross-device CEF texture copies using `D3D11_QUERY_EVENT` completion.
- Hardware-adapter discovery and adapter-LUID negotiation.
- Main-view and popup-layer compositor with premultiplied blending.
- Versioned, bounded binary protocol over concurrent overlapped named-pipe I/O.
- Per-logon local security, remote-client rejection, client PID discovery, and NT handle duplication.
- Keyed-mutex output slots, critical frame/release messages, latest-frame coalescing, and fresh rings after reconnect.
- Interactive viewer with URL, Back, Forward, Reload/Stop, toolbar toggle, aspect-fit, 1:1 panning, fullscreen, alpha checkerboard, cursor changes, mouse, wheel, keyboard, focus, and basic IME forwarding.
- Producer Visible viewer checkbox and headless capture mode.
- Single-instance CEF cache behavior and second-launch handling.
- Structured runtime log and GPU memory-budget diagnostics.

## Automated validation

| Validation | Result |
| --- | --- |
| Debug build | Passed |
| Release build | Passed |
| Protocol serialization tests | Passed |
| Concurrent overlapped pipe test | Passed |
| Opaque/50%/transparent GPU alpha probe | Passed |
| Continuous producer-to-viewer 4K frames | Passed |
| Viewer frame progression | Passed |
| Producer hide/show viewer control | Passed |
| Viewer URL navigation to CEF | Passed |
| Viewer mouse click reaching webpage | Passed |
| Viewer kill/restart and fresh-ring reconnect | Passed |
| Release 120-second soak | 3,618 frames; 30.1 fps |
| Final Release 60-second soak | 1,825 frames; 30.4 fps |
| Portable packaged demo | Passed |

The soak reported peak process working sets of approximately 130 MiB for the producer and 70 MiB for the viewer. GPU-local budget was 5,234 MiB on the validation adapter. Process working set does not include all CEF/driver GPU allocations.

## Package

- Directory: `out/streaming-browser-0.1.0-windows-x64`
- Archive: `out/streaming-browser-0.1.0-windows-x64.zip`
- Archive size: approximately 175 MiB
- Run `prepare-sandbox.ps1` after extraction, then `run-demo.ps1`.

For development, run `scripts/run-demo.ps1` after building.

## Known limitations

- On the validation machine, Chromium M150 receives the click on the HTML select control but does not emit a `PET_POPUP` show callback; only popup-hide was observed. The compositor supports popup textures when CEF supplies them.
- IME composition and commit forwarding are implemented, but candidate-window positioning is not automated in the validation suite.
- Custom bitmap cursors fall back to a standard cursor; standard CEF cursor types are mapped.
- The custom stream is a local shared-GPU protocol, not a byte-serializable file or network format.
- One viewer and one browser stream are supported.
- Binaries are not code-signed.
- HDR, audio transport, downloads, permission UI, file-dialog proxying, clipboard, touch, accessibility bridging, macOS, and Linux are outside this first version.

## Git milestones

- `1198c6c` — CEF accelerated capture spike.
- `dc3b948` — shared D3D viewer and interactive transport.
- `b8e5a29` — final hardening, automated validation, packaging, and report.
