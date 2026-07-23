## Bottom line

There are reusable, mature components—but not one library that cleanly covers both:

1. **Identify and read a selected keyboard or mouse while Windows continues using it**
2. **Selectively block that device from Windows and route it exclusively to your app**

For a new Windows 11 C++ implementation in 2026, my shortlist is:

| Option                            |        Per-device input | Blocks selected device from Windows | Assessment                                      |
| --------------------------------- | ----------------------: | ----------------------------------: | ----------------------------------------------- |
| **Microsoft GameInput 3.4**       |                     Yes |                                  No | Best supported modern Windows abstraction       |
| **SDL3**                          | Yes, with mouse caveats |                                  No | Best mature cross-platform abstraction          |
| **Win32 Raw Input directly**      |                     Yes |                                  No | Most proven and controllable foundation         |
| **ManyMouse**                     |              Mouse only |                                  No | Useful small library, but limited               |
| **Interception**                  |                     Yes |                                 Yes | Convenient, but too old/risky for a new product |
| **Microsoft KMDF filter samples** |                     Yes |                                 Yes | Best production starting point, but not turnkey |

My recommendation is:

> **Start with GameInput 3.4 or a small internal Raw Input component. Add a custom KMDF device filter only if exclusive suppression is genuinely required.**

---

# 1. Microsoft GameInput 3.4: strongest modern user-mode option

Despite the gaming-oriented name, GameInput is a general native Windows input API supporting individual keyboards, mice, game controllers and raw HID devices. As of **22 July 2026**, Microsoft’s current NuGet package is **Microsoft.GameInput 3.4.259**, published on **16 July 2026**. GameInput 3.4 added raw HID reports and further reliability improvements. ([NuGet][1])

It provides substantially better device identity handling than a basic Raw Input wrapper:

* Per-device keyboard and mouse readings
* Vendor and product IDs
* PnP device path
* Container ID
* An application-local device ID
* A root-device ID for grouping children of composite USB devices
* Device connect/disconnect callbacks
* Filtering reading callbacks to one specific `IGameInputDevice` ([Microsoft Learn][2])

That composite-device support matters. A modern keyboard may expose separate HID collections for:

* Standard keyboard keys
* Consumer/media controls
* Mouse or touch functionality
* Vendor-specific buttons
* Lighting or configuration

GameInput’s root-device and container information give you a supported way to decide that these collections belong to the same physical USB product. ([Microsoft Learn][2])

### Architectural shape

```text
IGameInput
   │
   ├─ device callbacks
   │    └─ build/update physical device registry
   │
   └─ reading callback for selected device
        ├─ keyboard state
        ├─ mouse state
        ├─ optional raw HID reports
        └─ normalize into application events
```

### Important intricacies

GameInput keyboard input is fundamentally a stream of **device-state readings**, not a Windows text-input API. For key-down and key-up events, retain the previous key set for each device and calculate transitions. Do not use it to interpret keyboard layouts, dead keys, AltGr or IME composition; keep normal Windows text input for that. Microsoft explicitly describes GameInput keyboard handling as physical-key state rather than text entry. ([Microsoft Learn][3])

The device ID is designed to be stable for the application, but reconnect behaviour can still depend on the USB port and device topology. For an end-user-selectable device, persist multiple identifiers and provide a “press any key on the desired keyboard” binding flow. ([Microsoft Learn][2])

GameInput can receive input while your application is in the background. Its “exclusive” focus policy only affects competing **GameInput clients**; it does not prevent Windows or ordinary applications from seeing keyboard and mouse input. ([Microsoft Learn][4])

It also requires shipping Microsoft’s GameInput redistributable with a conventional desktop installer; adding the NuGet package alone does not install the runtime on the target PC. ([Microsoft Learn][5])

### Verdict

**Best first choice** when:

* The application is Windows-only.
* You want a supported Microsoft API.
* Device identity and composite-device handling matter.
* You might later need vendor-specific raw HID reports.
* Normal Windows input should continue.

---

# 2. SDL3: mature library, but examine the mouse semantics carefully

SDL is one of the most battle-tested C libraries for input and windowing. SDL3 is actively maintained, uses the permissive zlib licence and now exposes individual keyboard and mouse device IDs. ([GitHub][6])

Relevant SDL3 APIs and events include:

```cpp
SDL_GetKeyboards(...)
SDL_GetMice(...)

SDL_EVENT_KEY_DOWN
SDL_EVENT_KEY_UP
SDL_KeyboardEvent::which

SDL_EVENT_MOUSE_MOTION
SDL_MouseMotionEvent::which
```

It supports Windows Raw Input internally, can request background Raw Input, and optionally has a GameInput-backed Windows path. ([wiki.libsdl.org][7])

SDL also solved real-world high-polling-rate mouse problems by moving its Windows implementation toward buffered Raw Input processing, illustrating one of the subtle issues you avoid by using a mature library. ([GitHub][8])

## The significant caveats

### Mouse identity is strongest in relative mode

SDL documents the mouse `which` identifier as meaningful when relative mouse mode is active; otherwise mouse motion, button and wheel events can report zero rather than the physical device ID. ([wiki.libsdl.org][9])

That makes SDL very suitable for:

* 3D navigation
* Camera control
* Multiple relative pointing devices
* Kiosk-style applications

It is less attractive when you need:

* A normal desktop cursor
* Absolute cursor position
* Reliable physical-device attribution for every wheel and click event

### IDs are connection-lifetime IDs

`SDL_KeyboardID` and related IDs are valid while the device is connected and can change after reconnecting. You still need your own persistent device-matching layer. ([wiki.libsdl.org][10])

SDL also warns that enumerations may include devices such as KVM interfaces, power-button collections or mouse devices exposing keyboard functionality. It is often better to mark a device “active” only after receiving a real event from it. ([wiki.libsdl.org][11])

### Verdict

Choose SDL3 when:

