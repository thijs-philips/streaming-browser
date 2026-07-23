# Compositor stub — architecture and implementation plan

## Goal

Build `streaming_compositor.exe`, a development stand-in for the AVCC
compositor path. The executable should show what the 4K display would receive:

1. a black composited-output canvas;
2. up to eight simulated video sources, placed and scaled by the overlay app;
3. the premultiplied-alpha CEF overlay drawn over those sources.

This lets the JavaScript viewport manager exercise realistic layout plumbing
before the AVCC wrapper and real VoIP sources are available.

```text
                                          graphics IPC
viewport-manager.html ─► headless CEF producer ───────────────┐
          │                                                    ▼
          └─ layout WebSocket ───────────────────► streaming_compositor.exe
                                                     │
                                         source blocks + CEF overlay
                                                     │
                                                     ▼
                                     windowed preview or fullscreen output
```

The stub owns no browser. It is a native D3D11 consumer of the headless CEF
stream, like the current debug viewer, with compositor-specific rendering and
layout ingress added.

## Non-negotiable boundary: generic CEF, specific compositor

The existing producer, graphics protocol, `StreamClient`, and debug viewer are
generic streaming-browser infrastructure. They must not learn about FlexVision,
AVCC source names, viewport limits, presets, or KVM routing policy.

FlexVision-specific code belongs in:

- `src/compositor/` — compositor window, layout server, state, and rendering;
- `tests/fixtures/compositor-client.js` — page-facing layout helper;
- `tests/fixtures/viewport-manager.html` — source assignments and layout policy.

The page opens a normal loopback WebSocket itself. CEF already provides the
browser WebSocket implementation, so layout control requires no custom V8
binding, CEF scheme handler, or extension to the graphics named-pipe protocol.

If a generic browser-to-host application-message API is independently needed
later, it can be designed as a separate feature. It is not a prerequisite for
this compositor.

## What can actually be reused

### Reuse unchanged

- `src/viewer/stream_client.*`: graphics handshake, shared-handle reception,
  frame release, reconnect, and stream-generation reset. The compositor uses it
  as the producer's single graphics client; ordinary window input may continue
  through its existing generic commands for divider-drag development, but this
  path is not the per-device HID-routing design.
- `src/common/protocol.*`, pipe I/O/security, and logging.
- The D3D11 adapter-selection, shared-ring import, keyed-mutex copy, and
  swap-chain patterns currently implemented by the viewer renderer.
- The viewer's windowed/fullscreen transition pattern.

### Refactor before sharing

`D3DRenderer` is currently `final`, owns the checkerboard and frame-composition
shaders, and exposes none of its D3D resources. It therefore cannot simply be
reused unchanged for compositor drawing.

Extract generic frame consumption and presentation into a library target under
`src/viewer/` or `src/presentation/`, for example:

```text
FrameConsumer
  imports ring resources on the producer's adapter
  copies a received slot into a local shader-resource texture
  releases the keyed mutex safely

PresentationSurface
  owns the D3D11 device/context and flip-model swap chain
  resizes and presents on an HWND
  exposes the local overlay SRV and the current frame viewport
```

The debug viewer keeps its checkerboard renderer. The compositor receives a
separate `CompositorRenderer` built on the shared lower-level pieces. Do not
make a FlexVision subclass of the current renderer and do not duplicate the
ring synchronization code.

The current dedicated child render-surface HWND remains a viewer concern: it
allows native toolbar siblings to appear above a flip-model swap chain. The
compositor has no toolbar, so its swap chain may target its own full client
surface.

## Output and coordinate contract

Layout rectangles use normalized coordinates relative to the **entire CEF
overlay/output surface**, not relative to the `.viewport-grid` alone:

```text
origin = top-left of the 3840 × 2160 overlay
x      = left / overlayWidth
y      = top / overlayHeight
width  = viewportWidth / overlayWidth
height = viewportHeight / overlayHeight
```

