# cm5audio

**A low-latency Linux audio patchbay with channel-level routing, a browser-based visual patchbay, and extensible C++ DSP chains.**

cm5audio routes and mixes audio between multiple local capture/playback devices, a built-in sine generator, and remote WebSocket audio clients. It is developed on a Raspberry Pi Compute Module 5, but the application is not tied to a particular audio board, manufacturer, or fixed input-to-output wiring arrangement. USB microphones, USB interfaces, onboard/I²S audio, and endpoints exposed by the selected Linux audio backend can participate, subject to their capabilities and the current engine limits.

The project combines a C++17 audio engine, miniaudio device access, an HTTP(S) control API, a WebSocket transport, and a dashboard served directly by the native application. Routing and DSP are controlled at runtime; adding a compiled-in processor does not require adding matching plugin-specific frontend code.

> **Deployment status:** active development. P0 control/adoption, persistence, endpoint ownership, and shutdown changes have passed the isolated plugin, sample-output, null-device lifecycle and plain/TLS process suites described below. The user also confirmed Ctrl+C now exits where Ctrl+\ was previously required, and runtime hot-apply/hot-save works in their normal setup. This is not comprehensive backend/hardware qualification; channel mapping, broader teardown scenarios and latency still need dedicated validation. Designed for a trusted development/LAN environment—not the public Internet. See [Security](#security-and-deployment-boundaries), especially the existing certificate/private-key download routes.

## Contents

- [Capabilities and boundaries](#capabilities-and-boundaries)
- [Audio format, timing, and limits](#audio-format-timing-and-limits)
- [Backend selection and device compatibility](#backend-selection-and-device-compatibility)
- [Dependencies and setup](#dependencies-and-setup)
- [Build and tests](#build-and-tests)
- [Running and configuration](#running-and-configuration)
- [Dashboard workflow](#dashboard-workflow)
- [DSP processors and extension](#dsp-processors-and-extension)
- [Routing architecture](#routing-architecture)
- [HTTP API reference](#http-api-reference)
- [WebSocket protocol](#websocket-protocol)
- [Persistence and restoration](#persistence-and-restoration)
- [TLS and browser microphone access](#tls-and-browser-microphone-access)
- [Security](#security-and-deployment-boundaries)
- [Source map](#source-map)
- [Diagnostics and troubleshooting](#diagnostics-and-troubleshooting)
- [Known limitations and roadmap](#known-limitations-and-roadmap)

## Capabilities and boundaries

### Implemented

- Enumerate and independently activate/deactivate multiple audio devices.
- Route individual source channels to individual destination channels.
- Fan out one source channel to several destinations, or sum several routes into one destination channel.
- Route local hardware, tone, and connected client endpoints through the same engine.
- Adjust route gain, enable/bypass a route, or remove its connection.
- Apply an ordered DSP chain to each route, independently of other routes.
- Discover available processors and their parameter metadata from C++ through `GET /api/dsp/available`.
- Generate plugin menus and typed parameter controls in the dashboard without duplicating processor definitions in JavaScript.
- Add, remove, and reorder processors; edit route/DSP settings as local drafts, discard on CLOSE/outside click, and submit one validated whole-route APPLY request.
- Display endpoint RMS/peak metering through the API and live patchbay meters.
- Generate a configurable sine tone for routing tests.
- Save accepted, engine-adopted routes/DSP, activated device IDs, tone, fixed global DSP, legacy hardware control banks, and known client names. Report adoption and persistence separately.
- Serve HTTP/HTTPS and WS/WSS, including an optional Emscripten/WASM browser audio terminal.
- Maintain browser client names and IDs in browser storage and reject simultaneous identity claims.

### Important distinctions

- **Device-agnostic does not mean unlimited compatibility.** The current routing width is eight channels; not every backend/device combination has been validated. See [Backend selection](#backend-selection-and-device-compatibility).
- **Capture is a source; playback is a destination**, from the patchbay's perspective. A USB microphone need not have a playback endpoint, and an output-only DAC need not have capture.
- There is **no automatic input-N-to-output-N connection**. Create the routes you want.
- The plugin system is a **compiled-in C++ registry**, not a VST/LV2 host, plugin-directory scanner, or runtime shared-library loader.
- Client media remains raw PCM over WebSockets. Versioned frames, sequence tracking, clock-drift handling, and network jitter control are future work.
- **Hardware channel gain/mute is not implemented in the sample path.** `POST /api/control` now returns **501** explicitly; it no longer acknowledges or stores a new inaudible change. Existing `AudioControls` banks are restored/preserved in settings for compatibility but do not affect samples. Use route gain or a Gain processor for operational level control.

## Audio format, timing, and limits

| Property | Current value / behavior |
|---|---|
| Language | C++17 |
| Internal sample format | Interleaved 32-bit float PCM |
| Engine sample rate | 48,000 Hz |
| Internal node width | 8 channels (`CM5_INTERNAL_WIDTH`) |
| Channel indices in APIs | Zero-based, `0..7`; dashboard labels are `CH1..CH8` |
| Engine processing block | **64 frames**, nominally **1.333 ms** at 48 kHz |
| Worker wake interval | Calculated from block/rate; integer-microsecond interval is currently 1,333 µs |
| Requested hardware period | 64 frames; backend may negotiate a larger actual period/buffer |
| Hardware ring-buffer requested capacity | 3 × engine block = 192 frames |
| Node handler/buffer slots | 256; tone at node 0, at most **255 distinct non-tone endpoint identities** per process lifetime |
| Route DSP slots | 1,024 array entries; supported route DSP IDs are `1..1023` |
| Processors per route | Maximum 16; discovery API advertises this limit |
| Media staging size | `CM5_MAX_AUDIO_FRAMES = 4320`; **not** the engine processing block |

These are implementation values, not latency guarantees. The 64-frame block is the low-latency target, not a statement that microphone-to-speaker latency is 1.333 ms. Backend buffering, sample/format conversion, scheduling, separate device clocks, and network transport add latency. For example, PulseAudio can accept a 64-frame request while reporting a larger actual buffer.

Hardware sample formats may differ from float32; miniaudio converts as needed. The application requests a 48 kHz client-facing device stream. The sample rate reported by device enumeration is capability metadata, not proof of the currently opened hardware format.

The current build is not an arbitrary-width engine. Devices with more than eight channels are not fully supported by the routing graph; do not assume all their channels become routable. Endpoint-to-node IDs survive disconnect/deactivation and are reused for the **same endpoint identity**, not recycled for unrelated identities. New node registration returns `NODE_INVALID` when the 255 non-tone slots are exhausted. A client normally needs two slots (capture/playback). Cycling through new client IDs can exhaust capacity; this is not an unlimited/recycling endpoint pool. Route IDs also increase without reuse; DSP is available only for IDs `1..1023`, even if earlier routes have been deleted.

## Backend selection and device compatibility

`DeviceManager` supplies this priority list to `ma_context_init`:

1. **JACK**
2. **PulseAudio**
3. **ALSA**

miniaudio selects the **first backend that initializes successfully**. It falls back only when an earlier backend fails. One context uses one selected backend; it does not merge the devices from all three backends or switch backends separately for each activation.

The constructor prints the candidate list and the selected backend. `MA_DEBUG_OUTPUT` in `src/miniaudio_impl.cpp` also enables detailed library/device logging. A failed load of an unversioned library name followed by a successful load of its versioned `.so` is not by itself an error.

A default `ma_context_config` and a null configuration both use miniaudio's default configuration values; constructing that object is **not** what enables fallback. The backend array and its count are the first two `ma_context_init` arguments. They are not members of `ma_context_config` in the vendored header.

### What devices can be used?

The intended scope is general Linux audio devices, not a specific sound card. Multiple mono, stereo, or multichannel capture/playback endpoints can be active together. Compatibility still depends on:

- The selected backend and its running server, permissions, and device visibility.
- Ability to open a stream compatible with the application's requested format/rate.
- Channel enumeration and mapping, including backend-provided conversions.
- Current eight-channel routing width and remaining lifecycle/clock handling limitations.

The current device layer retains Linux/ALSA-specific assumptions: ID serialization uses the `ma_device_id.alsa` union member and some device-name heuristics force eight channels. Virtual ALSA-name filtering and the `plughw` activation fallback are now restricted to the ALSA backend; the fallback is not attempted on JACK/PulseAudio. Prior PulseAudio operation is not comprehensive backend validation, and adding JACK to the candidate list is **not** comprehensive JACK integration. Backend-independent opaque IDs and physical device/channel testing remain hardening work.

PipeWire is not a separate candidate in this list. It may provide a PulseAudio- or JACK-compatible server, in which case the selected backend name remains PulseAudio or JACK. Device visibility can differ from `aplay -l`/`arecord -l`, which inspect ALSA rather than the chosen sound server.

There is no backend-selection command-line flag. Change the constructor's list/order if a different preference is required; changing a running sound server or `/etc` configuration is not necessary for ordinary plugin development. `DeviceManager(false)` and the application's **`--no-audio`** mode bypass context initialization entirely: no JACK, PulseAudio, ALSA, or miniaudio null backend is initialized, enumeration returns no hardware, and hardware activation fails. Tone, client routing, DSP, HTTP, and WS can still run for isolated checks.

## Dependencies and setup

### Native application

- Linux with an audio backend usable by miniaudio.
- C++17 compiler (`g++`), GNU Make, and pthreads.
- ALSA development headers/library (`libasound2-dev` on Debian/Ubuntu).
- OpenSSL development headers/libraries (`libssl-dev`).
- zlib development headers/library (`zlib1g-dev`).
- TLS-enabled IXWebSocket headers and the matching **static** library, normally installed under `/usr/local`; see the local compatibility objects below.
- nlohmann/json headers. The Makefile defaults to **`../json/include`** and accepts `NLOHMANN_JSON_INCLUDE` overrides.

miniaudio and cpp-httplib are vendored in `include/`. nlohmann/json and the full IXWebSocket distribution are external dependencies. Emscripten is needed only for the optional browser audio bundle. Node.js is needed only for dashboard tests; Python 3 and the `openssl` CLI are used by process-level reliability tests.

**Local IX compatibility objects:** the Makefile builds the matching `IXSocketOpenSSL.cpp`, `IXSocketServer.cpp` and `IXWebSocketServer.cpp` under `third_party/ixwebsocket/`, linking the three compatibility objects before `-lixwebsocket`. These narrowly patched sources match the installed static IX library used for development; they bound server TLS handshakes, fix accepted-descriptor ownership on failure/stop and retain immutable callbacks during shutdown to prevent a connection-churn `bad_function_call` race. This does **not** modify installed headers/libraries. It is not a general IX upgrade or a drop-in fix for arbitrary shared/static IX versions: compare source/ABI and matching TLS-enabled installed headers before changing IX. See `third_party/ixwebsocket/README.md` for provenance and limits. A separate project-owned `ReliableWebSocketServer` handles late accepted-worker shutdown ownership.

For Debian/Ubuntu, a manual prerequisite starting point is:

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config cmake git \
    libasound2-dev openssl libssl-dev zlib1g-dev nlohmann-json3-dev
```

If using distribution JSON headers, build with `NLOHMANN_JSON_INCLUDE=/usr/include`. If using a sibling nlohmann/json checkout, its layout should provide `../json/include/nlohmann/json.hpp`.

### Setup helper

From the repository root:

```bash
./scripts/setup.sh --install-ixwebsocket --no-wasm --host YOUR_SERVER_IP_OR_DNS
```

The helper installs/checks prerequisites, prepares development TLS files, and builds the native application. Without `--no-wasm`, it also builds the browser audio bundle. Its package list currently includes Emscripten even if WASM building is skipped; it does **not** cover all native dependencies above, notably ALSA/JSON. Install those separately when necessary.

| Option | Purpose |
|---|---|
| `--no-install` | Skip Debian package installation; still check required tools |
| `--no-build` | Skip the native build; use `--no-wasm` too if no builds are wanted |
| `--no-wasm` | Skip the browser bundle build |
| `--install-ixwebsocket` | Install IXWebSocket when `pkg-config` cannot find it |
| `--host IP_OR_DNS` | Select the development certificate subject/SAN |
| `-h`, `--help` | Show script usage |

The standalone IXWebSocket installer is:

```bash
./scripts/install_ixwebsocket.sh
```

It defaults to release `v12.0.1`, enables OpenSSL TLS/zlib, and installs under `/usr/local`. `IXWEBSOCKET_VERSION` and `IXWEBSOCKET_PREFIX` override these choices. With a custom prefix, adjust application include/link flags and `PKG_CONFIG_PATH` as appropriate; the Makefile does not automatically discover that prefix.

**Setup changes the machine:** it can install packages/libraries and create/regenerate TLS files. It is not required for every rebuild. Its TLS bundle contains the server private key; see [Security](#security-and-deployment-boundaries).

## Build and tests

### Build the native server

```bash
make -j2
```

Use a parallelism level appropriate for available memory. On a configured development machine, `make -j7` may also be appropriate; large C++ translation units can be memory-intensive.

With distribution JSON headers:

```bash
make -j2 NLOHMANN_JSON_INCLUDE=/usr/include
```

Clean application rebuild:

```bash
make clean
make -j2
```

`make clean` removes application objects/dependency files, the native server, plugin/reliability C++ test artifacts, and the local IX TLS/socket-server compatibility objects. It intentionally retains `build/miniaudio_impl.o` to avoid repeatedly compiling the large vendored implementation.

After changing miniaudio implementation macros or compiler settings affecting it, explicitly rebuild that object:

```bash
make clean-miniaudio
make -j2
```

Compiler-generated `.d` files track header dependencies, including processor definitions, for subsequent builds. These dependency files and build outputs live under ignored `build/`.

### Optional browser audio bundle

```bash
./web_client/build_wasm.sh
```

Requires `emcc` in `PATH`. Produces:

- `web_client/dist/client_audio.js`
- `web_client/dist/client_audio.wasm`

The script resolves the repository root automatically. Generated files are ignored by Git and must be rebuilt on a fresh checkout before using the browser audio terminal. The native dashboard itself does **not** require WASM and is plain HTML/CSS/JavaScript.

### Focused plugin tests

```bash
make test-plugins
```

This builds/runs C++ registry, parameter validation, processor behavior, chain-reader lifetime, HTTP discovery, and persistence tests, followed by Node.js dashboard DOM-logic tests.

Tests use temporary settings, loopback listeners on **28182/28183**, and `DeviceManager(false)`: no audio backend is initialized and no physical devices are enumerated/activated. They do not restart the normal server. These are not a complete visual browser, hardware lifecycle, or end-to-end latency test.

### P0 reliability and shutdown checks

```bash
make test-reliability
```

This target builds the application and both C++ reliability fixtures, runs the main C++ test under a 45-second timeout and null-device lifecycle checks under 20 seconds, then invokes `python3 tests/shutdown_test.py`:

- **C++ reliability:** PCM ring wrap/RAII; a deterministic publication-during-rebuild race; sample-level gain, DSP master gain, bypass and disabled-route behavior; endpoint IDs containing slashes; reconnect identity; full-route HTTP APPLY/validation/adoption; concurrent settings saves, save failure, refusal to save without adoption, and restoration into a fresh engine including global DSP and an offline endpoint route. It uses temporary settings and loopback ports **28184/28185**.
- **Python process integration:** launches only its own `cm5audio --no-audio` children with temporary settings/certificates and selected nonzero loopback ports. It checks real WS sample output, APPLY/bypass/disable/reconnect, repeated-hello restrictions, SIGINT/SIGTERM under traffic, idle/startup exits, plain/TLS listeners, stalled handshakes, invalid TLS and bind failure, and listener reuse. Each graceful-exit assertion has a five-second deadline; that is a test criterion, not a universal shutdown-time guarantee. TLS verification is disabled only inside this isolated self-signed test.

These plugin/reliability targets do not read normal settings, initialize physical audio backends, manage a service, or change system audio configuration. `tests/device_lifecycle_test.cpp`, included in `test-reliability`, exercises initialized/never-started/stopped devices, idempotent activation, exact IDs and retained owners using an explicitly selected **null backend**. It also substitutes a controlled blocking data loop to verify uninitialization invokes the backend wakeup after setting terminal state; the old uninit sequence hangs that test. Null is distinct from `DeviceManager(false)` and is not physical-backend qualification. The Python harness needs Python 3 and `openssl`; the C++ targets use `timeout`.

**Verification observed on this CM5 (2026-10-17):** `make test-plugins` and the full `make test-reliability` target passed. The latter includes deterministic PCM assertions, **72 concurrent HTTP saves**, an injected short write (`RLIMIT_FSIZE`) preserving the previous file, no-adoption/no-save, fresh restore, null-device lifecycle and the blocked-loop wakeup regression. Python recorded **21 graceful child-process exits**: loaded plain/TLS SIGINT/SIGTERM, ten connection-churn shutdowns, six idle launches and one early-startup exit, plus expected-failure invalid-TLS/bind checks. The final process matrix was also repeated three consecutive times (63 additional graceful exits). The slowest measured graceful exit was approximately **1.015 seconds**, below its five-second assertion; all tested listener ports were reusable. An additional ASan/UBSan build of the C++ reliability suite passed with no runtime sanitizer report (application sources instrumented; prebuilt miniaudio/IX objects were not). These results do not establish universal shutdown bounds or memory safety. Physical capture/playback, actual multichannel mapping, real-backend teardown, visual browser operation, overload behavior and measured end-to-end latency remain unverified.

**User-confirmed operation (2026-10-17):** after testing the rebuilt application in their normal setup, the user reported that a single **Ctrl+C** now exits where **Ctrl+\\** had previously always been required, and that **hot-apply/hot-save works**. This confirms the two originally reported failures are resolved in that tested setup. It is distinct from assistant-run isolated regressions and does not certify every device/backend, multichannel mapping, overload condition or latency measurement.

### Optional ALSA diagnostic tools

```bash
make alsatools
./alsa_caps
```

`alsa_caps` probes ALSA devices and prints native capability tables. Devices held by a sound server may report busy/restricted. The `alsa_bridge_test` target is an experimental standalone bridge with development-machine assumptions; inspect its source/device choices before running it. It is not the normal cm5audio server or a general hardware certification test. `make clean-alsatools` cleans these optional targets.

## Running and configuration

Start the process **from the repository root**, so relative web asset, certificate, and settings paths resolve correctly:

```bash
./cm5audio --plain-text
```

Or, with TLS certificate/key files present:

```bash
./cm5audio
```

Stop with Ctrl+C or SIGTERM. Signal handlers only flag shutdown and notify a nonblocking self-pipe; the lifecycle owner performs teardown outside signal/callback context. Do not assume stopping the process automatically saves unsaved changes. See [Ownership and shutdown](#ownership-and-shutdown).

For isolated API/tone/client testing without touching an audio backend or normal settings:

```bash
CM5AUDIO_SETTINGS=/tmp/cm5audio-isolated-settings.json \
CM5AUDIO_BIND_ADDRESS=127.0.0.1 \
CM5AUDIO_HTTP_PORT=28186 CM5AUDIO_WS_PORT=28187 \
./cm5audio --no-audio --plain-text
```

Choose unused ports and a disposable settings path. This is not a physical-audio test.

### Listener URLs

Replace `HOST` with your server's reachable address:

| Service | Plain text | TLS |
|---|---|---|
| Main dashboard | `http://HOST:8182/` | `https://HOST:8182/` |
| Browser audio terminal | `http://HOST:8182/client/index.html` | `https://HOST:8182/client/index.html` |
| Auxiliary device scan | `http://HOST:8182/client/scan.html` | `https://HOST:8182/client/scan.html` |
| Audio WebSocket | `ws://HOST:8183/ws/audio` | `wss://HOST:8183/ws/audio` |

The table shows defaults. `CM5AUDIO_HTTP_PORT`, `CM5AUDIO_WS_PORT`, and `CM5AUDIO_BIND_ADDRESS` override **8182**, **8183**, and **`0.0.0.0`** respectively. Port values are parsed as integers `0..65535`; use explicit nonzero WS ports. HTTP port 0 binds an ephemeral port and the listener log/`WebServer::http_port()` reports the result. **Do not rely on WS port 0 discovery:** IX `getPort()` may still return the requested 0 rather than the assigned port; tests deliberately select nonzero WS ports. The browser audio terminal still defaults its editable WS URL to port 8183—update it manually when overriding that port. There are no port CLI flags; accepted switches are `--plain-text` and `--no-audio`, and unknown arguments fail startup.

Without `--plain-text`, TLS is selected when both certificate and key files are readable; otherwise the application falls back to plain HTTP/WS. Invalid readable TLS credentials can fail startup rather than produce a usable TLS listener. Check the listener logs and actual sockets; a startup failure returns a nonzero exit status.

Only one instance should own the normal ports:

```bash
ss -ltnp | grep -E ':8182|:8183'
```

### Runtime environment

| Variable | Default | Used for |
|---|---|---|
| `CM5AUDIO_HTTP_PORT` | `8182` | HTTP(S) listener port; 0 requests an ephemeral HTTP port |
| `CM5AUDIO_WS_PORT` | `8183` | WS(S) listener port; use a nonzero value |
| `CM5AUDIO_BIND_ADDRESS` | `0.0.0.0` | Bind address for both listeners; `127.0.0.1` for isolated tests |
| `CM5AUDIO_SETTINGS` | `./cm5audio_settings.json` | Settings load/save path |
| `CM5AUDIO_TLS_CERT` | `./certs/server.crt` | HTTPS/WSS server certificate and certificate download |
| `CM5AUDIO_TLS_KEY` | `./certs/server.key` | HTTPS/WSS server key and **private-key download** |
| `CM5AUDIO_TLS_P12` | `./certs/server.p12` | PKCS#12 download file; not needed to enable HTTPS/WSS |

Example:

```bash
CM5AUDIO_SETTINGS=/path/to/cm5audio_settings.json \
CM5AUDIO_TLS_CERT=/path/to/server.crt \
CM5AUDIO_TLS_KEY=/path/to/server.key \
./cm5audio
```

Use writable settings directories. Absolute settings/certificate paths do not change the relative dashboard/static asset path requirement.

Setup-only variables include `CM5AUDIO_HOST` (default certificate address `192.168.168.172`) and `CM5AUDIO_P12_PASSWORD` (default `cm5audio`). They do not set the native server bind address, change engine sample rate, or select an audio backend. `etc/environment` and `etc/asound.conf` contain machine-specific examples; cm5audio does not automatically install/read them as application configuration. Do not copy them over system files without adapting them.

## Dashboard workflow

The main UI is **`web_client/dashboard.html`**, served at `/`. It has two tabs.

### Patchbay

1. Open **Hardware & DSP**, activate the devices to use, then return to **Patchbay**.
2. Capture endpoints appear as sources; playback endpoints appear as destinations. The tone generator is also a source.
3. Connect channels by dragging a source channel toward a destination channel, or selecting source then destination.
4. Drag endpoint headers to reposition nodes. Pan the canvas and use wheel/buttons to zoom; reset zoom or positions when needed.
5. Click a cable to open **Route Inspector & DSP Chain**.
6. Adjust route gain/enabled, active/bypass, DSP master gain, and ordered processors. These edits are **local drafts**. **CLOSE** or clicking outside the inspector discards the draft; none sends it to the server. Closing is disabled while APPLY/delete is in flight.
7. Click **APPLY** to submit the complete route/DSP draft in one request. The inspector closes only after successful engine-adoption acknowledgement and graph refresh; an unconfirmed/error result leaves it open for review.
8. Use top-level **SAVE** to persist the server's accepted, engine-adopted state. SAVE never reads or applies inspector fields, including an open or abandoned draft, or unapplied tone/global form values.

Multiple inputs routed to the same output are **summed**, not automatically normalized. Start with low levels: route gain, processor Gain, and DSP master gain can multiply together. A zero gain silences a route; 1 is unity.

DSP-active cables receive an effect highlight; disabled routes use a different color. Endpoint meters are polled approximately every 100 ms by the page. This is visual monitoring, not the engine audio processing interval.

Node positions are stored in that browser's `localStorage`, separately from server settings. A different browser or cleared site storage will not inherit the same layout. REFRESH updates hardware/graph/discovery; node positioning does not require a server-side layout API.

Inspector APPLY now uses **`POST /api/routes/{id}/apply`** with JSON `{gain,enabled,dsp}`. All route and DSP fields are validated, normalized, and processors constructed before publication; invalid DSP cannot leave an otherwise valid route-gain edit partially accepted. A successful response confirms a completed engine block has adopted the accepted revision, not that downstream hardware/network buffers are empty. Route and DSP are not promised to switch as one instantaneous sample-level transaction during the in-flight request. A **503 pending** is not a rollback: the accepted change may still apply later. Refresh/review authoritative state before retrying; closing afterward discards only the remaining local draft, not an already accepted request. Dashboard non-GET requests also use a promise queue so a subsequent SAVE cannot overtake an earlier APPLY from that page; a rejected request does not poison later queued requests. This is not cross-browser compare-and-swap. There is no draft/applied comparison modal yet.

### Hardware & DSP

- **Hardware Manager:** enumerate devices and activate/deactivate them.
- **Tone Generator:** enabled, frequency, amplitude, APPLY TONE.
- **Global DSP:** master gain and UPDATE DSP.
- **System Info:** endpoint and route counts.

The global UI currently exposes only global gain; global compressor threshold/ratio are API-configurable. Tone/global gain fields are not automatically hydrated from their GET APIs on dashboard load; use those APIs to inspect authoritative state. The current dashboard does not provide a general hardware per-channel mute/gain editor, metering history charts, or raw-value visibility toggle.

The auxiliary `/client/scan.html` page also lists/activates devices, but the dashboard Hardware Manager is the primary workflow. Its scan text still mentions ALSA even when another backend is selected.

## DSP processors and extension

### Built-in route processors

| Type | Function | Parameters (defaults; accepted ranges) |
|---|---|---|
| `gain` | Linear amplitude multiplier | `gain`: 1.0; 0..8 |
| `compressor` | Basic hard-knee, linear-threshold compression with smoothed gain reduction | `threshold`: 0.5; 0..1. `ratio`: 4; 1..100. `attack`: 0.01 seconds; 0.0001..10. `release`: 0.1 seconds; 0.0001..10 |
| `passthrough` | No sample processing | Empty `params` object |

EQ, limiter, and other processors are not implemented or advertised. The live discovery response is the authoritative list for the running binary.

**Threshold is linear amplitude, not dBFS.** For example, −20 dBFS corresponds to `0.1`, not `-20`. To convert an amplitude level from dBFS, use `10^(dBFS/20)`. Older invalid saved configurations are bypassed with a warning rather than silently executed.

The route signal path is conceptually:

```text
source channel
  -> ordered processor chain (when DSP active)
  -> route gain × DSP master gain (when DSP active)
  -> sum into destination channel
```

When DSP is bypassed, the chain and DSP master gain are bypassed; ordinary route gain remains effective. Route processors receive a **mono** block at the engine sample rate.

A separate fixed global Gain + Compressor stage runs on **hardware playback** after mixing; it does not run on client playback. Defaults are gain 1, threshold 0.7 linear, ratio 4. Its compressor is stateful and shared between hardware playback calls, so it is not an independently instantiated dynamics processor per hardware channel/device. Its settings are separate from the route registry and are now saved/restored in the optional top-level `global_dsp` settings object (`gain`, `threshold`, `ratio`); older files without it keep defaults.

### Add an effect without editing the dashboard

1. Implement an `AudioProcessor` class using the interface in `include/audio_processor.h`.
2. Keep its descriptor and factory with its implementation under `src/plugins/`, add a registration function, and include/call it from `src/plugins/register_builtin_processors.cpp` and the Makefile. Application-owned processors can instead register during startup before settings/worker threads.
3. Rebuild and restart the native program.
4. Reload the dashboard or refresh processor discovery. The menu and controls follow the backend metadata.

Supported metadata types are numeric, integer, boolean, string, and enum, with defaults and optional numeric bounds/increments/units. Saved current values come from the route API; defaults are used for new instances/missing parameter keys. Unknown parameters/types and invalid values are rejected server-side.

C++ cannot infer useful parameter names, units, or ranges from an arbitrary processing function: **the descriptor and factory are required**, but all of this plugin-specific knowledge stays in C++. A new separate source file must also be compiled/linked. Registration is startup-only; there is no runtime hot reload.

See **[docs/PLUGINS.md](docs/PLUGINS.md)** for descriptor fields, a C++ registration example, API details, concurrency rules, and migration behavior. `process()` implementations must not allocate, lock, throw, or perform I/O on the audio thread.

## Routing architecture

### Main components

| Component | Responsibility |
|---|---|
| `DeviceManager` | miniaudio context, enumeration, activation/deactivation, active hardware ownership |
| `HardwareDevice` | Separate capture/playback stream, hardware callback, PCM ring buffers |
| `ClientManager` | WebClient sessions, endpoint-to-node mapping, immutable route snapshots, persistence |
| `DspEngine` | Graph rebuild/order, capture, tone, per-route processing/summing, playback, metering |
| `ProcessorRegistry` | Processor descriptors, discovery, default filling, validation, construction (`include/processor_registry.h`, `src/processor_registry.cpp`) |
| `WebServer` | HTTP control/static files, WebSocket sessions, canonical route DSP cache |

Normal startup enumerates hardware, constructs/links the engine, restores settings and processor chains, then starts DSP and web service threads. `--no-audio` skips backend initialization/enumeration and cannot activate restored hardware, while retaining logical settings/routes. DSP must exist before settings restoration to apply saved route chains and global DSP.

### Graph and processing

- Routes reference numeric `NodeId` values internally; HTTP/settings use endpoint strings.
- A dirty flag plus monotonically increasing requested revision triggers graph rebuilding with **Kahn's topological sorting algorithm**. A publication during rebuilding forces a subsequent pass instead of being lost when the current rebuild finishes.
- A fixed-size handler array resolves node handlers during processing.
- Node and route scratch buffers are preallocated; per-source adjacency lists avoid scanning all routes for every node.
- Audio is processed in 64-frame blocks. For fixed block size, routing work scales with active connections; sample work is proportional to routes × frames plus processor cost.
- A node without a bound physical/client handler can become an internal virtual handler. There is no public virtual-bus creation/list/editor API yet.
- Route DSP uses **three preallocated state slots** with audio-reader pinning and serialized control-thread writers. Each slot contains the chain **and** DSP master gain/active state; these are published together. Processors are constructed/destroyed on control threads, not while executing the chain. Ordinary route gain/enabled still reside in the separate immutable route snapshot.
- Disabled/invalid routes do not enter execution-order dependencies. Active media-ready client endpoints and active hardware are included even without an enabled route so playback can receive silence. Short capture reads are zero-filled rather than replaying stale samples.
- After a full block completes, the engine publishes `adopted_revision` and wakes control-side waiters. POST writers are serialized through this barrier; GET snapshots remain concurrent (the graph GET also takes the control mutex).

This is a DAG-oriented engine, not a guaranteed arbitrary feedback graph. Feedback pools and delay-selection code exist, but cycles are not removed from Kahn's dependency graph before sorting; cyclic nodes can be omitted from execution. **Do not rely on feedback loops as a supported feature.** The current topological ready queue selects the lowest available numeric NodeId first; this does not make cycles supported.

Preallocated buffers and safe publication are improvements, **not** a claim of zero-allocation, lock-free, or hard real-time processing. Graph rebuilding still happens from `process_audio()` and allocates/locks. Meter-map structure is now guarded by a shared **mutex** in both engine updates and API readers/reset; channel values remain atomic, but inserting an endpoint may allocate. Session/device snapshots and retirement queues also use locks/containers.

PCM rings are noncopyable RAII-owned SPSC buffers. Reads/writes handle wraparound and bound work to available complete frames without application-side allocation/locks. Both producer and consumer must be quiesced before init/uninit/destruction. This narrower property does not make the entire audio engine lock-free; network framing/jitter/drift work is still outstanding.

### Ownership and shutdown

`WebServer` has one serialized lifecycle/shutdown owner. Teardown stops HTTP acceptance and cancels accepted stalled/trickling socket reads through the project-owned `ReliableHttpServer`, then joins HTTP workers while DSP is still available to finish accepted mutations, stops/joins DSP, shuts down WS acceptance/workers, joins the session reaper, and drains session/endpoint retirement. An IX close callback only marks/removes the session and queues retirement: it never joins its own sender. A separate reaper joins retired senders and reclaims endpoint owners outside IX callback/transport locks.

Graph snapshots retain shared hardware/session owners for their raw handler pointers until a later graph or stop releases them. Retired owners are collected on a control thread, not destroyed on the audio worker. Hardware buffers are prepared after channel negotiation but **before** device start; initialized devices are uninitialized even if start failed. Device stop/destruction happens outside the manager lock. A local vendored-miniaudio uninit fix wakes synchronous backend data loops (notably ALSA poll) after setting terminal state, before joining; it avoids a preliminary `ma_device_stop()` wait when backend/state can disagree. Main releases the engine/graph owners before device/context teardown. The local IX TLS/socket-server objects and `ReliableWebSocketServer` address stalled handshakes, accepted-descriptor ownership and late accepted workers. Accepted HTTP connections fail closed if tracking-descriptor allocation fails; both normal stop and startup rollback cancel accepted reads before joining. These are implemented ownership rules, not a claim that physical-backend shutdown has been fully validated.

## HTTP API reference

**Base URL:** `http://HOST:8182` or `https://HOST:8182` according to runtime mode. All API responses are JSON; web/download responses use their appropriate content types.

Unless specifically marked **JSON body**, POST handlers read URL query or form parameters through cpp-httplib. Sending a JSON body to `/api/tone` or `/api/routes` is not equivalent. Whole-route APPLY and route DSP POST accept JSON bodies.

Gains are linear amplitude multipliers, not decibels. Channels are zero-based. Use the exact IDs returned by enumeration/graph; names are display labels, not stable identifiers. APIs do not provide application authentication.

### Mutation completion and save contract

Successful POST handlers run under the shared recursive **control mutex**, covering mutation, adoption wait, and any requested persistence. They request a fresh engine revision and wait up to **1,500 ms** for a complete processing block to acknowledge it (or fail earlier if the engine stops). Responses from accepted POSTs include:

| Field | Meaning |
|---|---|
| `accepted_revision` | Target revision accepted/requested for this operation |
| `adopted_revision` | Highest revision acknowledged after a completed engine block |
| `adopted` | Whether adoption of this request's target was confirmed |
| `persisted` | Whether this request successfully wrote settings; false for ordinary APPLY |
| `persisted_revision` | Last revision successfully saved through this WebServer; initially 0 |

A normal APPLY is **200**, `status:"ok"`, `adopted:true`, `persisted:false`: applied is not saved. Device activation/deactivation also save **after** adoption. Explicit SAVE waits for adoption and writes the accepted server state, never browser drafts.

- **400/404**: validation/lookup rejection where documented; full-route APPLY validates the whole request before changing route/DSP state.
- **503**, `status:"pending"`, `adopted:false`, `persisted:false`: the request was accepted but adoption was not confirmed within the wait. **It may apply later; this is not cancellation or rollback.** Refresh `/api/graph` and route DSP before retrying. A stopping server can also reject a new request with 503 without acceptance fields.
- **500**, `adopted:true`, `persisted:false`: accepted state is applied, but SAVE (or device-change autosave) failed. Fix the storage issue and retry SAVE; the live change was not rolled back.
- SAVE **does not call the serializer** if adoption was not confirmed. It must not report success or replace the file in that case.

Graph GET exposes real `revision` (the requested/accepted engine revision), `adopted_revision`, and `persisted_revision`. These are process-local progress counters, not persisted scene versions, ETags, a compare-and-swap concurrency API, or proof that a physical device has rendered/flushed the block. Revisions can advance for barriers and endpoint lifecycle changes, not only visible route edits. They reset with a new process; restored settings do not initialize `persisted_revision` to a historical disk revision. Legacy route DELETE also waits for adoption and returns accepted/adopted fields and `persisted:false`, but currently omits `persisted_revision`; read it from the graph if needed. Validation/unsupported responses need not include completion fields.

### Pages, static assets, and credential downloads

| Method | URL/path | Behavior |
|---|---|---|
| GET | `/` | Main dashboard HTML |
| GET | `/favicon.ico` | Dashboard icon |
| GET | `/client/*` | Static mount of `./web_client`; includes the pages/assets below and any other present files |
| GET | `/client/dashboard.html` | Same dashboard file through the static mount |
| GET | `/client/index.html` | Browser audio terminal |
| GET | `/client/scan.html` | Auxiliary device scan page |
| GET | `/client/dist/client_audio.js` | Generated browser bridge JavaScript |
| GET | `/client/dist/client_audio.wasm` | Generated WASM module |
| GET | `/client/favicon.ico` | Icon through the static mount |
| GET | `/server.crt` | Configured server certificate download; 404 if unavailable |
| GET | `/server.key` | **Configured server private-key download, unauthenticated**; 404 if unavailable |
| GET | `/server.p12` | **PKCS#12 bundle download, potentially containing the server key**; 404 if unavailable |

`/client/*` is a directory mount, not a whitelist. Source/build scripts/backups placed in `web_client/` may also be served. Remove/block unwanted files and protect credential routes before wider deployment. Credential routes remain registered in plain-text mode too.

### Device and endpoint discovery

| Method | Endpoint | Parameters / returned data |
|---|---|---|
| GET | `/api/devices` | Re-enumerates selected-backend devices; array of `{id,name,is_capture,channels,sample_rate}` |
| POST | `/api/devices/activate` | Required `id`; opens matching capture/playback entries, registers stable nodes, waits for adoption then saves; activation failure 400, adoption pending 503, applied/save-failed 500 |
| POST | `/api/devices/deactivate` | Required `id`; removes active bindings while preserving logical node IDs/routes, waits for adoption then saves; completion/error fields as above |
| GET | `/api/clients` | `{clients:[...]}` with session `id`, `client_id`, `name`, `ip`, `input_channels`, `output_channels`, `input_meters`, `output_meters` |
| GET | `/api/graph` | `{revision,adopted_revision,persisted_revision,endpoints:[...],routes:[...]}`; endpoint IDs/names/directions/channels, connected flags for known clients, route state including `dsp_active` |

`revision` is now the real changing engine requested revision, not the old hardcoded 1; adoption and persistence can lag it. Tone has no explicit `name` field in the graph; clients should tolerate optional endpoint fields. Disconnected known clients are advertised with placeholder counts (1 input/2 output), not persisted negotiated channel counts. Enumeration channel count 0 means unknown/native selection; it does not mean the device has no channels.

Repeated activation succeeds when the matching device is already active instead of requiring a new stream. Device state autosave is serialized after adoption and **does report** failure: 503 if adoption is unconfirmed (no save), or 500 if applied but writing settings failed. A 200 acknowledges both adoption and persistence, not physical listening/channel verification.

### Routes

| Method | Endpoint | Parameters / behavior |
|---|---|---|
| POST | `/api/routes` | `source_node`, `destination_node` **endpoint strings**; `source_channel`, `destination_channel` 0..7; optional `gain` (default 1, finite 0..8), `enabled` (default true) |
| POST | `/api/routes/update` | Legacy route-only update: required `id`; `gain` default 1, finite 0..8; `enabled` default true. Missing values reset to defaults, not preserve old values. Failure 400 |
| POST | `/api/routes/{id}/apply` | **JSON body** `{gain,enabled,dsp}`; whole inspector request validated before route/DSP publication, then engine-adoption acknowledgement; does not save |
| DELETE | `/api/routes/{id}` | Delete route and cached DSP configuration; 404 if not found |

Route creation returns `status:"ok"`, `id:"X"` plus the shared completion fields: the creation response ID is currently a **string**, while graph/settings route IDs are numbers. Duplicate source-channel/destination-channel connections are rejected with 400. Graph routes use keys `source` and `destination` for endpoint strings, not the POST parameter names `source_node`/`destination_node`.

Route creation rejects invalid/unregistered numeric nodes, out-of-range internal channels, duplicate edges, invalid gain, and exhausted route IDs. Fixed node registration is bounded. This is still **not** full endpoint-capability or graph validation: channels are checked against the internal width, not every endpoint's actual channel count/direction, and enabled cycles are not comprehensively rejected. Prefer known source/destination endpoints from `/api/graph` and avoid cycles. There is no separate `GET /api/routes`; obtain routes through `/api/graph`.

Endpoint naming:

```text
hardware/<device-id>/capture
hardware/<device-id>/playback
client/<client-id>/capture
client/<client-id>/playback
tone/generator
```

### DSP discovery and per-route chains

| Method | Endpoint | Parameters / returned data |
|---|---|---|
| GET | `/api/dsp/available` | `{schema_version:1,max_chain_length:16,processors:[...]}`; each processor includes `type,name,description,defaults,parameters`; `Cache-Control: no-store` |
| GET | `/api/routes/{id}/dsp` | `{active,gain,chain}`; optional `warning` for bypassed invalid saved configuration |
| POST | `/api/routes/{id}/dsp` | **JSON body** `{active:bool,gain:number,chain:[{type:string,params:object},...]}`; replaces whole DSP state |

Each processor must include `params` (use `{}` for all defaults). Omitted keys inside it are filled from descriptors. Unknown types/keys, invalid parameter types/ranges, chains longer than 16, and invalid master gain fail with 400 before altering the prior configuration. Missing top-level fields default to `active:false`, `gain:1`, `chain:[]`. DSP gain must be finite and 0..8.

A valid DSP-only update returns `{status:"ok",active,gain,chain}` plus the shared completion fields, with canonical/default-filled parameter values; 200 confirms adoption, not persistence. Malformed/zero/out-of-range numeric IDs return 400; nonexistent routes return 404; existing routes outside the supported DSP ID range return 400. Supported DSP IDs are currently `1..1023`.

Whole-route `/apply` requires numeric route `gain` (finite 0..8), boolean `enabled`, and a `dsp` object using the schema above. Validation of all route/DSP fields and processor construction precede publication; 400 leaves both field groups unchanged. It returns canonical DSP fields at the top level (`active`, DSP `gain`, `chain`), plus `route_gain`, `enabled`, and completion fields. Missing route is 404; the same DSP ID capacity applies. The route snapshot and DSP slot are separate publications while the request is in flight; the final adoption barrier confirms completion rather than promising a single instantaneous sample-level swap of both.

See [docs/PLUGINS.md](docs/PLUGINS.md) for the full metadata schema and typed controls.

### Tone and fixed global DSP

| Method | Endpoint | Parameters / returned data |
|---|---|---|
| GET | `/api/tone` | `{enabled,frequency_hz,amplitude}` |
| POST | `/api/tone` | Optional `enabled`, `frequency` 1..20000 Hz, `amplitude` 0..1; invalid numeric values 400 |
| GET | `/api/dsp` | Fixed global hardware-playback `{gain,threshold,ratio}` |
| POST | `/api/dsp` | Optional finite `gain >= 0`, `threshold >= 0`, `ratio >= 1`; invalid values 400 |

Tone POST uses **`frequency`**, while GET/settings use **`frequency_hz`**. Although the tone graph endpoint advertises eight channels, the current sine generator writes only **channel 0 (CH1)**. Route that channel to any desired outputs.

Global DSP is separate from per-route plugin chains and is now saved/restored under optional `global_dsp`. Tone and fixed-global POST handlers gather and validate values before storing fields, then use the shared adoption barrier. Numeric query helpers retain permissive legacy parsing: some unparsable text can fall back to the prior/default value rather than fail. Do not infer strict JSON typing or compare-and-swap semantics from these query endpoints. The global Gain/Compressor parameters remain individual atomic controls, not a dynamic registry rack.

### Metering, unsupported hardware controls, and settings

| Method | Endpoint | Parameters / returned data |
|---|---|---|
| GET | `/api/meters` | `{capture:{endpoint:[meters...]},playback:{endpoint:[meters...]}}` |
| GET | `/api/metrics` | Alias of `/api/meters` |
| GET | `/api/raw` | Same capture/playback map shape, each channel `{ch,value,peak}` |
| POST | `/api/control` | **501 unsupported** for hardware channel gain/mute; no new control-bank mutation or sample effect. Use route gain or a Gain processor |
| POST | `/api/reset_clips` | `all=1`, or required `endpoint`, `type`, `ch`; resets `clipped=false`, `peak_hold_db=-60` |
| POST | `/api/settings/save` | No required body; waits for engine adoption, then serializes accepted applied state. 200 adopted+persisted; 503 pending/no save; 500 applied but save failed |

Meter entries contain `ch`, `raw_value`, `raw_peak`, `rms_db`, `peak_db`, `peak_hold_db`, `clipped`. `raw_value`/`value` is **block RMS**, and `raw_peak`/`peak` is **block absolute peak**, not a stream of individual raw samples. `/api/meters` and `/api/raw` iterate existing metric maps, not a strict active-device snapshot, and emit eight entries per stored endpoint bank. Session meter arrays are also returned by `/api/clients`, but the engine currently updates endpoint metric maps rather than those session arrays; use endpoint maps for engine metering.

Peak-hold and clip fields are part of the API, but continuous peak-hold accumulation and latched clipping are not implemented in the current meter update. `clipped` reflects the most recently updated block's peak `> 1.0`. Active hardware/client capture short reads are zero-filled and metered as silence; a disconnected or no-longer-updated endpoint can leave old entries in the metric maps. RESET CLIPS is not a substitute for removing clipping from the signal path.

General tone/route query helpers treat `1`, `true`, and `on` as true. JSON route/DSP controls require actual booleans. `/api/reset_clips` resets all banks whenever the **`all` parameter is present**, even `all=0`. Meter-map access is mutex-protected in readers, reset, and engine updates; this is not lock-free telemetry.

### API examples

Examples modify the running configuration. Use a trusted host, low output levels, and endpoint IDs obtained from your own graph. For HTTPS, replace the base URL and configure certificate trust instead of disabling TLS verification.

```bash
BASE=http://127.0.0.1:8182
curl -fsS "$BASE/api/devices"
curl -fsS "$BASE/api/graph"
curl -fsS "$BASE/api/dsp/available"
```

Activate a device, then create a tone route to a chosen playback endpoint:

```bash
curl -fsS -X POST "$BASE/api/devices/activate" \
  --data-urlencode 'id=YOUR_DEVICE_ID'

curl -fsS -X POST "$BASE/api/routes" \
  --data-urlencode 'source_node=tone/generator' \
  --data-urlencode 'destination_node=hardware/YOUR_DEVICE_ID/playback' \
  --data-urlencode 'source_channel=0' \
  --data-urlencode 'destination_channel=0' \
  --data-urlencode 'gain=0.1' \
  --data-urlencode 'enabled=1'

curl -fsS -X POST "$BASE/api/tone?enabled=1&frequency=440&amplitude=0.05"
```

Use the returned route ID (substitute it for `1` here):

```bash
curl -fsS -X POST "$BASE/api/routes/1/apply" \
  -H 'Content-Type: application/json' \
  --data '{"gain":0.1,"enabled":true,"dsp":{"active":true,"gain":1,"chain":[{"type":"gain","params":{"gain":0.5}},{"type":"compressor","params":{"threshold":0.1,"ratio":4,"attack":0.01,"release":0.1}}]}}'

curl -fsS "$BASE/api/routes/1/dsp"
curl -fsS -X POST "$BASE/api/tone?enabled=0"
curl -fsS -X POST "$BASE/api/settings/save"
```

Inspect mutation JSON as well as HTTP status: APPLY should report `adopted:true`; SAVE must report both `adopted:true` and `persisted:true`. On 503, refresh instead of blindly repeating a route create; on 500 save failure, the live accepted state is still applied.

## WebSocket protocol

**Default port:** 8183 (override with `CM5AUDIO_WS_PORT`; use nonzero ports). The browser terminal uses `/ws/audio`; the current server callback does not enforce a specific request path. Use the conventional URL rather than assuming a REST endpoint on that port.

### Text identity messages

Client sends:

```json
{
  "type": "hello",
  "client_id": "stable-client-id",
  "client_name": "Desk microphone",
  "input_channels": 1,
  "output_channels": 2
}
```

`input_channels` means **client to server** (client capture); `output_channels` means **server to client** (client playback). Counts are clamped to 1..8, not zero-channel modes. `client_key` and `name` are accepted aliases for `client_id` and `client_name`.

Server messages:

- `assigned`: sent only after the WebSocket **Open** event/successful upgrade creates the application session; includes `connection_id`, provisional `client_id`, and `name`. A TCP accept or failed/stalled handshake creates no application session/sender.
- `identity_accepted`: includes accepted `client_id` and `name`.
- `identity_conflict`: includes the already claimed `client_id`.

The bundled page stores identities in `localStorage`. A duplicate active claim prompts the user before creating a new ID. This is naming/session coordination, **not authentication or secure identity ownership**.

### Binary media

Payloads are raw interleaved float32 PCM. There is no application header/version, sequence number, timestamp, stream ID, explicit frame count, or byte-order marker. Current deployment assumes the engine's 48 kHz format and compatible float representation/byte order.

The bundled terminal sends a mono microphone stream and receives stereo playback. Its WASM network tick uses 480-frame/10 ms capture packets; this is separate from the server's 64-frame processing block. The page sends `sample_rate:48000` and `format:"f32"` in its hello, but the server currently **does not negotiate or validate those fields**, nor echo final channel/rate parameters in `identity_accepted`.

The first valid hello initializes PCM rings once, starts the sender, claims the endpoint identity, then publishes `media_ready`. Binary media before readiness is ignored. Once ready, a repeated hello with the **same identity and clamped channel counts** is acknowledged without reinitializing buffers. Changing identity or channel counts returns `identity_conflict` with a reconnect instruction; a ready session is not resized/renamed in place (a new display name on an otherwise identical hello is not adopted). Duplicate identity or endpoint-capacity failure also rejects the claim and closes that connection.

Endpoint numeric IDs and route endpoint strings survive disconnect; graph handler binding refreshes on reconnect with the same client identity. Senders are retired/joined outside IX callbacks. Transport still lacks framing, jitter policy, underrun/overrun statistics, backpressure diagnostics, rate/format negotiation, and clock-drift compensation. Do not equate WebSocket reliability with bounded real-time latency.

## Persistence and restoration

Default file: **`./cm5audio_settings.json`**, overridden by `CM5AUDIO_SETTINGS`.

Stored top-level data:

| Key | Contents |
|---|---|
| `version` | Current settings marker, 1; not a full migration framework |
| `activated_devices` | Device IDs to attempt to activate at startup |
| `tone` | Enabled, `frequency_hz`, amplitude |
| `global_dsp` | Optional fixed hardware-playback DSP `gain`, `threshold`, `ratio`; older files without it keep defaults |
| `hardware.capture`, `hardware.playback` | Endpoint/channel stored gain/mute banks |
| `routes` | Route IDs, endpoint strings, channels, gain, enabled, optional DSP state/chain |
| `clients` | Known client IDs mapped to display names |

Save is serialized under the control mutex with snapshot construction and other accepted mutations. It creates a **unique same-directory `PATH.tmp.XXXXXX`** file with `mkstemp`, checks the complete write loop (including short writes/EINTR), checks file `fsync` and `close`, then checks rename over the destination. Failure returns false and removes the temporary file where possible; the old destination remains until a successful rename. Unique names prevent temporary-file collisions, but do not coordinate multiple independent processes writing the same destination—use one owner.

**Durability boundary:** there is no parent-directory `fsync`, crash/power-loss rollback, previous-good snapshot selection, or automatic `.bak` recovery. A successful API save is not a guarantee that the rename survives a sudden power loss. Crash rollback and a draft/applied comparison modal are deferred requirements, not P0 features. Back up important configurations yourself.

- Top-level **SAVE** / `POST /api/settings/save` waits for adoption and persists accepted applied state only. No adoption means 503 and **no file write**; an I/O failure after adoption means 500, not a false success or live-state rollback.
- Device activation/deactivation also saves after adoption, potentially including other already accepted changes; failures are surfaced in the response.
- Route create/update/delete/APPLY, route DSP, tone, and fixed global controls do not automatically save. Apply first, then SAVE. Browser-local drafts, including abandoned inspector edits, are never serialized.
- Global DSP **is** persisted. Browser canvas layout, browser identity storage, and negotiated client channel counts are **not** part of server settings persistence.
- Legacy hardware gain/mute banks are loaded and preserved on save but remain inaudible; `/api/control` rejects new edits with 501.
- Invalid/unavailable processor chains are preserved and bypassed with a warning. Correct/remove the offending processor and APPLY; recreating the route is also possible, but discards the old chain.
- There is no settings import/reload HTTP endpoint. Change an offline file carefully and restart the application to load it.

**Endpoint restoration:** loading registers nonempty saved source/destination strings even when endpoints are offline, instead of resolving them to an invalid transient node. A same-identity reconnect/reactivation binds the preserved node to its new handler; full endpoint strings (including embedded slashes) are matched. Offline source buffers start silent. Invalid/out-of-width/out-of-capacity saved routes are skipped. This is finite process-local identity mapping, not universal hardware hotplug discovery or backend-independent device IDs. Verify physical device IDs and routes after changing backend/hardware; disconnected known-client channel counts remain placeholders until hello.

If you want to test without your normal wiring, stop the application and use a separate settings path rather than deleting a working configuration. A missing settings file starts without restored routes.

## TLS and browser microphone access

The dashboard can be used over plain HTTP on a trusted LAN. Browser microphone access is different: `getUserMedia()` generally requires a **secure context** (HTTPS or localhost) and explicit browser permission. A LAN IP served over HTTP is usually not a secure context.

### Prepare a development certificate

```bash
./scripts/setup.sh --no-install --no-build --no-wasm --host YOUR_SERVER_IP_OR_DNS
```

The helper validates existing certificate/key files and their host/SAN, regenerating them if needed. It creates `certs/server.crt`, `certs/server.key`, and a PKCS#12 bundle. Override `CM5AUDIO_P12_PASSWORD` if creating the bundle; default password is `cm5audio`. **The bundle contains the server private key and is not a safe general-purpose client trust distribution format.**

Start normally (without `--plain-text`) and use `https://HOST:8182/client/index.html` with `wss://HOST:8183/ws/audio`.

### Trust a self-signed certificate on Ubuntu/Chromium

Copy **only the public certificate** and the trust helper to the browser machine via a trusted channel. For example, adjust server username/path/host for your installation:

```bash
scp USER@HOST:/path/to/cm5audio/certs/server.crt /tmp/cm5audio.crt
scp USER@HOST:/path/to/cm5audio/scripts/trust_server_cert.sh /tmp/trust_server_cert.sh
chmod +x /tmp/trust_server_cert.sh
/tmp/trust_server_cert.sh /tmp/cm5audio.crt
```

Run the helper as the **normal desktop/browser user**, not root and not after `sudo su -`. It uses sudo for the system store and imports peer trust into that user's NSS database. If `certutil` is missing:

```bash
sudo apt-get install libnss3-tools
/tmp/trust_server_cert.sh /tmp/cm5audio.crt
```

Compare the certificate fingerprint over a trusted channel before trusting it. Fully exit/restart Brave/Chromium, open the HTTPS terminal, allow microphone access, and click START AUDIO. Certificate trust behavior varies by OS/browser; importing a certificate is not a universal guarantee of WebSocket/browser acceptance.

Verify HTTP certificate trust independently:

```bash
curl -fsS https://HOST:8182/ -o /dev/null \
  -w 'HTTPS trust works: HTTP %{http_code}\n'
```

Port 8183 is a WebSocket service, not an ordinary webpage. Opening `https://HOST:8183/` in the address bar can produce HTTP/protocol errors and is not a valid UI test.

### Development-only Chromium override

For an isolated plain-text test, Chromium has:

```text
chrome://flags/#unsafely-treat-insecure-origin-as-secure
```

Add the exact dashboard/client origin, such as `http://192.168.168.172:8182`, relaunch the browser, and use WS rather than WSS. This weakens browser security and should not be a normal deployment strategy. Never use it as an alternative to protecting an exposed server.

## Security and deployment boundaries

**Current application authentication/authorization: none.** Anyone who can reach the listeners can potentially inspect/control routing, change settings, connect a media client, or claim an unused client identity. Both listeners default to all IPv4 interfaces (`0.0.0.0`); set `CM5AUDIO_BIND_ADDRESS=127.0.0.1` for local isolation when appropriate.

**Critical existing credential exposure:** `GET /server.key` serves the configured TLS private key without authentication, and `GET /server.p12` serves a bundle which the setup script creates with that key. The browser terminal currently links to these downloads. HTTPS does not protect the key from a requester authorized only by network reachability; plain-text mode leaves the same routes available over HTTP. A known bundle password is not a meaningful access boundary.

Before use beyond an isolated trusted development environment:

- Remove/disable or explicitly block private-key/bundle routes, including at any directly reachable listener—not merely a proxy the client can bypass.
- Rotate server credentials if they may have been downloaded by untrusted parties.
- Distribute only the public certificate/CA information to clients; never the server private key.
- Add appropriate authentication, authorization, origin/access policies, rate/size limits, and network isolation.
- Audit the broad `/client` static mount and remove source, backup, or other unintended downloadable files.
- Protect settings files, prevent accidental remote overwrites, and use a least-privileged service account with the required audio access.
- Do not assume a reverse proxy for HTTP automatically handles the separate WebSocket port or solves direct-port access.

The supplied files and helpers are development conveniences, not a production security configuration. Root is not an inherent application requirement, although your chosen audio server/device and deployment permissions must permit access. Backend selection under root may differ from the desktop user's audio session.

## Source map

```text
cm5audio/
├── include/
│   ├── constants.h                  Block size, channel width, staging/processor limits
│   ├── device_manager.h             Hardware/context ownership declarations
│   ├── audio_processor.h            Native processor interface
│   ├── processor_registry.h         Processor descriptors and registry API
│   ├── plugins/                     Built-in processor interfaces
│   ├── dsp_engine.h                 Routing engine state and processor publication
│   ├── web_server.h                 Routes, sessions, meters/controls, server declarations
│   ├── reliable_websocket_server.h  Single-owner IX accepted-worker shutdown wrapper
│   ├── reliable_http_server.h  Accepted HTTP socket cancellation during shutdown
│   ├── ring_buffer.h           miniaudio PCM ring-buffer wrapper
│   ├── miniaudio.h             Vendored audio implementation/API
│   └── httplib.h               Vendored HTTP implementation/API
├── src/
│   ├── main.cpp                Startup/link/restore order, signals, server ports
│   ├── device_manager.cpp      Backend priority, enumeration, activation, callbacks
│   ├── dsp_engine.cpp          Graph rebuilding, processing, DSP publication, meters
│   ├── processor_registry.cpp  Discovery, validation and construction
│   ├── plugins/                Built-in processor implementations and registration
│   ├── web_server.cpp          API handlers, settings, registry bridge, WebSockets
│   ├── miniaudio_impl.cpp      Single native miniaudio implementation + debug output
│   ├── alsa_caps.cpp           Optional native ALSA capability probe
│   └── alsa_bridge_test.cpp    Experimental standalone bridge
├── web_client/
│   ├── dashboard.html          Main visual patchbay and dynamic processor editor
│   ├── index.html              Optional browser audio terminal
│   ├── client_main.cpp         Emscripten/miniaudio PCM bridge
│   ├── scan.html               Auxiliary device enumeration UI
│   ├── build_wasm.sh           Browser bundle build
│   ├── favicon.ico             UI icon
│   └── dist/                   Generated JS/WASM (ignored)
├── scripts/
│   ├── setup.sh                Dependency checks, development TLS, builds
│   ├── install_ixwebsocket.sh  TLS-enabled IXWebSocket installer
│   └── trust_server_cert.sh    Desktop system/NSS certificate helper
├── third_party/ixwebsocket/     Matching local TLS/socket ownership patches, provenance/license
├── docs/PLUGINS.md              Processor registration/API reference
├── tests/
│   ├── dsp_plugins_test.cpp    Registry/DSP/API/persistence tests
│   ├── dashboard_plugins_test.js  Dashboard DOM-logic tests
│   ├── reliability_test.cpp    Ring/graph/sample/adoption/save/restore checks
│   ├── device_lifecycle_test.cpp  Isolated null-backend lifecycle fixture
│   └── shutdown_test.py        Isolated plain/TLS child-process shutdown/WS checks
├── etc/                        Machine-specific ALSA/environment examples
├── Makefile                    Native server, optional tools, test targets
├── cm5audio_settings.json       Local runtime settings (ignored)
├── certs/                      Local TLS credentials (ignored)
├── build/                      Generated native objects/dependencies/tests (ignored)
└── PLANNER.md                  Current P0 scope/status plus explicitly archived roadmap
```

Other root-level probe/bridge utilities and backup files may exist during development; they are not part of the main server build unless listed in the Makefile. Keep generated binaries/credentials out of commits. Do not assume a `.bak` settings file is automatically read or maintained by the application.

## Diagnostics and troubleshooting

### No sound despite devices appearing

- Confirm the desired capture/playback directions are activated, not just enumerated.
- Check `/api/graph` for correct endpoint strings, zero-based channel indices, enabled routes, and nonzero gain.
- Check both route gain and any Gain processors/DSP master gain; they multiply.
- Check processor active/bypass and any invalid saved-chain warning. A rejected DSP update leaves the previous configuration intact.
- Ensure the signal is on an actual device channel. Tone currently produces only CH1.
- Check PulseAudio/JACK/ALSA routing and device ownership outside cm5audio.
- `/api/control` returns 501 because hardware-bank mute/gain is not wired into samples. Preserved legacy bank values do not control the signal path.
- Check adoption fields as well as route JSON. An accepted 503 pending request may apply later; it was not rolled back. Avoid enabled cycles.
- With no enabled routes, active client playback should receive silence; final process-level regression verification is still in progress.

### JACK errors followed by PulseAudio success

This is expected fallback when JACK cannot connect and PulseAudio initializes. Once PulseAudio succeeds, ALSA is not tried for that context. Missing JACK runtime service is not itself a failure of the whole application.

### Why is a 64-frame request showing 128/192-frame buffers?

The application requests 64 frames; the backend negotiates actual hardware/server buffering. Read the miniaudio conversion/period logs. Actual latency includes more than the application block. Separate physical devices can also drift because their clocks are not synchronized by the current routing engine.

### Unexpected multichannel order

Inspect device capabilities and channel maps (enable the relevant development diagnostics if needed). miniaudio or the backend may perform speaker/channel routing. For discrete multichannel I/O, test each physical channel at low amplitude before relying on channel numbering. Avoid interpreting a successful open as verified channel identity.

### Dashboard works but browser audio does not

- Build the WASM bundle and check its JS/WASM requests under `/client/dist/`.
- Verify HTTPS trust, secure context, browser microphone permission, and matching WS/WSS scheme.
- Check ports 8182 and 8183 independently and inspect browser console/network errors.
- Confirm client identity acceptance; a displayed connection alone does not create route wiring.
- Treat network media debugging separately from local hardware/plugin operation; transport hardening is still pending.

### New processor missing from dropdown

- Register the factory/descriptor in C++, rebuild the actual binary you run, and restart it.
- Check `[DSP] Registered processors: ...` and `GET /api/dsp/available`.
- Refresh the page/discovery; opening the route inspector also reloads it.
- A class declaration alone is not registration. Separate `.cpp` files must be compiled/linked.
- Discovery errors intentionally disable adding/applying rather than use a fallback hardcoded menu.

### Settings do not restore or save

Check process working directory, `CM5AUDIO_SETTINGS`, JSON validity, write permissions, and invalid-chain log warnings. Explicit `/api/settings/save` distinguishes 503 unconfirmed adoption/no write from 500 applied but save failed. Global DSP is serialized; browser-only/unapplied drafts are not. Same endpoint identities retain route bindings across reconnects; new identities consume finite slots, and backend-specific hardware IDs can still change. Temporary write/fsync/close/rename errors are not crash rollback; no automatic `.bak` restore exists.

### Compile cannot find JSON/ALSA/IXWebSocket

Install the development headers/libraries and verify include paths. Supply `NLOHMANN_JSON_INCLUDE` for the JSON tree in use. `pkg-config --modversion ixwebsocket` is useful for checking installation, but native compiler/link flags still come from the Makefile. Changing external build flags may require cleaning affected objects.

## Known limitations and roadmap

The current UI and registry are implemented; the following are remaining work, not advertised completed features:

1. **Network media protocol:** versioned frames, explicit format/sample-rate negotiation, sequence/timestamp tracking, jitter buffers, bounds/error reporting, and clock-drift compensation.
2. **Real-time hardening and validation:** move graph construction off the processing path; remove remaining meter-map, snapshot and retirement locks/allocations where justified; measure scheduling/queue behavior. Graph endpoint ownership, deferred sender retirement and single-owner shutdown are now implemented, with isolated plain/TLS process checks passed; physical hardware lifecycle/latency checks remain outstanding.
3. **Graph correctness/resource management:** reject/define cycle policy (enabled cycles remain unsupported), validate endpoint directions/actual channel counts, and improve resource reporting. Stable endpoint IDs and bounded registration fix reconnect identity loss, not unlimited/recycling capacity. Route DSP IDs remain limited to `1..1023`.
4. **Backend portability:** opaque backend-aware IDs, more JACK/device testing, and explicit device/channel mapping behavior. ALSA-only fallback/filtering is now guarded, but general hardware compatibility is a goal, not a guarantee.
5. **DSP ecosystem:** add EQ/filters/limiters and other registered C++ processors. The dynamic discovery/editor foundation exists; the effects themselves must still be implemented.
6. **UI/state completion:** global control hydration, implemented hardware sample gain/mute (currently 501 unsupported), complete metering peak/clip behavior, and wider diagnostics. Whole-route validation/adoption is implemented; a draft/applied comparison modal is deferred.
7. **Persistence recovery:** directory-fsync/power-loss durability and crash rollback/previous-good recovery are deferred. Current unique-temp, checked-file-fsync/close/rename saving is not crash recovery.
8. **Security:** remove private-key distribution and add enforceable network/application access boundaries before production exposure.
9. **Deferred optimizations:** clear only necessary node buffers and revisit disabled debug probes when useful. These do not change the low-latency block-size policy; the engine remains at **64 frames**.

There is no implemented server-to-server mesh/discovery protocol, general native client SDK, arbitrary-channel engine, or external plugin format hosting yet.

[PLANNER.md](PLANNER.md) starts with a concise current P0 implementation/verification register, then retains the long historical roadmap **as an archive, not a current feature/status list**. Use this README, [docs/PLUGINS.md](docs/PLUGINS.md), and actual code/tests as the operational reference.