* You already use SDL.
* Cross-platform support is useful.
* Relative mouse mode fits the application.
* You want to avoid owning a Windows message-loop implementation.

For a native Windows application whose main purpose is physical USB-device routing, I would prefer **GameInput or direct Raw Input**.

---

# 3. Direct Win32 Raw Input may be better than a wrapper

Raw Input is old but still the canonical Windows mechanism for distinguishing physical keyboards and mice. Microsoft’s current documentation continues to describe it as a stable, robust API and recommends buffered processing for high-frequency devices. ([Microsoft Learn][12])

At first glance, wrapping it in a third-party library sounds attractive. However, Microsoft documents a crucial process-wide constraint:

> Only one window per raw-input device class may be registered within a process.

Microsoft consequently advises against having an arbitrary library independently call `RegisterRawInputDevices`, because it may overwrite or conflict with registration performed elsewhere in the host application. ([Microsoft Learn][13])

That affects combinations such as:

* Your wrapper plus SDL
* Your wrapper plus Qt internals
* Two plugins that both register for keyboard input
* A shared library creating a hidden window without coordinating with the main app

For this reason, the safest Raw Input “library” is often a relatively small component that your application owns:

```text
WindowsInputService
 ├─ one Raw Input registration owner
 ├─ one message window
 ├─ GetRawInputBuffer batching
 ├─ SetupAPI/PnP identity resolver
 ├─ hot-plug handling
 ├─ device selection and persistence
 └─ normalized lock-free event queue
```

This code is not enormous. The complexity lies mostly in:

* Lifetime and hot-plug handling
* Correct scan-code interpretation
* Extended keys
* Composite devices
* Multiple identical devices
* High-polling-rate mice
* Buffer alignment
* Device identity persistence
* Coordinating background input
* Avoiding duplicate input paths

A narrow internal module lets you define and test those behaviours explicitly instead of inheriting undocumented decisions from a tiny GitHub wrapper.

---

# 4. ManyMouse: a useful small library for multiple mice

**ManyMouse** is a small zlib-licensed C library designed specifically to distinguish multiple physical mice. Its Windows implementation uses Raw Input, does not require the application to provide its own visible window, and has a simple polling API. ([GitHub][14])

It is appealing when the requirement is simply:

```text
Mouse A movement → channel A
Mouse B movement → channel B
Mouse C buttons  → channel C
```

But it has material limitations:

* No keyboard support
* Device indexes can change when devices are removed or rediscovered
* Not thread-safe
* A relatively small, niche codebase
* Some Windows integration commentary reflects much older DirectInput-era behaviour ([GitHub][14])

### Verdict

A good reference implementation or prototype dependency for **multiple mice**, but not the foundation I would choose for a broader production input subsystem.

---

# 5. Interception: closest turnkey exclusive solution, but problematic

**Interception** is the project that most directly matches the full requirement. It provides:

* A C API
* Keyboard and mouse filter drivers
* Per-device capture
* The ability to suppress packets
* Modified-event reinjection

Unfortunately, it carries significant product risks.

Its published build environment is based on **WDK 7.1**, documentation says it was tested through Windows 10, and its latest published release dates from **2017**. The repository’s most recent substantive activity is also several years old. ([GitHub][15])

It also uses a dual licensing arrangement: non-commercial use under its open licence conditions, with a separate commercial licence for commercial integration and access to additional driver/installer source. ([GitHub][15])

More seriously, the driver architecture has known finite-device-ID behaviour. Repeated unplugging, reconnecting, suspend/resume or device enumeration can consume keyboard and mouse slots; downstream projects explicitly warn that devices can stop being captured until restart or reboot. ([GitHub][16])

There are also reports involving loss of keyboard or mouse operation and non-trivial recovery when the driver installation or state goes wrong. ([GitHub][17])

Wrappers such as AutoHotInterception or C++ convenience wrappers do not change the underlying kernel driver or these limitations.

### Verdict

Reasonable for:

* A lab prototype
* An experimental workstation
* A disposable test image
* Demonstrating that suppression solves the application problem

I would **not** use it as the shipped basis of a newly developed Windows 11 healthcare or clinical product.

---

# 6. Microsoft KMDF samples: best base for exclusive production behaviour

If the selected device really must be invisible to ordinary Windows input, the best-maintained source is not a library but Microsoft’s official driver samples:

* `kbfiltr` for keyboards
* `moufiltr` for mice
* `vhidmini2` and VHF for creating virtual HID devices

Microsoft maintains current Windows 11 driver samples for Visual Studio 2022 and the Windows 11 WDK. ([GitHub][18])

The keyboard and mouse filter samples demonstrate the key interception pattern:

```text
HID/class driver packet callback
          │
          ├─ inspect packet
          ├─ copy packet into own queue
          ├─ modify or remove packet
          └─ forward remaining packets
```

The samples specifically describe adding, deleting or transforming keyboard and mouse packets and show how to expose a separate control path to user mode. ([Microsoft Learn][19])

They are not a drop-in USB solution. Some sample details are historically PS/2-oriented, while USB keyboards and mice normally pass through `kbdhid`/`mouhid` and the HID class stack. Microsoft’s current stack guidance allows filter placement above the HID keyboard/mouse or class drivers, but warns against inserting unsupported filters between HIDCLASS and the underlying HID transport. ([Microsoft Learn][20])

A production design would probably contain:

```text
Selected USB keyboard/mouse
        │
        ▼
Device-specific KMDF upper filter
        │
        ├─ event copy → bounded kernel queue → Windows service → app
        │
        └─ policy:
             pass through
             suppress
             transform
```

For transformed input that should reappear as a new logical device:

```text
App/service
    │
    ▼
KMDF virtual-device driver
    │
    ▼
Virtual HID Framework
    │
    ▼
Virtual keyboard or mouse visible to Windows
```

