# About high Res-high-framerate rendering:

Yes. The key idea is to intercept the webpage **after Chromium has composited it**, but before—or instead of—presenting it to a monitor.

A useful mental model is:

```text
HTML / CSS / JavaScript
          │
          ▼
        Blink
  layout, paint records
          │
          ▼
   cc compositor
 layers, tiles, animations
          │
          ▼
   CompositorFrameSink
 CompositorFrames + GPU resources
          │
          ▼
      Chromium Viz
 surface aggregation, compositing
          │
          ▼
     Final GPU texture
          │
     ┌────┴─────────┐
     ▼              ▼
Display/swapchain   Your capture pipeline
                    │
                    ▼
            colour conversion
                    │
                    ▼
              encoder or
             custom format
```

## What a Chromium “frame sink” actually is

A Chromium `CompositorFrameSink` is not simply a framebuffer containing final pixels. It is an interface through which renderer compositors submit `CompositorFrame` objects to Chromium’s Viz compositing service.

Those frames contain:

* Render passes and draw quads.
* References to GPU or shared-memory resources.
* Damage rectangles.
* Surface identifiers.
* Timing and frame acknowledgement information.

Viz receives frames from the main page, out-of-process iframes, browser UI and other surfaces, then combines them into a final root surface. Chromium describes Viz as the implementation for compositing and GPU presentation, while its frame-sink component organizes the relationships between submitted frames and surfaces. ([Chromium Git Repositories][1])

So there are two fundamentally different interception points:

```text
Renderer frame sink                  Root/output surface
───────────────────                  ───────────────────
Partial page surface                 Fully composited page
May contain references               Actual final image
May omit iframes/popups               Best capture target
Not directly an RGBA bitmap          Suitable for video/export
```

For most applications, you want the **final Viz output or CEF’s off-screen-rendered result**, not an individual renderer’s `CompositorFrameSink`.

# Option 1: CEF software off-screen rendering

The Chromium Embedded Framework provides a supported off-screen rendering API.

You create a windowless browser and implement:

```cpp
class RenderHandler : public CefRenderHandler {
 public:
  void GetViewRect(
      CefRefPtr<CefBrowser> browser,
      CefRect& rect) override {
    rect = CefRect(0, 0, 1920, 1080);
  }

  void OnPaint(
      CefRefPtr<CefBrowser> browser,
      PaintElementType type,
      const RectList& dirty_rects,
      const void* buffer,
      int width,
      int height) override {
    // Copy BGRA pixels into your own buffer or queue.
  }
};
```

CEF’s current API specifies that `OnPaint` supplies a complete `width × height × 4` BGRA image with an upper-left origin, together with dirty rectangles identifying changed areas. The callback is used when shared-texture rendering is disabled. ([cef-builds.spotifycdn.com][2])

The pipeline is:

```text
Chromium/Viz
    │
    ▼
GPU or software compositor
    │
    ▼
GPU-to-CPU readback, if GPU rendered
    │
    ▼
CEF OnPaint(BGRA buffer)
    │
    ├── copy dirty rectangles
    ▼
Your CPU frame queue
    │
    ├── raw/custom serialization
    ├── PNG/JPEG compression
    └── software video encoder
```

### Advantages

It is relatively easy to implement, cross-platform and produces an ordinary CPU-accessible buffer.

### Limitations

The pixel bandwidth becomes significant:

```text
1920 × 1080 × 4 = 8,294,400 bytes/frame

30 fps ≈ 249 MB/s
60 fps ≈ 498 MB/s
```

That does not include the compositor readback, memory copies, format conversion or encoding.

Also, the callback should do very little work. CEF render-handler methods run on its UI thread. A good implementation copies the changed pixels into a preallocated buffer, places that buffer in a queue and returns immediately. ([cef-builds.spotifycdn.com][2])

This route is adequate for 1080p30 and can sometimes handle 1080p60, but it is not the ideal architecture for sustained high-resolution capture.

# Option 2: CEF accelerated off-screen rendering

This is usually the most practical high-performance solution.

Instead of receiving CPU pixels through `OnPaint`, enable shared-texture rendering and implement:

```cpp
void OnAcceleratedPaint(
    CefRefPtr<CefBrowser> browser,
    PaintElementType type,
    const RectList& dirty_rects,
    const CefAcceleratedPaintInfo& info) override;
```

CEF supplies a platform-native shared GPU resource:

| Platform | Resource supplied by CEF                   |
| -------- | ------------------------------------------ |
| Windows  | Shared D3D11/D3D12 texture handle          |
| macOS    | `IOSurface`                                |
| Linux    | Native-buffer planes with file descriptors |

CEF explicitly warns that these handles come from a pool. They can change each frame, cannot be retained after the callback, and should be opened and copied into a texture owned by your application before returning. ([cef-builds.spotifycdn.com][2])

The resulting pipeline is approximately:

```text
Chromium renderer
        │
        ▼
Chromium GPU process
        │
        ▼
Viz final composited texture
        │
        ▼
CEF shared texture callback
        │
        ▼  GPU-to-GPU copy
Your texture ring
        │
        ▼  GPU shader
BGRA/RGBA → NV12 or P010
        │
        ▼
Hardware video encoder
        │
        ▼
H.264 / HEVC / AV1 packets
        │
        ▼
Your custom container or network protocol
```

The important distinction is:

```text
Software OSR:
GPU → CPU readback → CPU copy → encoder

Accelerated OSR:
GPU texture → GPU texture copy → GPU encoder
```

The accelerated path is not literally zero-copy because CEF requires you to copy the temporary pooled texture into your own resource. However, it can remain entirely on the GPU, avoiding the expensive GPU-to-CPU readback.

## Example Windows pipeline

On Windows, the callback can follow this pattern:

```cpp
void RenderHandler::OnAcceleratedPaint(
    CefRefPtr<CefBrowser> browser,
    PaintElementType type,
    const RectList& dirty_rects,
    const CefAcceleratedPaintInfo& info) {

  HANDLE shared_handle = info.shared_texture_handle;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> cef_texture;

  HRESULT result = d3d_device1_->OpenSharedResource1(
      shared_handle,
      IID_PPV_ARGS(&cef_texture));

  if (FAILED(result)) {
    return;
  }

  TextureSlot* slot = texture_ring_.TryAcquire();
  if (!slot) {
    // Drop this frame rather than blocking Chromium's UI thread.
    return;
  }

  d3d_context_->CopyResource(
      slot->bgra_texture.Get(),
      cef_texture.Get());

  slot->timestamp = MonotonicTimestamp();
  slot->state = TextureState::ReadyForConversion;

  encoder_thread_.Wake();
}
```

A worker thread then performs:

```text
BGRA texture
     │
     ▼
Compute/pixel shader
     │
     ▼
NV12 texture
     │
     ▼
NVENC, Media Foundation, D3D12 Video, etc.
```

NVIDIA’s Video Codec SDK exposes NVENC hardware encoding on Windows and Linux, including interoperability with DirectX surfaces. ([NVIDIA Developer][3])

On macOS, the equivalent architecture uses the supplied `IOSurface`, a client-owned Metal/CoreVideo buffer and VideoToolbox. VideoToolbox provides low-level access to hardware video encoders and operates with CoreVideo pixel buffers. ([Apple Developer][4])

On Linux, CEF can provide native-buffer planes as file descriptors. A GPU/media pipeline can import these through DMA-BUF-capable APIs; DMA-BUF is specifically designed to share buffers between devices and subsystems using file descriptors and synchronization fences. ([cef-builds.spotifycdn.com][2])

# Option 3: Chromium’s internal FrameSink video capturer

Chromium itself already contains a capture pipeline called `FrameSinkVideoCapturer`.

It exists because Chrome needs to capture composited tabs and surfaces for features such as tab sharing and screen capture.

Conceptually:

```text
Viz FrameSink / Surface
          │
          ▼
FrameSinkVideoCapturer
          │
          ▼
CopyOutputRequest
          │
          ├── asynchronous GPU readback
          ├── shared-memory frame pool
          └── mappable SharedImage frame pool
          │
          ▼
media::VideoFrame
          │
          ▼
Capture consumer
```

Chromium’s Viz documentation says that this component captures frames sent to the compositing service and produces a stream of video frames, using asynchronous GPU readback through `CopyOutputRequest`. The current source also includes both mappable shared-image and shared-memory video-frame pools. ([Chromium Git Repositories][1])

This is close to what you are describing, but it is an **internal Chromium Mojo API**, not an API exposed by ordinary Chrome, CDP, Playwright or Puppeteer.

Using it generally means:

1. Building Chromium or a custom Chromium-based shell.
2. Finding the `FrameSinkId` or capture target for your page.
3. Creating a Viz `FrameSinkVideoCapturer`.
4. Providing a Mojo video-frame consumer.
5. Receiving frames from its buffer pool.
6. Returning frame-consumption callbacks promptly.
7. Passing the resulting `media::VideoFrame` to your exporter or encoder.

A simplified conceptual consumer looks like:

```cpp
class CaptureConsumer
    : public viz::mojom::FrameSinkVideoConsumer {
 public:
  void OnFrameCaptured(
      media::mojom::VideoBufferHandlePtr buffer,
      media::mojom::VideoFrameInfoPtr info,
      const gfx::Rect& content_rect,
      mojo::PendingRemote<
          viz::mojom::FrameSinkVideoConsumerFrameCallbacks> callbacks)
      override {

    // Import/map the supplied buffer.
    // Queue it to encoder or custom frame writer.
    // Signal completion through callbacks when finished.
  }
};
```

### Why use it?

It lets you capture the **root composited surface**, including child surfaces and out-of-process iframes, and it integrates naturally with Chromium’s own video-frame and buffer-pool infrastructure.

### Why not use it?

It ties your application to Chromium internals. Mojo interfaces, ownership rules, build files and Viz internals can change between Chromium versions.