This is important because the overlay includes the top bar and left system rail.
The compositor must leave those regions black and place simulated video only in
the transparent viewport openings. No toolbar/rail constants belong in the
compositor configuration or C++ code.

The web app is the source of truth. After applying its CSS grid, it computes
each assigned viewport from DOM geometry:

```js
const output = document.documentElement.getBoundingClientRect();
const rect = viewport.getBoundingClientRect();
const normalized = {
  x: (rect.left - output.left) / output.width,
  y: (rect.top - output.top) / output.height,
  width: rect.width / output.width,
  height: rect.height / output.height
};
```

This automatically includes the toolbar, rail, divider pixels, persisted
layouts, keyboard resizing, presets, and future UI geometry changes. A
`ResizeObserver` sends a fresh full snapshot when the page viewport changes.

The first compositor demo should run the producer in fixed/client scaling at
3840 × 2160. Server-side responsive scaling is a later test case; it must keep
the CEF frame dimensions, DOM-reported coordinates, and compositor output
aspect ratio aligned.

## Page-facing layout API

Add a small, standalone `compositor-client.js`. The app makes one call per
logical viewport change:

```js
const compositor = CompositorClient.connect({
  url: 'ws://127.0.0.1:8765/layout/v1'
});

compositor.setViewport({
  viewportId: 'lower-left',
  sourceId: 'live-xray',
  label: 'Live X-ray',
  rect: { x, y, width, height }
});

compositor.removeViewport('lower-left');
```

Use separate stable identifiers:

- `viewportId` identifies the destination rectangle and survives source
  reassignment;
- `sourceId` is the stable machine-facing source identity that the future AVCC
  wrapper maps to hardware;
- `label` is display-only text for the stub.

Do not use a mutable human-readable title as the primary key.

Version 1 describes the destination rectangle only. Source crop, aspect-fill,
rotation, z-order, and source-catalog validation are intentionally absent until
the real AVCC wrapper requirements are known; add them through a versioned
extension rather than assigning hidden meaning to the rectangle fields.

The helper owns the authoritative current map. It coalesces repeated updates
per `viewportId`, but the app still performs one API call whenever its logical
state changes. At the next animation frame the helper sends one atomic, complete
snapshot of the current map. Moving one separator changes multiple adjacent
rectangles; sending them together prevents transient gaps or overlaps in the
native compositor.

It also sends a complete snapshot:

- immediately after WebSocket connection/reconnection;
- after a preset or bulk source-assignment change;
- after a page/output resize.

Disconnected updates overwrite the helper's current map rather than building
an unbounded queue of stale drag positions.

## Layout WebSocket protocol v1

Use text JSON for this low-rate control plane. This is deliberately separate
from the binary, latency-sensitive HID routing protocol.

Endpoint and subprotocol:

```text
ws://127.0.0.1:8765/layout/v1
Sec-WebSocket-Protocol: flexvision-layout.v1
```

Message from page to compositor:

```jsonc
{
  "type": "layout",
  "protocol": 1,
  "revision": 12,
  "viewports": [
    {
      "viewportId": "lower-left",
      "sourceId": "live-xray",
      "label": "Live X-ray",
      "x": 0.101,
      "y": 0.516,
      "width": 0.247,
      "height": 0.484
    }
  ]
}
```

Version 1 deliberately has one page-to-compositor message type: an atomic full
`layout` snapshot. `setViewport()` and `removeViewport()` are convenient local
API operations on the helper's map, not incremental wire messages. With at most
eight entries, a full JSON snapshot at drag-frame rate is still a small control
stream and is much easier to recover and validate correctly.

Messages from compositor to page:

```jsonc
{ "type": "hello", "protocol": 1, "maxViewports": 8 }
{ "type": "applied", "protocol": 1, "revision": 12 }
{ "type": "error", "protocol": 1, "code": "invalid-layout" }
```

Rules:

- Accept exactly one active layout-control connection. Reject additional peers.
- Bind only to numeric loopback (`127.0.0.1` initially); create no firewall rule.
- Require the path and WebSocket subprotocol above. Validate the handshake
  `Origin` against an explicit local allowlist (including the exact value
  observed for the pinned CEF `file://` page) as defense in depth, but do not
  treat Origin as general authentication.
- Reject unknown message types and unknown object fields.
- Bound text messages (for example 64 KiB), labels/IDs, update rate, and parse
  depth before allocating state.
- Require finite numbers, `x/y >= 0`, `width/height > 0`, and
  `x + width <= 1`, `y + height <= 1` with a small floating-point tolerance.
- Require unique `viewportId` and no more than eight active viewports. Repeated
  `sourceId` is a policy decision; v1 permits it so mirroring can be tested.
- Require each snapshot to contain the complete authoritative set, including an
  empty array when all blocks should be cleared.
- Apply monotonically increasing revisions per connection. Ignore stale or
  duplicate revisions. A reconnect starts a new connection revision epoch and
  must begin with a full `layout` snapshot; do not compare the new page's
  revisions with a disconnected page instance.
- Parse on the WebSocket thread, then post one immutable full state snapshot to
  the window/render thread. Rendering state is single-writer; the D3D context is
  never touched from the socket thread.
- Keep the last valid layout when the page disconnects. The helper's mandatory
  full snapshot on reconnect re-establishes authority. Do not clear the display
  just because the control socket briefly reconnects. If the page reconnects
  with an intentionally empty snapshot, clear the blocks.

Implementation uses Boost.Beast/Asio, already fetched by the root build. Follow
its one-read/one-write-at-a-time rule with one I/O thread/strand and a bounded
write queue. Disable compression, use `TCP_NODELAY`, set read/message/idle
limits, and shut down without blocking the window thread.

Use Boost.JSON from the already pinned, hash-verified Boost 1.90 archive. Compile
its implementation once in the compositor layout-protocol target (for example
with one translation unit including `boost/json/src.hpp`) and place its licence
in the package. Schema strictness, nesting limits, string limits, and numeric
validation remain application responsibilities. Do not parse the protocol with
ad-hoc string searching.

## Compositor rendering

The renderer runs on the same D3D11 adapter selected by the producer ring.
Rendering order for each present:

1. Clear the output to opaque black.
2. Draw one opaque placeholder rectangle per active viewport, including a
   subtle border and deterministic per-source color/pattern.
3. Draw each source label centered in its block.
4. Draw the local CEF overlay texture over the whole output using
   premultiplied-alpha blending:

```text
SrcBlend  = ONE
DestBlend = INV_SRC_ALPHA
```

Do not reuse the viewer's checkerboard frame shader: it manually composites the
overlay over a checker and forces alpha to one. The compositor needs a normal
blend state so transparent overlay pixels reveal the source blocks.

Direct2D/DirectWrite can render the labels directly against a DXGI swap-chain
surface if the required BGRA/D2D interop is initialized. Cache text formats and
brushes; source labels change rarely. If D2D interop proves disproportionately
complex, use a small cached glyph texture atlas, not per-frame CPU rasterization.

Device-dependent text resources must be recreated after a producer adapter/ring
change. Keep layout data and cached label strings device-independent so they
survive that recreation.

Layout changes must trigger a present even when CEF has produced no new frame.
CEF frame arrival, layout-state arrival, window expose/resize, and reconnect all
set one `needs_render` flag; a window timer or render loop coalesces them.

the selected monitor. Reject unsupported output/overlay aspect-ratio mismatch
In client-scaling windowed/fit mode, blocks and overlay use the same letterboxed
content viewport. In server-scaling mode, a newly resized window may briefly
have only the previous CEF frame available. During that transition, stretch
the old frame and normalized source-block layout independently in X and Y to
fill the current client area; do not preserve the stale frame's aspect ratio or
show transient black bars. When CEF delivers a frame whose metadata dimensions
match the requested client size, presentation becomes exact 1:1 pixels. In
borderless fullscreen, map the full logical output to the selected monitor.