VHF is Microsoft’s supported kernel-mode framework for implementing a virtual HID source. ([Microsoft Learn][21])

## What remains your responsibility

Even with the samples, you must engineer:

* Device-instance selection
* Composite-device grouping
* Configuration and service protocol
* Fail-open behaviour
* Queue overflow policy
* Disconnect while keys are held
* Sleep/resume
* Service restart
* Recovery when configuration is corrupted
* Driver installation and rollback
* Signing, HVCI and deployment testing

The most important design rule is:

> When anything is uncertain or unavailable, pass input through normally.

A malfunctioning input filter must not strand the user without keyboard and mouse control.

---

# 7. Common false leads

## HIDAPI and libusb

HIDAPI is excellent for vendor-specific HID peripherals: measurement instruments, custom control panels, sensors and similar hardware.

It is not generally the right route for normal Windows keyboards and mice. Windows reserves those standard HID collections for its input stack, and libusb’s Windows documentation explicitly notes the exclusive-access restriction for HID keyboards and mice. ([GitHub][22])

Replacing the device driver with WinUSB/libusb would effectively detach that interface from its normal Windows keyboard/mouse function, which is very different from passively observing it.

## HidHide

HidHide is an actively maintained device-hiding filter with a programmatic C/C++ configuration interface and current Windows support. However, its official documentation explicitly says it cannot be used to hide mice, keyboards or touchpads. It targets controller-type HID devices instead. ([Nefarius][23])

## libuiohook

libuiohook is useful for global keyboard and mouse hooks, but global hooks do not reliably expose the originating physical HID device. It solves “observe global input,” not “distinguish keyboard A from keyboard B.”

## Gainput and InputEmulator

Gainput is archived, while InputEmulator’s drivers are old, unsigned and require test-signing workflows. Neither is an attractive starting point for a current Windows 11 product. ([GitHub][24])

## RawAccel

RawAccel is a useful example of a current, signed Windows 10/11 mouse filter architecture. It is actively used and worth studying, but its purpose is mouse acceleration rather than general keyboard/mouse routing, so it is reference material rather than a reusable API. ([GitHub][25])

---

# Proposed routing architecture for streaming-browser

## Separate capture, routing, transport and sinks

The idea makes sense, but the capture API should not itself own WebSockets. Treat
the system as four replaceable layers:

```text
Physical devices
            │
            ▼
Capture backend
    GameInput / Raw Input / future KMDF filter
            │
            ▼
Canonical device-event model
            │
            ▼
Router + route ownership + safety state
            │
            ├── local in-process or named-pipe transport
            └── binary WebSocket transport (loopback/LAN)
                             │
                             ├── CEF off-screen browser sink
                             ├── test/logger sink
                             ├── Windows injection or virtual-HID sink
                             └── remote receiver or hardware bridge
```

This separation is important because the right capture mechanism can change from
GameInput to Raw Input or a filter driver without changing the network protocol,
and WebSockets can later be replaced without touching device discovery.

No available library provides all of these layers with the required
per-physical-device semantics and product safety. Software-KVM projects such as
Lan Mouse and Input Leap are useful state-management references, but their Windows
low-level-hook paths observe the merged Windows input stream rather than preserving
the originating keyboard or mouse. They should not be used as the device-identity
foundation. ([GitHub][30]) ([GitHub][31])

## How this fits the current repository

The current viewer translates `WM_KEY*`, `WM_CHAR`, `WM_IME_*` and `WM_MOUSE*`
messages into `protocol::InputEvent` and `protocol::ImeEvent` in
[`viewer_main.cc`](../src/viewer/viewer_main.cc). The producer converts those
messages to `CefBrowserHost::SendKeyEvent`, `SendMouse*`, `SetFocus` and IME calls
in [`browser_client.cc`](../src/producer/browser_client.cc). Those CEF methods are
already the correct browser sink. Mouse coordinates are view-relative, and CEF
also provides explicit focus and capture-lost operations. ([CEF][26])

Do **not** connect a new device client to the existing streaming named pipe:

* The pipe handshake is coupled to viewer process verification and duplication of
    D3D11 texture handles.
* It permits one viewer client.
* Its `InputEvent` has no source device, source timestamp, event sequence, route or
    state-snapshot fields.
* Its input values are already adapted to CEF/Win32 semantics, not a general
    device-event model.

Instead, add a separate input ingress only after the isolated experiments pass.
The producer-side ingress should adapt canonical routed events to the existing
`BrowserClient::OnViewerInput()` path on the CEF UI thread. The current viewer
path should remain functional and become one input source among others.

Crucially, the **capture backend must run in its own process**, not inside the
producer or viewer, because both already register Raw Input via Chromium/Win32
(see "Raw Input registration collides with CEF and the viewer" below). The
producer's ingress is a thin adapter that consumes canonical events over the
transport; it never enumerates devices itself.

## Recommended first capture backend

Start the experiment with **GameInput 3.4.259** and implement a direct Raw Input
backend beside it for an A/B test on the intended hardware.

GameInput is the preferred product candidate because it supplies device/root IDs,
container IDs, PnP paths, VID/PID, connect/disconnect callbacks and per-device
reading callbacks. Its current package also adds raw HID reports. However:

* A keyboard reading is a set of currently pressed keys. The adapter must diff the
    current and previous sets per device to emit transitions.
* Mouse readings contain cumulative relative positions, not view coordinates.
* `codePoint` is for displaying a key binding, not constructing text.
* Background input must be explicitly enabled and tested.
* “Exclusive foreground input” excludes only other GameInput clients; it does not
    suppress normal Windows input.
* The GameInput redistributable must be installed with the product. ([Microsoft
    Learn][3]) ([Microsoft Learn][4]) ([Microsoft Learn][5])