This is a reasonable route for a product already maintaining a Chromium fork, but probably excessive for a standalone webpage renderer.

# Option 4: Replace or intercept the Viz output surface

The deepest integration is to modify Chromium where Viz renders the final scene into its `OutputSurface`.

The normal architecture is roughly:

```text
SurfaceAggregator
       │
       ▼
AggregatedRenderPasses
       │
       ▼
SkiaRenderer
       │
       ▼
SkiaOutputSurface
       │
       ▼
GPU backbuffer / swapchain
       │
       ▼
Operating-system compositor
```

Instead of presenting to a real window, a custom implementation could render into a pool of SharedImages:

```text
SkiaRenderer
       │
       ▼
Custom off-screen OutputSurface
       │
       ▼
SharedImage texture pool
       │
       ├── custom pixel consumer
       ├── WebRTC/video consumer
       └── hardware encoder
```

Chromium’s Shared Image infrastructure exists to let graphics resources be shared between APIs and components. A backing may be a D3D texture, `IOSurface`, Vulkan image, GL texture or shared-memory allocation, with API-specific representations for Skia, Dawn/WebGPU and other consumers. ([Chromium Git Repositories][5])

This gives maximum control over:

* Pixel format.
* Buffer allocation.
* Synchronization.
* Colour space.
* Alpha handling.
* Frame timing.
* Hardware encoder interoperability.
* Whether frames are ever presented to a display.

But it is also the most invasive option because it touches Chromium’s Viz/GPU architecture and potentially platform-specific output-surface implementations.

I would only choose it when you need something CEF cannot provide, such as:

* Direct rendering into application-allocated encoder surfaces.
* Exact ownership of every frame buffer.
* A specialized non-display GPU backend.
* Sub-frame or multi-surface capture.
* Very low latency at 4K60 or beyond.
* Integration into an existing graphics engine.

# Frame timing and `BeginFrame`

Chromium’s compositor is normally driven by `BeginFrame` signals, conceptually equivalent to virtual vertical-sync ticks:

```text
BeginFrame @ t = 0 ms
    │
    ├── update animations
    ├── run compositor work
    └── submit frame or report no frame

BeginFrame @ t = 16.67 ms
    │
    └── next 60 Hz slot
```

A compositor may respond that it did not produce a frame when nothing changed. Chromium’s compositor-frame-sink interfaces include both `SetNeedsBeginFrame` and `DidNotProduceFrame`, reflecting this demand-driven behaviour. ([Chromium Git Repositories][6])

That means a capture pipeline should distinguish between:

```text
Browser frame cadence:
Only frames when visual content changes

Output stream cadence:
Exactly 30, 50, 60 or another fixed rate
```

For an exact 60 fps output stream, the capture controller should maintain its own timeline:

```cpp
for each output timestamp:
    if a new Chromium frame is available:
        emit new frame
    else:
        emit previous frame again
```

For deterministic offline rendering, you can instead control page time and compositor ticks:

```text
Set logical time to 0.000 s → render → capture
Set logical time to 0.016667 s → render → capture
Set logical time to 0.033333 s → render → capture
...
```

The generation may take longer than real time, but the resulting stream has exact timestamps.

# Buffering and backpressure

A practical accelerated pipeline should use at least three independently owned textures:

```text
Texture A: being filled by Chromium copy
Texture B: being colour-converted
Texture C: being encoded
Texture D: available
```

A four- or five-buffer ring is often more tolerant of temporary encoder stalls.

Each slot should have states such as:

```cpp
enum class FrameState {
  Available,
  Copying,
  ReadyForConversion,
  Converting,
  ReadyForEncoding,
  Encoding
};
```

Do not block the CEF callback waiting for the encoder. Choose an explicit overload policy:

| Policy             | Result                                          |
| ------------------ | ----------------------------------------------- |
| Drop newest        | Preserves queued frames, increases latency      |
| Drop oldest        | Keeps latency low; usually best for live output |
| Duplicate previous | Maintains constant output cadence               |
| Block producer     | Risks stalling Chromium; usually undesirable    |
| Render offline     | No drops, but may run slower than real time     |

For interactive or streaming use, **drop oldest and preserve timestamps** is generally the safest strategy.

# Writing your custom frame format

Your post-compositor pipeline can store either raw textures, raw CPU pixels or encoded video packets.

A practical record format might be:

```text
File header
──────────────────────────────────────
magic
format version
canvas width
canvas height
nominal frame rate
pixel or codec format
colour space
time base

Per-frame record
──────────────────────────────────────
frame index
presentation timestamp
duration
flags
damage rectangle count
damage rectangles
payload length
payload
```

For raw GPU-origin frames:

```text
pixel format: BGRA8 / RGBA8 / NV12 / P010
payload: raw planes, optionally compressed
```

For encoded frames:

```text
codec: H.264 / HEVC / AV1
flags: keyframe / delta frame
payload: encoded access unit
```

You can also preserve Chromium-specific metadata:

```text
navigation ID
page URL hash
device scale factor
scroll position
viewport size
capture timestamp
logical webpage timestamp
damage rectangles
dropped-frame count
```