The stub only draws placeholders. It does not decode video, simulate AVCC
latency, or produce another VoIP stream in the first implementation.

## Window and configuration

Follow the repository's current split-config convention: each executable owns a
strict root-level YAML file. Do not add a `compositor:` top-level section to a
shared producer/viewer document.

Example `config/compositor.example.yaml`:

```yaml
websocket:
  bind_address: 127.0.0.1
  port: 8765
  path: /layout/v1

output:
  width: 3840
  height: 2160

window:
  width: 1440
  height: 810
  maximized: false
  fullscreen: false
  monitor: 0
  scaling: server
```

Windowed mode uses `WS_OVERLAPPEDWINDOW`. Fullscreen uses borderless `WS_POPUP`
on the configured monitor. `maximized` and `fullscreen` are mutually exclusive.
The compositor has no menu, URL bar, navigation commands, or debug-viewer
checkerboard. F11 toggles borderless fullscreen. `window.scaling` selects the
startup mode (server by default) and F12 toggles it at runtime: client mode
restores the configured logical output size, while server mode continuously
reports the compositor's exact client size to CEF for 1:1 pixels. The producer keeps the shared ring allocated at the
3840×2160 maximum and copies each CEF frame into its top-left sub-rectangle;
per-frame metadata carries the live content size, so sub-4K server-side resizes
never create a new ring generation or reset the stream. DOM resize observation
sends a fresh normalized layout snapshot independently.

`output.width` and `output.height` define the logical composition coordinate
space, not an independent texture-resampling target. For the first milestone
they must equal the accepted overlay ring dimensions; reject a mismatch clearly.

The producer configuration for the demo must set:

- the viewport-manager file URL;
- `force_transparency: true` (the current page has an opaque black body and is
  not yet a usable alpha overlay without this or equivalent transparent CSS);
- `viewer_visible: true` if that flag remains the producer-side output gate;
- fixed 3840 × 2160 viewport for the first milestone. `scaling: client` belongs
  to the debug-viewer configuration and is not a producer setting.

Before native composition, make the web app explicitly transparent in overlay
mode instead of depending only on forced CSS injection. The current opaque
`.application`, toolbar, workspace, rail, grid, and viewport backgrounds also
need an overlay-mode audit: only actual UI chrome should remain opaque, while
source-video openings must carry zero alpha. A standalone-browser preview mode
may add a black backdrop without changing the overlay geometry.

The launcher starts the compositor first (so the layout WebSocket is listening),
then the producer. The compositor's `StreamClient` already retries graphics-pipe
connection, and the page helper retries its WebSocket with bounded exponential
backoff and jitter. The launcher must stop both process trees cleanly and report
port conflicts and early process exit.

## Graphics connection and lifecycle constraints

- The producer supports one active graphics consumer. The compositor replaces
  the debug viewer for a demo; do not run both simultaneously. The producer
  already rejects an additional active connection.
- `StreamClient` reconnects and handles `kStreamReset` by reconnecting. The
  compositor must recreate adapter-dependent render/D2D resources whenever a
  new ring generation is accepted.
- Release each received shared slot immediately after the local GPU copy
  completes, as the viewer does now. Never hold a keyed-mutex slot while waiting
  for layout or presentation.
- Validate `RingDefinition.alpha_mode == premultiplied` and the expected BGRA/RGBA
  format before accepting the generation.
- A layout can arrive before the first overlay frame and should already render
  placeholder blocks. Conversely, an overlay frame can arrive before the page's
  layout socket connects and should render over black.
- The compositor ignores producer navigation/status/visibility callbacks that
  only exist for the debug viewer. Its own configured window mode controls
  visibility; the producer's `viewer_visible` flag remains only the generic
  stream-publication gate until that naming is independently cleaned up.
