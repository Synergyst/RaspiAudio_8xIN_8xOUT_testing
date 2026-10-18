# cm5audio

**A low-latency Linux audio patchbay with channel-level routing, a browser-based visual patchbay, and extensible C++ DSP chains.**

cm5audio routes and mixes audio between multiple local capture/playback devices, a built-in sine generator, and remote WebSocket audio clients. It is developed on a Raspberry Pi Compute Module 5, but the application is not tied to a particular audio board, manufacturer, or fixed input-to-output wiring arrangement. USB microphones, USB interfaces, onboard/I²S audio, and endpoints exposed by the selected Linux audio backend can participate, subject to their capabilities and the current engine limits.

The project combines a C++17 audio engine, miniaudio device access, an HTTP(S) control API, a WebSocket transport, and a dashboard served directly by the native application. Routing and DSP are controlled at runtime; adding a compiled-in processor does not require adding matching plugin-specific frontend code.

> **Deployment status:** actively developed and functional for local routing and DSP, with production hardening still in progress. Designed for a trusted development/LAN environment—not exposure to the public Internet. See [Security](#security-and-deployment-boundaries), especially the existing certificate/private-key download routes.

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
- Add, remove, and reorder processors; edit route/DSP settings as drafts before APPLY.
- Display endpoint RMS/peak metering through the API and live patchbay meters.
- Generate a configurable sine tone for routing tests.
- Save routes, route DSP, activated device IDs, tone settings, hardware control state, and known client names.
- Serve HTTP/HTTPS and WS/WSS, including an optional Emscripten/WASM browser audio terminal.
- Maintain browser client names and IDs in browser storage and reject simultaneous identity claims.

### Important distinctions

- **Device-agnostic does not mean unlimited compatibility.** The current routing width is eight channels; not every backend/device combination has been validated. See [Backend selection](#backend-selection-and-device-compatibility).
- **Capture is a source; playback is a destination**, from the patchbay's perspective. A USB microphone need not have a playback endpoint, and an output-only DAC need not have capture.
- There is **no automatic input-N-to-output-N connection**. Create the routes you want.
- The plugin system is a **compiled-in C++ registry**, not a VST/LV2 host, plugin-directory scanner, or runtime shared-library loader.
- Client media remains raw PCM over WebSockets. Versioned frames, sequence tracking, clock-drift handling, and network jitter control are future work.
- Some API state exists without corresponding signal-path functionality. In particular, the hardware control API stores gain/mute state, but the current `DspEngine` does **not** apply those `AudioControls` banks to samples. Use route gain or a Gain processor for operational level control.

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
| Node handler/buffer slots | 256; valid buffer indices `0..255`, with tone at node 0 |
| Route DSP slots | 1,024 array entries; supported route DSP IDs are `1..1023` |
| Processors per route | Maximum 16; discovery API advertises this limit |
| Media staging size | `CM5_MAX_AUDIO_FRAMES = 4320`; **not** the engine processing block |

These are implementation values, not latency guarantees. The 64-frame block is the low-latency target, not a statement that microphone-to-speaker latency is 1.333 ms. Backend buffering, sample/format conversion, scheduling, separate device clocks, and network transport add latency. For example, PulseAudio can accept a 64-frame request while reporting a larger actual buffer.

Hardware sample formats may differ from float32; miniaudio converts as needed. The application requests a 48 kHz client-facing device stream. The sample rate reported by device enumeration is capability metadata, not proof of the currently opened hardware format.

The current build is not an arbitrary-width engine. Devices with more than eight channels are not fully supported by the routing graph; do not assume all their channels become routable. Node and route IDs grow during operation, so fixed array sizes are not equivalent to a fully managed/recycled resource pool.

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

The current device layer retains Linux/ALSA-specific assumptions: ID serialization uses the `ma_device_id.alsa` union member, several virtual ALSA names are filtered, some device-name heuristics force eight channels, and activation has an ALSA `plughw` fallback. PulseAudio operation has been exercised, but adding JACK to the candidate list is **not** comprehensive JACK integration or validation. Backend-independent opaque IDs and backend-specific fallback rules remain hardening work.

PipeWire is not a separate candidate in this list. It may provide a PulseAudio- or JACK-compatible server, in which case the selected backend name remains PulseAudio or JACK. Device visibility can differ from `aplay -l`/`arecord -l`, which inspect ALSA rather than the chosen sound server.

There is currently no backend-selection command-line flag. Change the constructor's list/order if a different preference is required; changing a running sound server or `/etc` configuration is not necessary for ordinary plugin development.

## Dependencies and setup

### Native application

- Linux with an audio backend usable by miniaudio.
- C++17 compiler (`g++`), GNU Make, and pthreads.
- ALSA development headers/library (`libasound2-dev` on Debian/Ubuntu).
- OpenSSL development headers/libraries (`libssl-dev`).
- zlib development headers/library (`zlib1g-dev`).
- TLS-enabled IXWebSocket headers/library, normally installed under `/usr/local`.
- nlohmann/json headers. The Makefile defaults to **`../json/include`** and accepts `NLOHMANN_JSON_INCLUDE` overrides.

miniaudio and cpp-httplib are vendored in `include/`. nlohmann/json and IXWebSocket are not supplied there. Emscripten is needed only for the optional browser audio bundle. Node.js is needed only for dashboard tests.

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

`make clean` removes application objects/dependency files, the native server, and plugin test artifacts. It intentionally retains `build/miniaudio_impl.o` to avoid repeatedly compiling the large vendored implementation.

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

Tests use temporary settings and ports **28182/28183**, construct a backend context, and do not enumerate/activate physical audio devices or restart the normal server. The test WebServer still binds its listeners on all interfaces, so run on a trusted host with those test ports free. Tests are not a complete visual browser, network media, hardware lifecycle, or end-to-end latency test.

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

Stop with Ctrl+C or SIGTERM. The application handles these signals to stop its DSP worker and web servers. Do not assume stopping the process automatically saves unsaved changes.

### Listener URLs

Replace `HOST` with your server's reachable address:

| Service | Plain text | TLS |
|---|---|---|
| Main dashboard | `http://HOST:8182/` | `https://HOST:8182/` |
| Browser audio terminal | `http://HOST:8182/client/index.html` | `https://HOST:8182/client/index.html` |
| Auxiliary device scan | `http://HOST:8182/client/scan.html` | `https://HOST:8182/client/scan.html` |
| Audio WebSocket | `ws://HOST:8183/ws/audio` | `wss://HOST:8183/ws/audio` |

HTTP and WebSocket listeners bind `0.0.0.0`. Ports **8182/8183** are supplied by `main.cpp`; there are no current port CLI/environment overrides. `--plain-text` is the implemented runtime switch. The `Operational:` console URL currently contains a development-machine IP; use your actual address rather than treating that string as address discovery.

Without `--plain-text`, TLS is selected when both certificate and key files are readable; otherwise the application falls back to plain HTTP/WS. Invalid readable TLS credentials can fail startup rather than produce a usable TLS listener. Check listener logs and actual sockets, not just the optimistic operational banner.

Only one instance should own the normal ports:

```bash
ss -ltnp | grep -E ':8182|:8183'
```

### Runtime environment

| Variable | Default | Used for |
|---|---|---|
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
6. Adjust route gain/enabled, active/bypass, DSP master gain, and ordered processors. Click **APPLY** to update the running configuration, or **CANCEL** to discard the inspector draft.
7. Use top-level **SAVE** to persist applied state to disk.

Multiple inputs routed to the same output are **summed**, not automatically normalized. Start with low levels: route gain, processor Gain, and DSP master gain can multiply together. A zero gain silences a route; 1 is unity.

DSP-active cables receive an effect highlight; disabled routes use a different color. Endpoint meters are polled approximately every 100 ms by the page. This is visual monitoring, not the engine audio processing interval.

Node positions are stored in that browser's `localStorage`, separately from server settings. A different browser or cleared site storage will not inherit the same layout. REFRESH updates hardware/graph/discovery; node positioning does not require a server-side layout API.

Inspector APPLY uses separate DSP and route-update requests. If one succeeds and the other fails, the modal reports partial completion and stays open; it is not a cross-endpoint transaction.

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

A separate fixed global Gain + Compressor stage runs on **hardware playback** after mixing; it does not run on client playback. Defaults are gain 1, threshold 0.7 linear, ratio 4. Its compressor is stateful and shared between hardware playback calls, so it is not an independently instantiated dynamics processor per hardware channel/device. Its settings are not part of the route registry or persisted global settings.

### Add an effect without editing the dashboard

1. Implement an `AudioProcessor` class in C++.
2. Register its stable type, display name, description, parameter descriptors, and factory in `ProcessorRegistry` in `include/dsp_engine.h`, or during startup before settings/worker threads.
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
| `ProcessorRegistry` | Processor descriptors, discovery, default filling, validation, construction |
| `WebServer` | HTTP control/static files, WebSocket sessions, canonical route DSP cache |

Startup enumerates hardware, constructs/links the engine, restores settings and processor chains, then starts DSP and web service threads. DSP must exist before settings restoration to apply saved chains.

### Graph and processing

- Routes reference numeric `NodeId` values internally; HTTP/settings use endpoint strings.
- A dirty flag triggers graph rebuilding with **Kahn's topological sorting algorithm**.
- A fixed-size handler array resolves node handlers during processing.
- Node and route scratch buffers are preallocated; per-source adjacency lists avoid scanning all routes for every node.
- Audio is processed in 64-frame blocks. For fixed block size, routing work scales with active connections; sample work is proportional to routes × frames plus processor cost.
- A node without a bound physical/client handler can become an internal virtual handler. There is no public virtual-bus creation/list/editor API yet.
- Route DSP uses **three preallocated chain slots** with audio-reader pinning and serialized control-thread writers. Processors are constructed/destroyed on control threads, not while executing the chain.
- Chain, gain, and active state are separate publications; they are not one atomic configuration snapshot.

This is a DAG-oriented engine, not a guaranteed arbitrary feedback graph. Feedback pools and delay-selection code exist, but cycles are not removed from Kahn's dependency graph before sorting; cyclic nodes can be omitted from execution. **Do not rely on feedback loops as a supported feature.** Ordering among independent nodes is not guaranteed stable because rebuild uses unordered containers.

Preallocated buffers and safe chain publication are improvements, not a blanket claim of hard real-time safety. Graph rebuilding still happens from `process_audio()` and allocates/locks; metering uses shared maps; further ownership, scheduling, and lock-free transport work remains.

## HTTP API reference

**Base URL:** `http://HOST:8182` or `https://HOST:8182` according to runtime mode. All API responses are JSON; web/download responses use their appropriate content types.

Unless specifically marked **JSON body**, POST handlers read URL query or form parameters through cpp-httplib. Sending a JSON body to `/api/tone` or `/api/routes` is not equivalent. Route DSP POST is the JSON-body exception.

Gains are linear amplitude multipliers, not decibels. Channels are zero-based. Use the exact IDs returned by enumeration/graph; names are display labels, not stable identifiers. APIs do not provide application authentication.

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
| POST | `/api/devices/activate` | Required `id`; tries matching capture/playback entries, registers nodes, attempts settings save; success `{status:"ok"}`, failure 400 |
| POST | `/api/devices/deactivate` | Required `id`; deactivates matching device entries, unregisters nodes, attempts settings save; success `{status:"ok"}` |
| GET | `/api/clients` | `{clients:[...]}` with session `id`, `client_id`, `name`, `ip`, `input_channels`, `output_channels`, `input_meters`, `output_meters` |
| GET | `/api/graph` | `{revision:1,endpoints:[...],routes:[...]}`; endpoint IDs/names/directions/channels, connected flags for known clients, route state including `dsp_active` |

`revision` is currently fixed at 1; it is not a changing graph version/ETag. Tone has no explicit `name` field in the graph; clients should tolerate optional endpoint fields. Disconnected known clients are advertised with placeholder counts (1 input/2 output), not persisted negotiated channel counts. Enumeration channel count 0 means unknown/native selection; it does not mean the device has no channels.

Repeated activation is not fully idempotent: if no new stream is opened, activation can return failure even if a device is already active. Activation/deactivation attempt persistence but do not report save failure through those success responses; use `/api/settings/save` when explicit save error handling matters.

### Routes

| Method | Endpoint | Parameters / behavior |
|---|---|---|
| POST | `/api/routes` | `source_node`, `destination_node` **endpoint strings**; `source_channel`, `destination_channel` 0..7; optional `gain` (default 1, finite 0..8), `enabled` (default true) |
| POST | `/api/routes/update` | Required `id`; `gain` default 1, finite 0..8; `enabled` default true. Missing values reset to defaults, not preserve old values. Failure 400 |
| DELETE | `/api/routes/{id}` | Delete route and cached DSP configuration; 404 if not found |

Route creation returns `{"status":"ok","id":"X"}`: the creation response ID is currently a **string**, while graph/settings route IDs are numbers. Duplicate source-channel/destination-channel connections are rejected with 400. Graph routes use keys `source` and `destination` for endpoint strings, not the POST parameter names `source_node`/`destination_node`.

The API checks channels against the internal width, not every endpoint's actual channel count/direction, and does not comprehensively validate graph cycles or capacity. Prefer known source/destination endpoints from `/api/graph` and avoid cycles. There is no separate `GET /api/routes`; obtain routes through `/api/graph`.

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

A valid update returns `{status:"ok",active,gain,chain}` with canonical/default-filled parameter values. Malformed/zero/out-of-range numeric IDs return 400; nonexistent routes return 404; existing routes outside the supported DSP ID range return 400. Supported DSP IDs are currently `1..1023`.

See [docs/PLUGINS.md](docs/PLUGINS.md) for the full metadata schema and typed controls.

### Tone and fixed global DSP

| Method | Endpoint | Parameters / returned data |
|---|---|---|
| GET | `/api/tone` | `{enabled,frequency_hz,amplitude}` |
| POST | `/api/tone` | Optional `enabled`, `frequency` 1..20000 Hz, `amplitude` 0..1; invalid numeric values 400 |
| GET | `/api/dsp` | Fixed global hardware-playback `{gain,threshold,ratio}` |
| POST | `/api/dsp` | Optional finite `gain >= 0`, `threshold >= 0`, `ratio >= 1`; invalid values 400 |

Tone POST uses **`frequency`**, while GET/settings use **`frequency_hz`**. Although the tone graph endpoint advertises eight channels, the current sine generator writes only **channel 0 (CH1)**. Route that channel to any desired outputs.

Global DSP is separate from per-route plugin chains and is not saved by the current settings serializer. Tone/global handlers apply fields sequentially; a later invalid field may return an error after an earlier field was already changed. Do not assume all older control endpoints provide transactional updates.

### Metering, stored hardware controls, and settings

| Method | Endpoint | Parameters / returned data |
|---|---|---|
| GET | `/api/meters` | `{capture:{endpoint:[meters...]},playback:{endpoint:[meters...]}}` |
| GET | `/api/metrics` | Alias of `/api/meters` |
| GET | `/api/raw` | Same capture/playback map shape, each channel `{ch,value,peak}` |
| POST | `/api/control` | Required `endpoint`, `type=capture` or `playback`, `ch=0..7`; optional finite `gain=0..8`, `mute=0` or `1`; stores hardware control state (not currently applied in DSP) |
| POST | `/api/reset_clips` | `all=1`, or required `endpoint`, `type`, `ch`; resets `clipped=false`, `peak_hold_db=-60` |
| POST | `/api/settings/save` | No required body; saves current serializable state; `{status:"ok"}` or 500 on reported save failure |

Meter entries contain `ch`, `raw_value`, `raw_peak`, `rms_db`, `peak_db`, `peak_hold_db`, `clipped`. `raw_value`/`value` is **block RMS**, and `raw_peak`/`peak` is **block absolute peak**, not a stream of individual raw samples. `/api/meters` and `/api/raw` iterate existing metric maps, not a strict active-device snapshot, and emit eight entries per stored endpoint bank. Session meter arrays are also returned by `/api/clients`, but the engine currently updates endpoint metric maps rather than those session arrays; use endpoint maps for engine metering.

Peak-hold and clip fields are part of the API, but continuous peak-hold accumulation and latched clipping are not implemented in the current meter update. `clipped` reflects the most recently updated block's peak `> 1.0`. Silent/absent capture blocks may leave older meter values until another update. RESET CLIPS is not a substitute for removing clipping from the signal path.

Boolean parsing differs across existing endpoints: general tone/route helpers treat `1`, `true`, and `on` as true; `/api/control` mute recognizes only `1`; `/api/reset_clips` resets all banks whenever the **`all` parameter is present**, even `all=0`. Use the explicit forms above.

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
curl -fsS -X POST "$BASE/api/routes/1/dsp" \
  -H 'Content-Type: application/json' \
  --data '{"active":true,"gain":1,"chain":[{"type":"gain","params":{"gain":0.5}},{"type":"compressor","params":{"threshold":0.1,"ratio":4,"attack":0.01,"release":0.1}}]}'

curl -fsS "$BASE/api/routes/1/dsp"
curl -fsS -X POST "$BASE/api/routes/update?id=1&gain=0.1&enabled=1"
curl -fsS -X POST "$BASE/api/tone?enabled=0"
curl -fsS -X POST "$BASE/api/settings/save"
```

## WebSocket protocol

**Port:** 8183. The browser terminal uses `/ws/audio`; the current server callback does not enforce a specific request path. Use the conventional URL rather than assuming a REST endpoint on that port.

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

- `assigned`: sent on connection; includes `connection_id`, provisional `client_id`, and `name`.
- `identity_accepted`: includes accepted `client_id` and `name`.
- `identity_conflict`: includes the already claimed `client_id`.

The bundled page stores identities in `localStorage`. A duplicate active claim prompts the user before creating a new ID. This is naming/session coordination, **not authentication or secure identity ownership**.

### Binary media

Payloads are raw interleaved float32 PCM. There is no application header/version, sequence number, timestamp, stream ID, explicit frame count, or byte-order marker. Current deployment assumes the engine's 48 kHz format and compatible float representation/byte order.

The bundled terminal sends a mono microphone stream and receives stereo playback. Its WASM network tick uses 480-frame/10 ms capture packets; this is separate from the server's 64-frame processing block. The page sends `sample_rate:48000` and `format:"f32"` in its hello, but the server currently **does not negotiate or validate those fields**, nor echo final channel/rate parameters in `identity_accepted`.

Buffers are initialized after an accepted hello with the selected channel counts. Transport remains a development implementation: repeated hellos/format changes and thread-safe lifecycle, jitter, underrun/overrun reporting, and drift compensation require further work. Do not equate WebSocket reliability with bounded real-time latency. Media framing is a planned protocol upgrade, not present functionality.

## Persistence and restoration

Default file: **`./cm5audio_settings.json`**, overridden by `CM5AUDIO_SETTINGS`.

Stored top-level data:

| Key | Contents |
|---|---|
| `version` | Current settings marker, 1; not a full migration framework |
| `activated_devices` | Device IDs to attempt to activate at startup |
| `tone` | Enabled, `frequency_hz`, amplitude |
| `hardware.capture`, `hardware.playback` | Endpoint/channel stored gain/mute banks |
| `routes` | Route IDs, endpoint strings, channels, gain, enabled, optional DSP state/chain |
| `clients` | Known client IDs mapped to display names |

Save writes a `.tmp` file and renames it over the destination. This reduces replacement exposure but is not a crash-durable database or a guarantee against concurrent saves/I/O failure. Back up important configurations yourself.

- Top-level **SAVE** / `POST /api/settings/save` persists current serializable state.
- Device activation/deactivation also attempts a save of the current state, potentially including other changes already applied in memory.
- Route/plugin edits and tone/global controls do not all automatically save.
- Global DSP settings, browser canvas layout, browser identity storage, and negotiated client channel counts are **not** part of server settings persistence.
- Invalid/unavailable processor chains are preserved and bypassed with a warning. Correct/remove the offending processor and APPLY; recreating the route is also possible, but discards the old chain.
- There is no settings import/reload HTTP endpoint. Change an offline file carefully and restart the application to load it.

**Endpoint restoration caveat:** routes resolve strings to node IDs while loading. Hardware activation is attempted first, but disconnected client/other unregistered endpoints may resolve to `NODE_INVALID`; persistent reconnection/remapping is not complete. Seeing an offline known client in the graph does not prove an old route will automatically resume. Verify routes after reconnects, hardware deactivation/reactivation, or switching backend.

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

**Current application authentication/authorization: none.** Anyone who can reach the listeners can potentially inspect/control routing, change settings, connect a media client, or claim an unused client identity. Both normal listeners bind all IPv4 interfaces.

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
│   ├── constants.h              Block size, channel width, staging/processor limits
│   ├── device_manager.h        Hardware/context ownership declarations
│   ├── dsp_engine.h            AudioProcessor classes, registry/schema, engine state
│   ├── web_server.h            Routes, sessions, meters/controls, server declarations
│   ├── ring_buffer.h           miniaudio PCM ring-buffer wrapper
│   ├── miniaudio.h             Vendored audio implementation/API
│   └── httplib.h               Vendored HTTP implementation/API
├── src/
│   ├── main.cpp                Startup/link/restore order, signals, server ports
│   ├── device_manager.cpp      Backend priority, enumeration, activation, callbacks
│   ├── dsp_engine.cpp          Graph rebuilding, processing, DSP publication, meters
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
├── docs/PLUGINS.md              Processor registration/API reference
├── tests/
│   ├── dsp_plugins_test.cpp    Registry/DSP/API/persistence tests
│   └── dashboard_plugins_test.js  Dashboard DOM-logic tests
├── etc/                        Machine-specific ALSA/environment examples
├── Makefile                    Native server, optional tools, test targets
├── cm5audio_settings.json       Local runtime settings (ignored)
├── certs/                      Local TLS credentials (ignored)
├── build/                      Generated native objects/dependencies/tests (ignored)
└── PLANNER.md                  Detailed planning register; contains older baselines
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
- Stored `/api/control` mute/gain is not currently wired into sample processing; it cannot diagnose route-level silence reliably.

### JACK errors followed by PulseAudio success

This is expected fallback when JACK cannot connect and PulseAudio initializes. Once PulseAudio succeeds, ALSA is not tried for that context. Missing JACK runtime service is not itself a failure of the whole application.

### Why is a 64-frame request showing 128/192-frame buffers?

The application requests 64 frames; the backend negotiates actual hardware/server buffering. Read the miniaudio conversion/period logs. Actual latency includes more than the application block. Separate physical devices can also drift because their clocks are not synchronized by the current routing engine.

### Unexpected multichannel order

Inspect the logged input/output channel maps. miniaudio or the backend may perform speaker/channel routing. For discrete multichannel I/O, test each physical channel at low amplitude before relying on channel numbering. Avoid interpreting a successful open as verified channel identity.

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

Check process working directory, `CM5AUDIO_SETTINGS`, JSON validity, write permissions, and invalid-chain log warnings. Use explicit `/api/settings/save` to surface a reported save error. Global DSP and browser-only state are not serialized. Recheck route bindings after endpoint reconnects; ID remapping remains incomplete.

### Compile cannot find JSON/ALSA/IXWebSocket

Install the development headers/libraries and verify include paths. Supply `NLOHMANN_JSON_INCLUDE` for the JSON tree in use. `pkg-config --modversion ixwebsocket` is useful for checking installation, but native compiler/link flags still come from the Makefile. Changing external build flags may require cleaning affected objects.

## Known limitations and roadmap

The current UI and registry are implemented; the following are remaining work, not advertised completed features:

1. **Network media protocol:** versioned frames, explicit format/sample-rate negotiation, sequence/timestamp tracking, jitter buffers, bounds/error reporting, and clock-drift compensation.
2. **Real-time and lifecycle hardening:** move graph construction off the processing path, eliminate remaining shared-map/session snapshot locks/allocations, strengthen hardware/client ownership, and complete a well-defined SPSC transport design. In particular, cached raw handler pointers and concurrent endpoint teardown need further review.
3. **Graph correctness/resource management:** cycle policy, usable feedback scheduling, stable ordering if required, endpoint direction/channel validation, node-ID bounds/reuse, route DSP slot lifecycle/capacity, and reliable disconnected-endpoint restoration.
4. **Backend portability:** opaque backend-aware IDs, backend-specific enumeration/fallback, more JACK/device testing, and explicit device/channel mapping behavior. General hardware support is the goal, not a guarantee for all Linux devices.
5. **DSP ecosystem:** add EQ/filters/limiters and other registered C++ processors. The dynamic discovery/editor foundation exists; the effects themselves must still be implemented.
6. **UI/state completion:** global control hydration, applied hardware gain/mute, complete metering peak/clip behavior, wider diagnostics, and transactional whole-route editing.
7. **Security:** remove private-key distribution and add enforceable network/application access boundaries before production exposure.
8. **Deferred optimizations:** clear only necessary node buffers and revisit disabled debug probes when useful. These are not the current low-latency block-size policy; the engine remains at 64 frames.

There is no implemented server-to-server mesh/discovery protocol, general native client SDK, arbitrary-channel engine, or external plugin format hosting yet.

[PLANNER.md](PLANNER.md) retains the detailed roadmap and implementation register, but several of its baseline/status passages predate the current graph/UI/registry. Treat this README, [docs/PLUGINS.md](docs/PLUGINS.md), and actual code/tests as the current operational reference; do not infer that every planned item is already implemented.