# What I would build

For a product needing **1080p60 or possibly 4K60 webpage rendering**, I would use:

```text
CEF accelerated OSR
        │
        ▼
OnAcceleratedPaint
        │
        ▼
GPU-to-GPU copy into 4-texture ring
        │
        ▼
GPU colour conversion to NV12/P010
        │
        ▼
Hardware encoder or custom GPU consumer
        │
        ▼
Custom container
```

That gives most of the performance benefits of modifying Chromium without carrying a Chromium fork.

I would move to `FrameSinkVideoCapturer` or a custom Viz output surface only when the requirements include exact control over Chromium surfaces, internal timing, custom allocations or capture behaviour that CEF’s accelerated OSR cannot provide.

One important deployment detail is that headless Chromium does not always imply hardware-accelerated rendering. Chromium’s current documentation says GPU use must be enabled/configured appropriately; on Linux, backend and display-server configuration can affect whether hardware rendering is available. ([Chromium Git Repositories][7])

[1]: https://chromium.googlesource.com/chromium/src/%2Bshow/HEAD/components/viz/README.md "components/viz/README.md - chromium/src - Git at Google"
[2]: https://cef-builds.spotifycdn.com/docs/150.0/classCefRenderHandler.html "Chromium Embedded Framework (CEF): CefRenderHandler Class Reference"
[3]: https://developer.nvidia.com/video-codec-sdk?utm_source=chatgpt.com "Video Codec SDK | NVIDIA Developer"
[4]: https://developer.apple.com/documentation/videotoolbox?utm_source=chatgpt.com "Video Toolbox | Apple Developer Documentation"
[5]: https://chromium.googlesource.com/chromium/src/%2B/refs/heads/main/gpu/command_buffer/service/shared_image/README.md "Shared Image Infrastructure"
[6]: https://chromium.googlesource.com/chromium/src/%2B/refs/heads/main/components/viz/service/frame_sinks/compositor_frame_sink_impl.h?utm_source=chatgpt.com "components/viz/service/frame_sinks/compositor_frame_sink_impl.h ..."
[7]: https://chromium.googlesource.com/chromium/src/%2B/HEAD/docs/gpu/using-gpu-hardware-in-headless-chrome.md "Chromium Docs - docs/gpu/using-gpu-hardware-in-headless-chrome.md"




# About alpha blending (with Viz)

## 1. Final RGBA output with transparency

Viz supports `CopyOutputRequest` with an `RGBA` result format. In system memory, Chromium returns an `N32Premul` bitmap; on the GPU it can return an RGBA texture through a mailbox/shared-image mechanism. “Premul” means the RGB channels have already been multiplied by alpha. ([Chromium Git Repositories][1])

Conceptually:

```text
DOM elements
    │
    ▼
Blink paint operations
    │
    ▼
Compositor layers and render passes
    │
    ▼
Viz aggregation and blending
    │
    ▼
Transparent RGBA render target
    │
    ▼
RGBA8 premultiplied frame
```

For this to work, the complete chain must remain transparent:

1. Chromium’s root output surface must have an alpha channel.
2. The surface must be cleared to transparent rather than opaque white or black.
3. The browser background must be transparent.
4. The webpage must not paint an opaque `html` or `body` background.
5. The copy/capture destination must be RGBA rather than I420 or NV12.

With CEF off-screen rendering, transparent painting is enabled by setting the browser or global background colour to fully transparent. `OnPaint` then returns the complete frame as a four-byte-per-pixel BGRA image. ([CEF Builds][2])

For example:

```cpp
CefSettings settings;
settings.windowless_rendering_enabled = true;
settings.background_color = CefColorSetARGB(0, 0, 0, 0);

CefBrowserSettings browser_settings;
browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0);
```

And the page must allow transparency:

```css
html,
body {
  background: transparent !important;
}
```

The resulting frame might look like:

```text
Pixel 0: BGRA = 0,   0,   0,   0       fully transparent
Pixel 1: BGRA = 0,   0, 128, 128       50% transparent red, premultiplied
Pixel 2: BGRA = 255, 255, 255, 255      opaque white
```

## 2. The result is normally premultiplied alpha

Chromium’s CPU-side RGBA copy result is explicitly described as `N32Premul`. ([Chromium Git Repositories][3])

For a straight-alpha pixel:

```text
R = 255
G = 100
B = 0
A = 128
```

the premultiplied representation is approximately:

```text
R = 128
G = 50
B = 0
A = 128
```

The premultiplied compositing operation is efficient:

```text
Cout = Csource + Cdestination × (1 − Asource)

Aout = Asource + Adestination × (1 − Asource)
```

I would keep the frames premultiplied throughout your pipeline and record that fact in the format metadata:

```text
pixel_format = BGRA8
alpha_mode  = PREMULTIPLIED
color_space = SRGB
```

Only convert to straight alpha at an external boundary that specifically requires it. Unpremultiplication amplifies rounding errors in nearly transparent pixels:

```cpp
if (alpha != 0) {
  straight_red = premultiplied_red * 255 / alpha;
}
```

## 3. What information survives flattening?

Suppose the page contains:

```html
<div id="background"></div>
<div id="overlay" style="opacity: 0.5"></div>
```

Viz produces the visual result:

```text
background pixels
      +
overlay pixels × 0.5
      ↓
one resulting RGBA pixel
```

After that operation, you generally cannot reconstruct:

* Which element produced the colour.
* Whether transparency came from CSS `opacity`.
* Whether it came from a partially transparent PNG.
* Whether it came from an antialiased edge.
* Whether it came from a shadow or blur.
* Whether a parent opacity was applied to a complete group.
* Which elements were hidden behind other elements.
* How a blend mode contributed to the output.

Flattening is a many-to-one operation.

For example, the same resulting pixel might have come from:

```text
one 50%-transparent red element

or

two overlapping 29.3%-transparent red elements

or

an opaque red element beneath a 50%-transparent mask
```

The final RGBA stream preserves **net pixel coverage relative to the transparent canvas**, not the history of how that coverage was generated.

## 4. `FrameSinkVideoCapturer` versus RGBA capture

Chromium’s documentation says ordinary `CopyOutputRequest` is intended for occasional captures and recommends `FrameSinkVideoCapturer` for video capture. ([Chromium Git Repositories][4])

However, Chromium’s normal video path is designed around formats such as I420 and NV12:

```text
Y plane
U/V chroma planes
no alpha plane
```

That makes it efficient for video encoding, but transparency is lost.

Therefore, for a continuous transparent stream you would need one of these:

### Practical CEF route

```text
CEF accelerated off-screen rendering
        │
        ▼
transparent shared BGRA texture
        │
        ▼
GPU-to-GPU copy to owned texture
        │
        ▼
RGBA custom frame stream
```

CEF’s accelerated callback exposes a platform-native shared texture or native buffer. The handle must be reopened and copied during the callback because CEF returns it to an internal pool afterward. ([CEF Builds][5])

### Custom Viz route

Modify or supplement `FrameSinkVideoCapturer` so it requests or renders:

```text
RGBA8 instead of I420/NV12
```

and delivers an RGBA shared image or mappable RGBA buffer to your consumer.

### Dual-plane route

For better compression:

```text
Colour stream: NV12/H.264/HEVC/AV1
Alpha stream:  R8 grayscale
```

The final frame is reconstructed as:

```text
RGBA = decoded RGB + decoded alpha
```

This is often more efficient than encoding raw RGBA:

```text
Colour:
1920 × 1080 NV12 ≈ 3.1 MB/frame uncompressed

Alpha:
1920 × 1080 R8 ≈ 2.1 MB/frame uncompressed

RGBA:
1920 × 1080 RGBA8 ≈ 8.3 MB/frame uncompressed
```

It also lets the colour plane use an ordinary hardware video encoder while the alpha plane is losslessly or near-losslessly compressed separately.

## 5. Becoming aware of individual rendered elements

There are several levels of element awareness.

### Level A: final pixel alpha only

Output:

```text
RGBA frame
```

You know that a pixel is 40% opaque, but not why.

This is straightforward with transparent CEF OSR or a Viz RGBA copy.

### Level B: DOM metadata alongside RGBA

Instrument the webpage and collect:

```json
{
  "elementId": "dialog-shadow",
  "tag": "div",
  "bounds": [250, 100, 800, 500],
  "computedOpacity": 0.6,
  "visibility": "visible",
  "transform": [1, 0, 0, 1, 0, 0],
  "zIndex": 12
}
```

Your frame record becomes:

```text
Frame
├── RGBA pixels
├── presentation timestamp
└── DOM element metadata
```

This is useful, but not pixel-exact. A DOM element may:

* Produce several paint fragments.
* Become several compositor layers.
* Share a layer with other elements.
* Render through canvas, WebGL or video.
* Include pseudo-elements.
* Be clipped or occluded.
* Be flattened into a parent effect layer.

So DOM metadata describes intent and geometry, but not necessarily the actual pixel contribution.

### Level C: separate element or group mattes

Render important elements into separate transparent passes:

```text
Pass 1: background
Pass 2: person
Pass 3: labels
Pass 4: controls
Pass 5: shadows
```

Each pass produces:

```text
RGBA layer + position + blend mode
```

This gives compositing-software-style output, but it requires controlling the webpage or compositor. Re-rendering the whole page once per element is usually too expensive.

A practical compromise is to identify a small number of semantic groups:

```html
<div data-capture-layer="background">...</div>
<div data-capture-layer="main-content">...</div>
<div data-capture-layer="overlay">...</div>
```

Then capture perhaps three to ten separate RGBA passes rather than hundreds of DOM elements.

### Level D: Viz render-pass awareness

Viz receives a hierarchy closer to:

```text
CompositorRenderPass
├── SharedQuadState
│   ├── transform
│   ├── opacity
│   ├── clip
│   └── blend mode
├── TextureDrawQuad
├── SolidColorDrawQuad
├── SurfaceDrawQuad
└── RenderPassDrawQuad
```

