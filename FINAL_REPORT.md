# Streaming Browser implementation report

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
- Archive size: 183,529,598 bytes
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
- Final hardening/report commit follows this report.