The Raw Input backend is the control implementation because it exposes exact
make/break packets and is built into Windows. Use a single owned registration,
one message-only window, `RIDEV_INPUTSINK | RIDEV_DEVNOTIFY`, and buffered reads
for high-rate mice. Microsoft documents both the one-window-per-device-class
process constraint and `GetRawInputBuffer` for 1 kHz devices. ([Microsoft
Learn][12]) ([Microsoft Learn][13])

The experiment must decide based on measured behavior, especially:

* identity after reconnect, port changes, hubs and docks;
* grouping composite keyboard collections;
* key repeat behavior;
* background and session-lock behavior;
* 1 kHz and, if target hardware supports it, 8 kHz mouse input;
* simultaneous use with the current viewer and CEF process;
* process shutdown and runtime/redistributable deployment.

## Canonical event and device model

Keep device identity richer than one opaque ID:

```cpp
struct PhysicalInputDevice {
        DeviceKey runtime_key;
        DeviceKey root_key;
        Guid container_id;
        std::string pnp_path;
        std::string display_name;
        std::uint16_t vendor_id;
        std::uint16_t product_id;
        DeviceKind kind;
        bool connected;
};
```

Persist a conservative match record containing the root/container identity,
VID/PID and PnP path. A GameInput device ID can remain stable across reboots when
the device returns to the same USB port, but a different port may change it. The
binding UI should therefore use “activate the desired device,” and it must ask
again instead of guessing when two devices match equally well.

The canonical event envelope should contain at least:

```text
protocol version
connection session ID
route ID
source peer ID
source device key
monotonic source timestamp
monotonic sequence number
event type and typed payload
```

Keyboard payloads need scan code, extended-key flags, make/break/repeat state and
a modifier-state snapshot. Mouse payloads need relative motion, optional absolute
position, buttons and horizontal/vertical wheel deltas. Keep source timestamps
for diagnostics; never compare clocks from different hosts without an explicit
clock-synchronization estimate.

Do not make raw keyboard input promise full text entry. Physical key transitions,
text and IME composition are different channels. The present viewer correctly
gets text and IME from Windows window messages. The first device-routing prototype
should support physical keys/actions only. Later text support should use a trusted
high-level text/IME source or a separately validated target-layout translation;
it must not concatenate GameInput `codePoint` values.

CEF requires absolute, view-relative mouse coordinates. For a relative physical
mouse, the browser sink should maintain a per-route virtual cursor, apply motion,
clamp it to the current viewport, and then emit the existing absolute
`InputEvent`. Do not use the Windows desktop cursor as this state.

## WebSocket transport assessment

WebSockets are a reasonable first network transport because they are full duplex,
have binary messages, work through common infrastructure, and are easy to inspect
or consume from browser-based diagnostics. Use **Boost.Beast** for the C++ spike:
it is actively maintained with Boost, supports client/server, binary messages,
asynchronous I/O, TLS, ping/pong and message-size limits. ([Boost][27])

Beast deliberately does not provide the application queue. A `websocket::stream`
is not thread-safe, and only one asynchronous write may be outstanding. Serialize
all connection work on one Asio strand and drain one owned write queue. Buffers
must remain alive through completion. ([Boost][28]) Context7 and the Beast
examples confirm this same one-write-at-a-time session pattern.

For this protocol:

* Use binary messages and a small versioned envelope, not JSON per device event.
* Use a WebSocket subprotocol name that includes the routing protocol major
    version.
* Set `TCP_NODELAY` after connecting.
* Disable `permessage-deflate`; tiny input messages are not worth compression
    latency or complexity.
* Set strict handshake, idle and maximum-message limits. Beast exposes
    `read_message_max`; RFC 6455 explicitly requires implementation-specific size
    limits. ([Boost][29]) ([IETF][32])
* Use asynchronous ping/pong for liveness, but maintain an application heartbeat
    containing route/session state as well.
* For LAN use, use `wss://` with authenticated peers. WebSocket masking is not
    encryption, and `Origin` is not authentication for native clients.

WebSocket is ordered and reliable because it runs over TCP. That is useful for
key/button edges but can deliver old mouse motion after packet loss. The router
must therefore have bounded queues and event-aware overload behavior:

| Event class | Queue policy |
| --- | --- |
| Key/button down or up | Never coalesce; on inability to deliver promptly, reset the route rather than replay stale edges |
| Relative mouse motion | Sum adjacent deltas for the same device/route |
| Absolute mouse motion | Keep only the newest unsent position |
| Wheel | Sum adjacent deltas with overflow bounds |
| Device and route state | Send latest state snapshot after connect/reconnect |

Batch events for at most a small configurable interval, initially 1 ms, or until
the message reaches a small event-count limit. If queue age or size crosses its
hard limit, close/reset the route and release state instead of eventually
delivering stale control input. Do not acknowledge every event over an already
reliable stream.

WebSocket should remain a replaceable transport. If impairment testing shows TCP
head-of-line blocking is unacceptable, evaluate a datagram/QUIC design later.
There is no reason to take on that complexity before measuring the target network.

## Wire messages and recovery state

Use a separate routing protocol rather than exposing C++ structs directly. Its
first revision needs these message classes:

```text
Hello / Accept / Reject
DeviceList / DeviceChanged
ClaimRoute / ReleaseRoute / RouteStatus
EventBatch
InputStateSnapshot
Heartbeat
ReleaseAll
Error / Close
```

The receiver owns the authoritative state for each route: currently pressed keys,
mouse buttons, virtual cursor and active source session. On connection loss,
heartbeat timeout, source switch, device unplug, route revocation or protocol
error, it must atomically:

1. synthesize key-up and button-up for all held state;
2. send CEF capture-lost and focus-false when the browser route is affected;
3. discard queued motion and events from the old session;
4. require a new route claim and a complete state snapshot before accepting more
     events.

Use a new random session ID after each reconnect. Sequence numbers detect gaps and
duplicates within that session; never continue an old event stream after a new
connection. Route ownership should be explicit. Default to one controller lease
per browser, with viewer/device input merging enabled only by configuration.
Changing the lease holder always performs `ReleaseAll` first.