A custom Viz consumer could preserve:

* Render-pass hierarchy.
* Quad opacity.
* Quad transforms.
* Blend modes.
* Clip rectangles.
* Texture/resource references.
* Surface relationships.
* Damage regions.

You would effectively create a recording format resembling:

```text
FRAME
├── PASS 0
│   ├── QUAD 0
│   ├── QUAD 1
│   └── QUAD 2
├── PASS 1
│   └── QUAD 3
└── ROOT PASS
    ├── PASS 0 reference
    └── PASS 1 reference
```

This is much richer than RGBA, but Viz quads do not necessarily correspond one-to-one with DOM elements. One element may generate multiple quads, and multiple elements may be rasterized into one texture.

## 6. A parallel element-ID buffer

For accurate per-pixel element awareness, the most robust custom solution is a second render target.

```text
Normal attachment:
RGBA8_PREMULTIPLIED

Identification attachment:
R32_UINT element-or-layer ID
```

The compositor performs two related renders:

```text
Colour pass                   ID pass
───────────                   ───────
normal colours                write ID 381
opacity and effects           same clipping
normal transformations        same transformations
RGBA output                   integer-ID output
```

Example:

```text
RGBA pixel:       [80, 30, 10, 127]
Element-ID pixel: 381
```

A metadata table then maps:

```json
{
  "381": {
    "domId": "notification-panel",
    "opacity": 0.5,
    "bounds": [480, 50, 320, 120]
  }
}
```

There is a design decision for overlapping transparency:

* Store the topmost contributing ID.
* Store the element with the largest contribution.
* Store multiple IDs per pixel.
* Render a separate mask for every semantic layer.
* Store an index into a per-tile contribution table.

A single `R32_UINT` ID buffer cannot represent several simultaneously contributing translucent elements. For that you need multiple mattes, an A-buffer-style structure, or a limited list of contributors per tile/pixel.

## Recommended architecture

For **transparent webpage video that will later be composited elsewhere**, I would use:

```text
CEF accelerated off-screen rendering
        │
        ▼
transparent premultiplied BGRA texture
        │
        ▼
copy to client-owned GPU texture
        │
        ├── retain RGBA directly
        │
        └── optionally split:
             ├── colour → NV12 encoder
             └── alpha  → R8 encoder
        │
        ▼
custom timestamped frame container
```

Frame metadata:

```text
width
height
timestamp
duration
pixel format
alpha mode: premultiplied
colour space
damage rectangles
colour payload
alpha payload, if separate
```

For **awareness of a manageable number of page components**, add explicitly designated capture layers and output a separate RGBA stream or alpha matte for each layer.

For **complete compositor-level inspection**, modify Viz to retain render-pass and quad metadata and optionally generate an element-ID attachment. That is substantially more work, but it is the path that genuinely preserves information beyond the flattened final image.

[1]: https://chromium.googlesource.com/chromium/src/%2B/723d8f2375772b3fe3695e7a65a3cc9736542f05/components/viz/common/frame_sinks/README.md "Supplemental Documentation: CopyOutputRequests"
[2]: https://cef-builds.spotifycdn.com/docs/150.0/structcef__settings__t.html?utm_source=chatgpt.com "Chromium Embedded Framework (CEF): cef_settings_t Struct Reference"
[3]: https://chromium.googlesource.com/chromium/src/%2B/8ba1bad80dc22235693a0dd41fe55c0fd2dbdabd/components/viz/common/frame_sinks/copy_output_result.h "components/viz/common/frame_sinks/copy_output_result.h - chromium/src - Git at Google"
[4]: https://chromium.googlesource.com/chromium/src/%2B/e8d5f33e75f0832d4f2d1430ff1da13c3c84cf6d/components/viz/common/frame_sinks/copy_output_request.h "components/viz/common/frame_sinks/copy_output_request.h - chromium/src - Git at Google"
[5]: https://cef-builds.spotifycdn.com/docs/149.0/classCefRenderHandler.html "Chromium Embedded Framework (CEF): CefRenderHandler Class Reference"




# My recommendation

For a **4K image stream with alpha, up to 30 fps, emitting only when the page changes**, the most future-proof architecture is:

> **CEF accelerated off-screen rendering → client-owned GPU texture ring → versioned custom stream format**

I would not modify Chromium or use Viz internals unless you need compositor-level information such as render passes, quads, per-surface metadata, or element attribution.

The architecture would be:

```text
CEF / Chromium
3840 × 2160 transparent windowless browser
                    │
                    ▼
       OnAcceleratedPaint callback
   temporary native shared GPU texture
                    │
             GPU-to-GPU copy
                    ▼
       Client-owned texture ring
                    │
          ┌─────────┴─────────┐
          ▼                   ▼
 changed-tile stream     colour + alpha
 lossless/custom         encoded tracks
          │                   │
          └─────────┬─────────┘
                    ▼
       Versioned custom container
```

## Why CEF is the future-proof boundary