- For development, mouse/keyboard input received by the compositor preview
  window is mapped through the same letterboxed content viewport and forwarded
  to CEF using the existing generic `StreamClient` input commands. This keeps
  divider dragging and keyboard resizing usable without coupling the layout
  WebSocket to input. It is separate from per-device HID/KVM routing.
- Page reload, compositor restart, producer restart, WebSocket reconnect, monitor
  change, and window resize are expected lifecycle transitions, not fatal errors.

## HID routing boundary — active experiment, not compositor scope yet

HID routing is being developed separately in `experiments/usb-routing/`,
`src/input_routing/`, and the disabled-by-default experimental producer ingress
in `src/producer/network_input_server.*`. It already has a binary, versioned
routing protocol and an experimental Boost.Beast WebSocket path targeting
`/input/v1` on port 17831. It is not ready to integrate into the compositor
plan.

Therefore:

- do not implement HID capture, route ownership, event forwarding, or KVM
  visualization as part of compositor phases;
- do not reuse the JSON layout socket for HID events;
- do not change the generic graphics protocol for HID routing;
- preserve a future integration seam: stable `viewportId`/`sourceId` assignments
  from the layout state can later map a page routing decision to an AVCC KVM
  destination.

The authoritative input design remains `docs/device-routing.md`. Integration is
revisited only after the USB-routing experiment passes its device identity,
safety, reconnect, held-state, and transport gates.

## Implementation phases and gates

### Phase 0 — prove the contracts in a normal browser

- Make the viewport overlay explicitly support transparent-overlay and black
  standalone-preview modes, including transparent viewport openings rather than
  only transparent `html`/`body`.
- Implement `compositor-client.js`, stable IDs, DOM-to-full-output coordinates,
  revisioning, coalescing, and reconnect snapshots.
- Run a small local test WebSocket server (preferably reusing the production
  layout protocol/server code in a test host) and verify with Playwright that
  mouse, keyboard, reset, persisted layout, presets, and responsive resize
  produce the expected full-output rectangles.

**Gate:** browser tests prove exact coordinates (including top bar and rail), no
unbounded disconnected queue, and one update per viewport per animation frame.

### Phase 1 — shared graphics consumer and stripped window

- Refactor viewer frame-import/presentation primitives into a reusable library.
- Add a compositor executable with strict config, windowed and borderless
  fullscreen modes, black clear, and premultiplied overlay rendering.
- Keep the existing viewer behavior and tests unchanged.

**Gate:** the compositor consumes the real CEF ring, shows the transparent
overlay over black, survives producer restart/ring generation changes, and the
generic viewer E2E suite still passes.

### Phase 2 — layout server and placeholder blocks

- Add the strict loopback Beast server and JSON protocol parser/state model.
- Post immutable layout snapshots to the window thread.
- Render bounded blocks, borders, and labels below the overlay.
- Add change-driven presents for both CEF frames and layout updates.

**Gate:** a Playwright divider drag moves blocks in the native compositor with
pixel agreement against DOM rectangles; malformed/flooding peers cannot alter
state or exhaust memory.

### Phase 3 — lifecycle, packaging, and deployment behavior

- Add `config/compositor.example.yaml` and `scripts/run-compositor-demo.ps1`.
- Package the compositor, config, JS helper, HTML fixture, and Boost/json licence
  obligations where applicable.
- Qualify windowed, maximized, fullscreen, monitor selection, reconnect, page
  reload, producer/compositor startup in either order, and clean shutdown.
- Run a 4K30 soak with divider activity and report GPU/CPU/memory use.

**Gate:** automated unit/integration/E2E tests pass in Debug and Release, and a
60-minute 4K30 interactive soak has no stuck ring slots, socket growth, device
loss, or compositor/page drift.

### Future — real AVCC wrapper