## Process and security boundaries

Keyboard capture is effectively keylogging, and remote input is a remote-control
surface. Treat both as security-sensitive even in a prototype.

* Run the user-mode capture agent in the intended interactive logon session, not
    Session 0. Prove service/session behavior before considering a Windows service.
* Bind only to `127.0.0.1` in the first spike. Do not open a firewall rule.
* For LAN trials require WSS plus mutual TLS or pinned peer certificates, a
    destination allowlist and explicit pairing/authorization.
* Authenticate the peer independently of WebSocket `Origin`.
* Put no bearer secret in a URL or command line.
* Apply connection, message-size, event-rate and route-count limits before parsing
    event payloads.
* Log route ownership, device connect/disconnect and safety resets, but do not log
    key values, text, passwords or other event payloads by default.
* Keep the current logon-SID-protected named pipe for local graphics transport;
    do not weaken it to support the router.

If exclusive capture is later added, the kernel filter must be device-specific
where possible, default to pass-through and fail open whenever its service or
policy is unavailable. A watchdog must never make suppression the safe default.

## “Other devices” means separate sink types

Three destinations require different implementations:

1. **Headless browser:** adapt to CEF `SendKeyEvent`, `SendMouse*`, focus,
     capture-lost and IME APIs. This is the first real sink.
2. **Another Windows application/desktop:** a `SendInput` spike can prove generic
     injection, but it loses physical-device identity and is constrained by desktop,
     integrity-level and secure-desktop boundaries. A production virtual device
     requires a signed VHF-based driver.
3. **Another physical host over USB:** Windows VHF creates a virtual device only
     on the same Windows host. Presenting as a USB keyboard/mouse to a different host
     needs USB-gadget-capable hardware/OS or a dedicated microcontroller bridge. It
     is not a WebSocket-library feature.

## Missed implications and mitigations

These are second-pass concerns not covered above. Each is a real risk with a
concrete mitigation, ordered roughly by how early it can bite.

### Raw Input registration collides with CEF and the viewer

`RegisterRawInputDevices` allows only one window per device class per **process**,
and CEF (Chromium) already registers Raw Input internally in the browser process.
Putting a Raw Input backend inside the producer would fight Chromium's own
registration and is explicitly discouraged for libraries. GameInput has the
mirror problem: older GameInput versions had bugs when an app used Raw Input
outside GameInput in the same process (fixed in 2.1, but still a coupling).

*Mitigation:* run the capture backend in its **own dedicated process**, never
inside the producer or viewer. This process hosts the message-only window, owns
the single Raw Input (or GameInput) registration, and is the WebSocket peer. The
producer only ever receives already-canonical routed events. This also keeps
keylogging-capable code isolated for review and signing, and lets capture crash
or restart without touching the render pipeline.

### GameInput background input and RDP/remote sessions

`GameInputEnableBackgroundInput` and remote-session support are real, but the
lock screen (secure desktop) and cross-session capture are not something a normal
user-mode process can reach. Over RDP, the physical mouse/keyboard on the client
are consumed by the RDP client, not enumerated as local HID on the host. The plan
already lists RDP as a test, but should state the likely outcome so it is not
treated as a bug.

*Mitigation:* declare lock-screen and cross-session capture **out of scope** for
the user-mode phases; they require a driver/session-0 service and separate
security review. For RDP, target the *host-local* devices only and document that
redirected client devices will not appear with stable physical identity.

### Hot-plug and route ownership race conditions

Device disconnect, route hand-off, and event delivery are concurrent. A device
can unplug *while* a key is held and *while* a route change is in flight, so
`ReleaseAll` and re-claim can interleave and either drop the release or apply it
to the wrong session.

*Mitigation:* make the receiver's route state a single-writer state machine keyed
by session ID. Every event carries its session ID; events whose session no longer
owns the route are dropped, not applied. `ReleaseAll` is idempotent and always
synthesizes releases from the *receiver's* tracked held-set, never from the
sender's claim. A disconnect and a route switch both funnel through the same
"revoke session → release held-set → require snapshot" transition.

### Timestamps, clock domains, and ordering

The envelope carries a monotonic source timestamp, but sender and receiver clocks
differ, and `QueryPerformanceCounter` is per-boot, not comparable across hosts.
Coalescing and latency budgets must not assume a shared clock.

*Mitigation:* order strictly by per-session sequence number, not by timestamp.
Use source timestamps only for *intra-session* delta timing (e.g. wheel/motion
accumulation windows) and for diagnostics. For any cross-host latency figure,
estimate offset from ping/pong round-trips and label it an estimate.

### Coordinate mapping must track the render-surface refactor

The viewer now renders into a dedicated child window
(`StreamingBrowserViewerRenderSurface`) with letterboxing (scale + offset), as the
current input-forwarding scripts already compute. A routed absolute-cursor sink
must use the *same* surface geometry, or clicks land in the wrong place, and the
mapping changes on resize/toolbar toggle/fullscreen.

*Mitigation:* the browser sink owns the virtual cursor in **page/view space**
(0–3840 × 0–2160), exactly like `protocol::InputEvent` today, and lets the
existing producer-side clamp handle it. Never compute pixel coordinates against
the OS window in the router; keep letterbox math on the presentation side so one
source of truth (the render surface) governs it.

### Split producer/viewer config already anticipates extra sinks

The repo just split configuration into `config/producer.*.yaml` and
`config/viewer.*.yaml` so alternative sinks can carry their own schema. The router
and capture agent should follow that convention rather than extending the
producer/viewer schemas.

*Mitigation:* give the capture agent and router their own
`config/input-router.*.yaml` (bind address, TLS/pairing material paths, route
leases, rate limits, batch interval, queue bounds). Parse it with the same strict
YAML loader used elsewhere (reject unknown keys, validate ranges). Network input
stays absent/disabled unless this file explicitly enables it.