CEF’s public `OnAcceleratedPaint` callback gives you a platform-native shared rendering resource. The current API describes a D3D texture handle on Windows, an `IOSurface` on macOS, and native-buffer planes/file descriptors on Linux. It also explicitly says that the resource belongs to a pool, cannot be retained after the callback, and must be copied into a client-owned texture before returning. ([CEF Builds][2])

That ownership model is a good architectural boundary:

```text
CEF-specific code
    Import temporary texture
    Copy texture
    Return callback
             │
             ▼
Your code
    Own texture
    Schedule frames
    Detect changes
    Compress
    Serialize
```

Everything downstream of the texture copy can remain independent of CEF, Chromium, Viz, Mojo, SharedImage, and other changing browser internals.

# Recommended CEF configuration

Conceptually:

```cpp
CefWindowInfo window_info;
window_info.windowless_rendering_enabled = true;
window_info.shared_texture_enabled = true;

CefBrowserSettings browser_settings;
browser_settings.windowless_frame_rate = 30;
browser_settings.background_color =
    CefColorSetARGB(0, 0, 0, 0);
```

CEF documents `windowless_frame_rate` as a **maximum** rate. The actual rate may be lower when the browser does not generate frames that quickly. A fully transparent windowless browser background enables transparent painting. ([CEF Builds][3])

That aligns closely with your desired semantics:

```text
Page changes at 60 fps  → at most 30 delivered frames/s
Page changes at 12 fps  → approximately 12 delivered frames/s
Static page             → no continuous duplicate image stream
```

I would let Chromium drive its own `BeginFrame` cadence. CEF also exposes external `BeginFrame` control, but that is more appropriate for deterministic/offline rendering than a naturally change-driven stream. ([CEF Builds][4])

## One cross-platform caveat

The current CEF generated documentation is somewhat inconsistent here:

* `OnAcceleratedPaint` describes native resources for Windows, macOS, and Linux.
* The Windows-oriented `cef_window_info_t` documentation describes shared-texture windowless rendering as currently supported on Windows with D3D11. ([CEF Builds][2])

For a production first version, **Windows/D3D11 is the lowest-risk accelerated path**. Keep the GPU importer behind an interface:

```cpp
class FrameImporter {
 public:
  virtual ImportedFrame CopyFromCef(
      const CefAcceleratedPaintInfo& info) = 0;
};
```

Implementations can later be:

```text
D3D11FrameImporter
MetalIOSurfaceFrameImporter
LinuxDmabufFrameImporter
CpuBgraFrameImporter       // correctness fallback
```

The CPU `OnPaint` path returns a complete width × height × 4 BGRA buffer with dirty rectangles, but it should be considered a fallback rather than the primary 4K30 path. ([CEF Builds][2])

# 4K30 bandwidth implications

One uncompressed 4K BGRA frame is:

```text
3840 × 2160 × 4 = 33,177,600 bytes
                  ≈ 31.6 MiB
```

At 30 fps:

```text
≈ 995 MB/s
≈ 949 MiB/s
```

An 8-bit alpha plane alone is:

```text
3840 × 2160 = 8.29 MB/frame
30 fps      = 248.8 MB/s
```

This is why the design should avoid:

```text
GPU render
    ↓
CPU readback of complete 4K frame
    ↓
CPU comparison
    ↓
CPU compression
```

Instead:

```text
GPU render
    ↓
GPU-to-GPU copy
    ↓
GPU or tiled change processing
    ↓
Read or encode only required payload
```

A full GPU texture copy at this rate is reasonable; repeated GPU-to-CPU transfer, memory copying, and serialization are more likely to become the bottleneck.

# The callback implementation

Inside `OnAcceleratedPaint`, do almost nothing:

```cpp
void RenderHandler::OnAcceleratedPaint(
    CefRefPtr<CefBrowser> browser,
    PaintElementType type,
    const RectList& dirty_rects,
    const CefAcceleratedPaintInfo& info) {

  TextureSlot* slot = ring_.TryAcquire();

  if (!slot) {
    // Prefer dropping an old or incoming frame over blocking CEF.
    dropped_frames_++;
    return;
  }

  ImportAndCopyToOwnedTexture(info, *slot);

  slot->dirty_rects = dirty_rects;
  slot->capture_time = MonotonicNow();
  slot->state = FrameState::Ready;

  stream_worker_.Signal();
}
```

Use a ring of approximately four textures:

```text
Slot A: available
Slot B: receiving GPU copy
Slot C: change detection/compression
Slot D: being serialized or consumed
```

Do not encode, hash the whole frame, write to disk, or wait for a consumer inside the callback. CEF render-handler methods execute on its UI thread, and the shared resource is returned to CEF’s pool when the callback ends. ([CEF Builds][2])

# Change-dependent frame semantics

Treat each callback as a new candidate visual state, not as one required output frame.

Use this policy:

```text
No callback:
    emit nothing

One callback:
    emit changed frame

Several callbacks within 33.3 ms:
    retain the newest completed frame
    discard or coalesce older frames

Consumer requires constant 30 fps:
    consumer repeats the most recent frame
    producer remains change-driven
```