Replace placeholder rendering with AVCC source selection/scaling while
preserving the page-facing layout protocol where practical. The hardware wrapper
will own hardware source mapping, AVCC validation/limits, and KVM output. It must
not require FlexVision behavior in the generic CEF producer.

HID integration is a separate future workstream after its experiment gates pass.

## Expected file impact

Likely new files:

```text
src/compositor/
  CMakeLists.txt
  compositor_main.cc
  compositor_configuration.h/.cc
  compositor_renderer.h/.cc
  layout_protocol.h/.cc
  layout_server.h/.cc

tests/fixtures/compositor-client.js
config/compositor.example.yaml
scripts/run-compositor-demo.ps1
tests/compositor_configuration_tests.cc
tests/compositor_layout_protocol_tests.cc
tests/compositor_layout_server_tests.cc
scripts/test-compositor-e2e.ps1
```

Likely refactors:

```text
src/viewer/CMakeLists.txt
src/viewer/d3d_renderer.*
src/viewer/stream_client.*        # target/library ownership, not semantics
src/common/configuration.*        # share strict YAML helpers only if useful
src/CMakeLists.txt
tests/CMakeLists.txt
scripts/package.ps1
tests/fixtures/viewport-manager.html
```

The current packager copies only `tests/fixtures/*.html`; it must explicitly
copy `compositor-client.js` (and any future fixture assets) as part of Phase 3.

No planned FlexVision changes:

```text
src/producer/browser_client.*
src/producer/d3d_frame_pipeline.*
src/common/protocol.*
```

Generic fixes discovered while implementing may still land there, but must be
specified and tested without compositor terminology or policy.

## Validation matrix

### Unit

- compositor YAML defaults, bounds, conflicts, loopback-only binding, and unknown
  key rejection;
- JSON schema, UTF-8, finite/bounded coordinates, unique IDs, max eight,
  revisions, unknown fields, oversized/deep input;
- layout state apply/snapshot/remove/stale-revision behavior;
- normalized-to-pixel conversion and clipping at 3840 × 2160.

### Integration

- WebSocket path/subprotocol/origin checks, second-client rejection, reconnect,
  heartbeat/idle timeout, bounded queue, clean stop;
- producer ring import, keyed-mutex copy/release, stream reset, adapter resource
  recreation, premultiplied alpha;
- layout update without new CEF frame triggers a present.

### E2E

- launch compositor then producer and the reverse order;
- drag each divider through the CEF input path and compare DOM rectangles with
  compositor state/rendered edges;
- reset/preset, persisted reload, page reload, WebSocket interruption, producer
  restart, compositor restart;
- window resize, fit letterboxing, fullscreen monitor selection;
- visual alpha cases: opaque toolbar/header, translucent element, fully
  transparent viewport revealing a source block;
- regular viewer regression (`scripts/test-e2e.ps1`) and compositor 4K30 soak.

## Decisions closed by this pass

- WebSocket from the page is the layout transport; no CEF-specific bridge.
- Coordinates are full-output normalized DOM rectangles, not grid-relative
  fractions and not C++ toolbar/rail offsets.
- Stable viewport/source IDs are separate from display labels.
- The compositor is the sole graphics consumer during a run.
- Layout JSON and HID binary routing remain separate protocols/endpoints.
- HID work remains experimental and outside compositor implementation.
- Config follows the current per-executable root-level YAML convention.
- Generic frame import/presentation is refactored; FlexVision rendering is not
  added to the generic viewer.

## Decisions still requiring evidence during implementation

- Whether Direct2D/DirectWrite interop or a cached glyph atlas is simpler and
  more robust for placeholder labels.
- Exact CEF/file-page `Origin` value and WebSocket handshake behavior on the
  pinned CEF build.
- Target-monitor selection semantics (index versus stable display identity).
- Whether the future AVCC wrapper can adopt this JSON protocol directly or will
  need an adapter at the compositor boundary.