### Backpressure fairness across multiple sinks

A single slow or malicious sink (e.g. a stalled remote receiver) must not stall
capture or other sinks. The one-write-at-a-time rule is per connection, but the
router fans out to several.

*Mitigation:* give each route/sink its **own** bounded queue and write pump.
Capture publishes into per-route queues without blocking; a queue that exceeds its
age/size bound resets *that* route only (`ReleaseAll` + drop), never global. The
capture thread must never block on any sink's socket.

### Anti-cheat, EDR and antivirus false positives

A process that enumerates all keyboards/mice, injects input into a browser, and
opens a socket looks exactly like malware to endpoint protection and to
game-anti-cheat drivers. This can silently kill the capture process or block the
Raw Input/GameInput path in the field.

*Mitigation:* sign the capture agent, document its behavior, keep it in its own
process with a clear name and manifest, and test on machines with the target EDR
before relying on results. Prefer GameInput's supported path over low-level hooks,
which are the most flagged.

### High-rate mouse (8 kHz) and message-window throughput

At 1 kHz–8 kHz, per-message Raw Input reads and any per-event WebSocket write will
saturate. The plan mentions buffered reads and batching but should tie them
together as a hard requirement, and account for the combined-read ordering caveat.

*Mitigation:* always drain with `GetRawInputBuffer`, and when mixing the `WM_INPUT`
`lParam` read with buffered draining, read the current event first, then drain the
buffer, per Microsoft's ordering note. Never emit one WebSocket message per device
event; the 1 ms/N-event batch is mandatory, not optional, above ~500 Hz.

### Reconnect storms and denial of service

Automatic reconnect plus per-connection resets can turn a flaky link or a hostile
client into a reconnect storm against the receiver.

*Mitigation:* apply RFC 6455-style randomized exponential backoff on the client,
and connection-rate + concurrent-connection limits on the server. Reject before
the WebSocket upgrade when limits are exceeded; do not allocate route state for
unauthenticated peers.

### Test and telemetry gaps

The current suites cover protocol, pipe, config and viewer E2E, but not stuck-key
safety, overload resets, or multi-device attribution. Those are exactly the
failure modes that hurt in the field.

*Mitigation:* add automated tests for: synthesized `ReleaseAll` completeness after
every failure injection; overload → single-route reset (not global); sequence-gap
rejection; and attribution when two synthetic sources feed distinct routes. Export
counters (resets, dropped-stale-sessions, max queue age, held-key count) without
logging event payloads, and assert them in tests.

# Prototype and implementation plan

All prototype-only work starts under **`experiments/usb-routing/`**. Keep it out
of the production targets in `src/` until its decision gates pass. The experiment
directory should own its dependencies, build instructions, fixtures and captured
measurements; it must not contain production credentials or captured keystroke
logs.

## Stage 0 — requirements and safety decisions

Record the answers that materially change the design:

* Must the selected keyboard/mouse continue to control Windows?
* Is the first destination CEF, another application, another PC, or USB hardware?
* Are text entry, keyboard layouts, dead keys and IME required, or only actions?
* Must capture work when no viewer is focused, on the lock screen, over RDP, or
    across user sessions?
* What are the target devices, polling rates, reconnect topology and maximum LAN
    latency?
* Is one source exclusive, or may viewer and routed devices control one browser
    simultaneously?

The first default should be **non-exclusive, same interactive session, one selected
source, browser action input, loopback transport**.

## Stage 1 — capture A/B probe

Create:

```text
experiments/usb-routing/
    README.md
    CMakeLists.txt
    common/
    gameinput-probe/
    raw-input-probe/
    tests/
```

Both probes should emit the same in-memory canonical event type and a privacy-safe
diagnostic display. Implement device enumeration, “activate to bind,” hot-plug,
state diffing and explicit `ReleaseAll`. Do not add WebSockets yet.

Test at least:

* two keyboards and two mice, including identical models if available;
* USB port, hub and dock changes;
* fast taps, chords, repeats and held-key unplug;
* five mouse buttons and horizontal/vertical wheels;
* 1 kHz sustained motion and target maximum polling rate;
* background, focus changes, sleep/resume, lock/unlock and RDP;
* repeated start/stop and redistributable install/repair/uninstall.

**Gate:** select GameInput, Raw Input or neither using captured evidence. If normal
Windows side effects are unacceptable, stop and make suppression a separate driver
project decision; do not disguise that requirement in user-mode code.

## Stage 2 — synthetic router and loopback WebSocket

Add a synthetic source, logger sink and Boost.Beast loopback client/server. This
stage validates protocol and backpressure without USB variability.

Required tests:

* strict parse/serialize round trips and malformed-length rejection;
* only one asynchronous write at a time;
* bounded queue behavior for each event class;
* route claim/revoke and rejected stale-session events;
* disconnect at every point in a key/button sequence;
* heartbeat timeout, reconnect and state snapshot;
* slow receiver and burst rates above the intended hardware rate;
* no stuck keys/buttons after any failure.

**Gate:** zero stuck-state failures, bounded memory, deterministic reset behavior
and acceptable loopback latency under overload.

## Stage 3 — selected capture backend over loopback

Connect the winning Stage 1 backend to the Stage 2 transport. Measure separately:

```text
device report → capture timestamp
capture → serialized batch
batch → receiver
receiver → sink callback
```

Initial targets are p99 under 5 ms for capture-to-sink on loopback, no lost
key/button edges, and no motion queue older than the configured hard limit. These
are routing targets, not click-to-visible browser latency; visible response also
depends on the page and the 30 fps render cadence.

## Stage 4 — experimental CEF sink