Store monotonic timestamps rather than deriving timestamps from frame number:

```text
frame 101: 2.000 s
frame 102: 2.033 s
frame 103: 2.267 s   ← page was unchanged for 234 ms
```

This preserves the difference between:

* A page that remained unchanged.
* A page that rendered duplicate frames.
* Frames dropped because the producer was overloaded.

Dirty rectangles are useful hints, but I would not treat them as proof that every pixel in the rectangle changed. For stronger deduplication, split the image into tiles and hash only tiles intersecting the dirty rectangles.

# Custom format design

Do not make the file format equivalent to a C++ struct dump. Use a versioned, length-delimited chunk format:

```text
FILE HEADER
    magic
    major version
    minor version
    time base
    initial width and height
    default pixel format
    alpha mode
    colour-space metadata

STREAM DESCRIPTION
    stream ID
    payload encoding
    tile dimensions
    codec configuration

FRAME
    frame ID
    presentation timestamp
    flags
    dimensions, if changed
    changed-region list
    payload descriptors
    payload data
```

Every chunk should contain:

```text
chunk type
chunk version
chunk length
chunk payload
```

This allows future readers to skip unfamiliar extensions.

## Pixel and alpha metadata

Make these explicit:

```text
pixel format: BGRA8, RGBA8, R8, NV12, etc.
alpha mode: premultiplied, straight, opaque
colour primaries
transfer function
matrix coefficients
range
orientation
```

I would normalize Chromium output into a canonical:

```text
BGRA8
premultiplied alpha
top-left origin
explicit sRGB colour space
```

Premultiplied alpha is well suited to compositor output. Convert to straight alpha only at a boundary that explicitly requires it.

# Two useful payload modes

Design the format to support at least two modes.

## Mode 1: lossless changed tiles

Best for UI rendering, inspection, remote application surfaces, and exact alpha:

```text
Initial frame:
    complete 4K BGRA keyframe

Subsequent frame:
    list of changed 64 × 64 or 128 × 128 tiles
    each tile compressed independently
```

For example:

```text
FRAME
    timestamp = 4.2667 s
    tiles:
        x=12, y=7, encoding=ZSTD, length=...
        x=13, y=7, encoding=ZSTD, length=...
        x=12, y=8, encoding=ZSTD, length=...
```

Advantages:

* Exact alpha.
* Random tile updates.
* Efficient for menus, text, cursors, and localized animation.
* Easy recovery after packet loss.
* Decoders do not need a video-codec stack.

Disadvantage: full-screen animation can approach the raw 1 GB/s rate before compression.

## Mode 2: separate colour and alpha tracks

Best when most or all of the 4K canvas changes continuously:

```text
Colour:
    hardware-encoded video track

Alpha:
    synchronized 8-bit grayscale track
```

For example:

```text
FRAME
    timestamp
    colour payload: AV1/HEVC/etc.
    alpha payload: lossless R8 tiles or separate codec
```

This lets the colour stream use conventional GPU video encoding while retaining alpha separately. Make codec identifiers extensible instead of making AV1, HEVC, or any particular encoder part of the container specification.

For graphics with sharp transparent edges, avoid relying blindly on 4:2:0 colour subsampling: it can degrade coloured edges around alpha transitions. A lossless tile mode, RGB/4:4:4 colour stream, or sufficiently high-quality codec configuration may be preferable.

# Concrete production choice

For a Windows-based first implementation, I would build:

```text
CEF windowless browser
3840 × 2160
transparent background
maximum 30 fps
D3D11 accelerated shared texture
        │
        ▼
GPU copy into four-texture ring
        │
        ▼
64 × 64 dirty-tile tracking
        │
        ▼
Custom versioned container
    BGRA8 premultiplied
    full keyframes
    compressed changed tiles
    monotonic timestamps
    explicit colour metadata
```

Then add an optional colour-plus-alpha encoded mode if testing shows that full-screen animation produces too much bandwidth.

Use direct Chromium/Viz integration only when you need something CEF does not expose, such as:

* Per-render-pass output.
* Viz quad metadata.
* Surface-level capture.
* Client-allocated compositor targets.
* Element or layer ID buffers.
* Exact control of Chromium’s compositor schedule.

For a final flattened 4K RGBA stream, a Chromium fork would add considerable maintenance risk without giving enough benefit over accelerated CEF OSR.

[1]: https://www.chromium.org/Home/ "Chromium"
[2]: https://cef-builds.spotifycdn.com/docs/150.0/classCefRenderHandler.html "Chromium Embedded Framework (CEF): CefRenderHandler Class Reference"
[3]: https://cef-builds.spotifycdn.com/docs/150.0/structcef__browser__settings__t.html "Chromium Embedded Framework (CEF): cef_browser_settings_t Struct Reference"
[4]: https://cef-builds.spotifycdn.com/docs/150.0/structcef__window__info__t.html "Chromium Embedded Framework (CEF): cef_window_info_t Struct Reference"