Add a disabled-by-default producer input ingress and a thin adapter to the current
CEF input methods. Do not change the D3D streaming handshake. Add explicit source
arbitration between the viewer and routed device, route-relative virtual cursor
state, and CEF release/focus behavior. Keep the virtual cursor in page/view space
(0–3840 × 0–2160); let the existing producer-side clamp and the render surface own
letterbox geometry so the routed cursor tracks resize, toolbar and fullscreen the
same way the viewer does. Gate the ingress with a dedicated
`config/input-router.*.yaml`, consistent with the split producer/viewer configs.

Validate with an HTML fixture that records key, pointer, wheel, focus, repeat and
button state. Extend end-to-end testing to kill the sender, unplug a held device
and switch route ownership while inputs are down.

**Gate:** the browser never retains pressed/captured state, unselected devices do
not affect it, and current viewer input/IME behavior remains unchanged.

## Stage 5 — secured LAN trial

Only after loopback is stable, enable a non-loopback endpoint with WSS,
authenticated pairing, certificate rotation and rate limits. Test latency, jitter,
loss, reordering below TCP, cable removal, Wi-Fi roaming and receiver restart.
Capture aggregate timing and reset counters only, never input contents.

**Gate:** measured LAN behavior meets the product latency and recovery budget. If
stale TCP delivery violates it, evaluate QUIC/datagrams behind the same transport
interface.

## Stage 6 — suppression and additional sinks

Decide independently whether to fund:

* a device-specific KMDF keyboard/mouse filter with fail-open service policy;
* a VHF virtual keyboard/mouse for local Windows injection;
* a receiver agent for another computer; or
* a USB HID hardware bridge for a physically separate target.

Interception may be used only as a disposable lab demonstration of suppression,
not as the production driver base.

## Acceptance criteria before production integration

* Correct source attribution with all qualified devices; ambiguous reconnects are
    surfaced, never silently rebound.
* No event from an unselected device reaches a route.
* No stuck key or button across unplug, process crash, timeout, route switch,
    sleep/resume or malformed input.
* Discrete transitions are never silently dropped; overload causes an observable
    route reset.
* Queue memory and event age remain bounded at the maximum qualified polling rate.
* Input payloads are absent from normal logs and crash telemetry.
* Network input is disabled by default and cannot be enabled unauthenticated.
* Existing viewer, IME, streaming, reconnect and soak tests continue to pass.

## Final selection

```text
Per-device capture while Windows still receives input
        → Microsoft GameInput 3.4 first; Raw Input as measured fallback

Transport for the first loopback/LAN prototype
        → Binary WebSocket over Boost.Beast, behind a transport interface

Browser destination
        → Separate producer input ingress adapting to existing CEF sink methods

Selected device must be blocked from Windows
        → Separate custom KMDF project based on current Microsoft samples

Generic local Windows destination
        → SendInput for a spike; signed VHF driver if productized

Physically separate USB destination
        → USB HID gadget or dedicated hardware bridge
```

# Implementation status (22 July 2026)

The first four user-mode phases are now implemented and validated in the current
workspace:

* A standalone Raw Input capture process enumerates PnP topology and routes only
    explicitly activated USB keyboards/mice. The built-in keyboard, touchpad,
    TrackPoint, Bluetooth mouse and unknown/virtual device nodes are refused.
* The per-device emergency chord is `Ctrl+Alt+Shift+F12`; all members must come
    from the same routed physical keyboard group.
* A versioned canonical binary protocol, bounded/coalescing queue, receiver-held
    state machine, random connection session, sequence checks and `ReleaseAll`
    behavior are implemented in `src/input_routing/`.
* The capture process connects to a Boost.Beast WebSocket endpoint on
    `127.0.0.1:17831`, using path `/input/v1`, subprotocol
    `streaming-browser-input.v1`, binary messages, `TCP_NODELAY`, a 16 KiB message
    limit, one write at a time, 1 ms event batching, application heartbeats and
    reconnect backoff.
* The producer ingress is opt-in, strictly loopback-only and accepts one client.
    It maintains a virtual page-space cursor, releases receiver-held keys/buttons
    on every disconnect/reset, and arbitrates exclusive control between viewer and
    routed-device input.
* The browser fixture received synthetic keyboard, motion and click events through
    the complete capture-client → WebSocket → producer → CEF path in both Debug and
    Release builds.

Automated validation completed:

* Debug and Release compilation with `/W4 /WX`.
* All five production tests pass (configuration, graphics protocol, pipe IPC,
    canonical input routing and WebSocket ingress).
* Emergency chord/bus-policy experiment tests pass in Debug and Release.
* Twenty-five repeated WebSocket fault/lifecycle integration tests pass.
* Thirty consecutive Release capture-client reconnects against one live producer
    pass; the producer remains running and listening.
* Wrong WebSocket path/subprotocol, non-loopback bind, malformed messages,
    duplicate/stale sessions, sequence gaps, bounded-queue overload and disconnect
    held-state cleanup are exercised.

The GameInput comparison probe is also implemented with the pinned Microsoft
GameInput 3.4.259 package. This machine's installed GameInput runtime enumerates
the keyboard/mouse nodes through API v0 but returns `E_NOTIMPL` for keyboard
reading callbacks and omits useful VID/PID/name details. Installing the package's
redistributable requires elevation and failed with MSI exit 1603 in this
non-elevated session. Therefore Raw Input remains the evidence-based backend for
this machine; GameInput 3.4 readings must be re-tested after an elevated
redistributable install.

Still gated by external prerequisites rather than code:

* **Secured LAN/WSS:** intentionally not enabled. Loopback is the only accepted
    address. LAN exposure requires certificate provisioning/pairing and a second
    host for meaningful network-impairment testing.
* **Exclusive suppression:** requires an installed, signed KMDF filter and
    administrator access. The WDK is present, but the current session is not
    elevated; no input filter was installed or tested remotely for safety reasons.
* **VHF/physical USB sinks:** require driver installation or USB-gadget hardware
    that is not available in this session.
* **Physical emergency-chord and unplug-while-held tests:** the logic is unit
    tested, but an unattended agent cannot press a chord entirely on one physical
    keyboard or unplug hardware. Run these with an operator before production use.

[1]: https://www.nuget.org/packages/Microsoft.GameInput?utm_source=chatgpt.com "NuGet Gallery | Microsoft.GameInput 3.4.259"
[2]: https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/input/gameinput/structs/gameinputdeviceinfo?utm_source=chatgpt.com "GameInputDeviceInfo - Microsoft Game Development Kit | Microsoft Learn"
[3]: https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/advanced/input-keyboard-mouse?utm_source=chatgpt.com "GameInput keyboard and mouse - Microsoft Game Development Kit | Microsoft Learn"
[4]: https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/input/gameinput/enums/gameinputfocuspolicy?utm_source=chatgpt.com "GameInputFocusPolicy - Microsoft Game Development Kit | Microsoft Learn"
[5]: https://learn.microsoft.com/en-us/gaming/gdk/docs/features/common/input/overviews/input-nuget?view=gdk-2604&utm_source=chatgpt.com "GameInput for PC and console with NuGet - Microsoft Game Development Kit | Microsoft Learn"
[6]: https://github.com/libsdl-org/SDL?utm_source=chatgpt.com "GitHub - libsdl-org/SDL: Simple DirectMedia Layer · GitHub"
[7]: https://wiki.libsdl.org/SDL3/SDL_HINT_WINDOWS_RAW_KEYBOARD?utm_source=chatgpt.com "SDL3/SDL_HINT_WINDOWS_RAW_KEYBOARD - SDL Wiki"
[8]: https://github.com/libsdl-org/SDL/issues/8756?utm_source=chatgpt.com "Mice with high polling rate causes significant lag on Windows · Issue #8756 · libsdl-org/SDL"
[9]: https://wiki.libsdl.org/SDL3/SDL_MouseMotionEvent?utm_source=chatgpt.com "SDL3/SDL_MouseMotionEvent - SDL Wiki"
[10]: https://wiki.libsdl.org/SDL3/SDL_KeyboardID?utm_source=chatgpt.com "SDL3/SDL_KeyboardID - SDL Wiki"
[11]: https://wiki.libsdl.org/SDL3/SDL_GetKeyboards?utm_source=chatgpt.com "SDL3/SDL_GetKeyboards - SDL Wiki"
[12]: https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input?utm_source=chatgpt.com "Raw Input Overview - Win32 apps | Microsoft Learn"
[13]: https://learn.microsoft.com/tr-tr/windows/win32/api/winuser/nf-winuser-registerrawinputdevices?utm_source=chatgpt.com "RegisterRawInputDevices function (winuser.h) - Win32 apps | Microsoft Learn"
[14]: https://github.com/icculus/manymouse "GitHub - icculus/manymouse: Simple, cross-platform library to handle multiple mice. · GitHub"
[15]: https://github.com/oblitum/Interception?utm_source=chatgpt.com "GitHub - oblitum/Interception: The Interception API aims to build a portable programming interface that allows one to intercept and control a range of input devices. · GitHub"
[16]: https://github.com/evilC/AutoHotInterception?utm_source=chatgpt.com "GitHub - evilC/AutoHotInterception: An AutoHotkey wrapper for the Interception driver · GitHub"
[17]: https://github.com/oblitum/Interception/issues/117?utm_source=chatgpt.com "Interception driver stops keyboard and mouse from working. · Issue #117 · oblitum/Interception"
[18]: https://github.com/microsoft/windows-driver-samples?utm_source=chatgpt.com "GitHub - microsoft/Windows-driver-samples: This repo contains driver samples prepared for use with Microsoft Visual Studio and the Windows Driver Kit (WDK). It contains both Universal Windows Driver and desktop-only driver samples. · GitHub"
[19]: https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/keyboard-input-wdf-filter-driver-kbfiltr/?utm_source=chatgpt.com "Keyboard Input WDF Filter Driver (Kbfiltr) - Code Samples | Microsoft Learn"
[20]: https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers?utm_source=chatgpt.com "Developing Keyboard and Mouse HID Client Drivers - Windows drivers | Microsoft Learn"
[21]: https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/virtual-hid-framework--vhf-?utm_source=chatgpt.com "Write a HID Source Driver by Using Virtual HID Framework (VHF) - Windows drivers | Microsoft Learn"
[22]: https://github.com/libusb/libusb/wiki/Windows?utm_source=chatgpt.com "Windows · libusb/libusb Wiki · GitHub"
[23]: https://docs.nefarius.at/projects/HidHide/FAQ/?utm_source=chatgpt.com "Frequently Asked Questions - Nefarius™ Project Documentation"
[24]: https://github.com/jkuhlmann/gainput "GitHub - jkuhlmann/gainput: Cross-platform C++ input library supporting gamepads, keyboard, mouse, touch · GitHub"
[25]: https://github.com/RawAccelOfficial/rawaccel?utm_source=chatgpt.com "GitHub - RawAccelOfficial/rawaccel: kernel mode mouse accel · GitHub"
[26]: https://cef-builds.spotifycdn.com/docs/150.0/classCefBrowserHost.html "CEF 150 CefBrowserHost reference"
[27]: https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/using_websocket.html "Boost.Beast WebSocket overview"
[28]: https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/ref/boost__beast__websocket__stream/async_write.html "Boost.Beast websocket::stream::async_write"
[29]: https://www.boost.org/doc/libs/latest/libs/beast/doc/html/beast/ref/boost__beast__websocket__stream/read_message_max.html "Boost.Beast WebSocket message-size limit"
[30]: https://github.com/feschber/lan-mouse "GitHub - feschber/lan-mouse"
[31]: https://github.com/input-leap/input-leap "GitHub - input-leap/input-leap"
[32]: https://datatracker.ietf.org/doc/html/rfc6455 "RFC 6455 - The WebSocket Protocol"
