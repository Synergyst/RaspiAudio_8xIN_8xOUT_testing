# CM5 Audio Network Patchbay Planner

**Current status:** P0 implemented and isolated regressions passed. On 2026-10-17 the user confirmed both Ctrl+C shutdown and hot-apply/hot-save work in their normal setup. Comprehensive device/backend qualification remains open.
**Scope:** Reliable acceptance/adoption/save semantics, endpoint/session ownership,
finite stable identities and shutdown; not a completed production-hardening claim.
**Operational reference:** [README.md](README.md), [docs/PLUGINS.md](docs/PLUGINS.md),
and current source/tests. The long roadmap below is explicitly archived.

## Current reliable scope and P0 implementation register

The application is now a channel-level visual patchbay with independent hardware,
tone and client endpoints; explicit fan-in/fan-out routes; a compiled-in C++ DSP
registry/discovery/editor; endpoint meters; and raw WebSocket PCM. It is **not**
the original fixed duplex input-N-to-output-N prototype. There are no implicit
routes or mandatory local DAC transit. The dashboard does not currently provide
the old per-channel history charts or raw-value visibility UI.

| Area | Implemented now / exact boundary |
|---|---|
| Inspector drafts | Gain/enabled/DSP changes stay local. CLOSE/outside click discards them; close is disabled while APPLY/delete is busy. Page mutations are promise-queued so later SAVE cannot overtake APPLY. No compare/diff modal. |
| APPLY | One JSON `POST /api/routes/{id}/apply` with required `{gain,enabled,dsp}`. Validates the whole request, fills defaults and constructs processors before publication. Invalid DSP does not partially accept route gain. 200 requires engine adoption; APPLY does not save. |
| Serialized control | POST wrappers hold the shared control mutex through mutation, completion wait, and requested persistence. Legacy route/DSP endpoints remain; DELETE also serializes/waits. Tone/global fields are validated before storing. |
| Completion | Accepted POST responses carry `accepted_revision`, `adopted_revision`, `adopted`, `persisted`, `persisted_revision`. Wait is up to **1,500 ms**, or ends early if DSP stops. 503 pending can still apply later; it is not rollback. Rejected/stopping requests need not have acceptance fields. DELETE currently omits `persisted_revision`. |
| SAVE | Top SAVE never reads open/abandoned inspector drafts or unapplied forms. It saves accepted, engine-adopted server state. No adoption: 503 and **no serializer/file write**. Adopted but save failed: 500 with `adopted:true,persisted:false`; the live change remains applied. Device activate/deactivate use adoption then autosave and report failures. |
| Revisions | Graph GET uses the real requested `revision` plus `adopted_revision` and `persisted_revision`, not hardcoded 1. Progress counters are process-local, not CAS/ETags, disk scene versions or device-render acknowledgements; restored files do not restore counters. |
| DSP publication | Chain, DSP master gain and bypass/active share a three-slot pinned publication. Ordinary route gain/enabled still use a separate immutable route snapshot; the entire APPLY is not an instantaneous sample-level swap while in flight. DSP block remains **64 frames**, 48 kHz, width 8. |
| Graph correctness | Revision/dirty handling preserves updates published during rebuild. Disabled/invalid routes do not affect ordering; active hardware and media-ready client endpoints remain scheduled for silence with no enabled routes; short capture reads are zero-filled. **Enabled cycles remain unsupported**, not automatically scheduled/repaired. |
| Identity/capacity | Endpoint numeric IDs survive disconnect/deactivation; loading registers saved endpoint strings even offline, and same-identity reconnect rebinds handlers. Full endpoint strings, including embedded slashes, are matched. At most **255 non-tone endpoint identities** per process; no unrelated-ID recycling. New registration fails at capacity. Route DSP IDs remain `1..1023`. |
| WS initialization | Application sessions are created only on **Open**, not TCP accept/pre-upgrade. First hello initializes rings once before readiness. Same identity/channel hello is idempotently accepted; changing identity/channels requires reconnect, not live resize. A new name alone does not rename a ready session. Binary before readiness is ignored. |
| Ownership/shutdown | One lifecycle owner; signal handler flags/notifies a self-pipe. HTTP acceptance stops and stalled accepted reads are cancelled before worker join; DSP stays alive until HTTP workers finish; WS shutdown handles late accepted workers. Close callbacks queue retirement; an external reaper joins senders. Graph snapshots retain hardware/session owners; last-owner release is control-side. Engine/graph owners are released before hardware/context destruction. |
| Hardware lifecycle | Negotiated buffers are prepared before device start; initialized devices uninit even after start failure; destruction/joins occur outside manager locks. Vendored miniaudio uninit wakes synchronous backend data loops after terminal state before joining, without a preliminary stop wait when states disagree. ALSA filtering/fallback is backend-guarded. Physical-backend validation remains outstanding. |
| Settings writer | Serialized unique same-directory `PATH.tmp.XXXXXX`, checked full write loop/file fsync/close/rename, cleanup on failure. No directory fsync or crash rollback/automatic `.bak` recovery. Multiple independent processes are not serialized by this mutex. |
| Persisted state | Routes/DSP, activated IDs, tone, known names, preserved legacy hardware banks, and optional fixed `global_dsp:{gain,threshold,ratio}`. Older files without global DSP retain defaults. Browser layout/identity storage and negotiated client channels remain outside server settings. |
| Hardware channel control | `POST /api/control` is **501 explicitly unsupported**, not silently stored as if audible. Existing hardware gain/mute banks load/save for compatibility but still do not process samples. Use route gain or Gain plugin. |
| Test/runtime isolation | `DeviceManager(false)` / `--no-audio` initializes **no backend**, not even miniaudio null. `CM5AUDIO_HTTP_PORT`, `CM5AUDIO_WS_PORT`, `CM5AUDIO_BIND_ADDRESS` override defaults **8182/8183/0.0.0.0**. HTTP 0 supports assigned-port reporting; use nonzero WS ports, not IX `getPort()` as an ephemeral-port guarantee. |
| IX compatibility | Local TLS/socket-server objects under `third_party/ixwebsocket/` are linked before the matching installed **static** IX library; bound server TLS handshake waits and fix accepted-fd ownership on failure/stop. No installed libraries changed. Compare source/ABI for another IX version. Project-owned `ReliableWebSocketServer` handles accepted-worker shutdown separately. |
| Real-time boundary | PCM SPSC rings are RAII/wrap-aware with bounded no-application-lock/allocation reads/writes. The **engine is not zero-allocation or lock-free**: rebuild/snapshots/retirement use locks/containers and meter-map structure is mutex-protected in updates/read/reset. Plugin process methods must still avoid allocation/locking/I/O. |

### Verification and remaining acceptance work

- `make test-plugins`: C++ registry/validation/processing, pinned-reader lifetime,
  HTTP discovery/rejection, persistence and invalid-plugin recovery, plus Node DOM
  tests. Temporary settings, loopback **28182/28183**, no backend initialization.
- `make test-reliability`: adds `tests/reliability_test.cpp` (loopback **28184/28185**,
  45-second timeout) and `tests/shutdown_test.py` (owned `--no-audio` children,
  temporary settings/certificates, selected nonzero loopback ports).
- C++ assertions cover ring wrap/RAII, deterministic rebuild race, sample
  gain/DSP/bypass/disable, slash IDs/reconnect, APPLY validation/adoption,
  concurrent saves, I/O failure, no-adoption/no-save, and fresh restore/global DSP.
  Python assertions cover plain/TLS WS samples, SIGINT/SIGTERM under traffic and
  stalled handshakes, repeated hello/reconnect, idle/startup exits, startup failures,
  port reuse and next launches. These are coverage intentions, not a claim all
  checks currently pass.
- **Observed 2026-10-17:** `make test-plugins` and full `make test-reliability`
  passed. The latter includes `tests/device_lifecycle_test.cpp` on the null
  backend and a controlled blocked-loop wakeup regression. Python recorded
  **21 graceful exits** (loaded plain/TLS signals, ten connection-churn exits,
  six idle and one early-startup exit), slowest approximately **1.015 s**;
  tested ports were reusable. Expected invalid-TLS and bind failures also exited.
  C++ checks include actual samples, 72 concurrent saves, injected short-write
  failure and fresh restoration. An application-source ASan/UBSan reliability
  build passed without runtime reports; dependency objects were not instrumented.
  These results are isolated regression evidence, not physical-backend approval.
- **Unverified:** physical capture/playback, real backend teardown, multichannel
  mapping, listening quality, overload and end-to-end latency. No service,
  installed library, system audio settings, or normal runtime settings were
  changed by this isolated P0 work/tests. Documentation maintenance itself does
  not run the server or build/test targets.

### Current follow-up priorities (not implemented P0 claims)

1. Preserve the now-passing P0 regressions and the user's confirmation of Ctrl+C
   and hot-apply/hot-save. Arrange broader device/backend and physical-path tests
   explicitly; do not infer universal compatibility from this successful setup.
2. Define/reject cycles and validate actual endpoint direction/channel capability;
   improve capacity diagnostics without claiming unbounded ID reuse.
3. Move graph construction off the processing path and measure/remove remaining
   locks/allocations; retain the **64-frame** engine policy.
4. Add framed media, sample-rate/format negotiation, jitter/drift handling and
   transport diagnostics; raw PCM WebSocket media remains current.
5. Implement hardware sample gain/mute or retain the explicit 501 boundary; improve
   global form hydration and full meter peak/clip semantics.
6. Future persistence crash rollback/previous-good recovery and parent-directory
   durability, plus a draft/applied comparison modal, are **deferred requirements**.
7. Protect private-key download/static routes and add enforceable access control
   before any untrusted-network deployment. Authentication is still absent.
8. Remote instances, scenes/import/atomic graph replacement, peer media paths,
   arbitrary-width audio and external plugin hosting remain roadmap items.

---

# Archived long-term roadmap — historical, NOT current status

> The sections numbered 1–17 below retain the earlier product/design roadmap and
> historical phase/register notes for context. **All statuses, “current” wording,
> completed-prototype claims and proposed constants in this archive are historical**;
> they are not current acceptance results or operational instructions. For example,
> a proposed 480-frame quantum, default matching-channel wiring, per-channel history
> charts, or old callback/control behavior must not override the current 64-frame,
> explicit-route implementation described above. The obsolete fixed-topology
> baseline has been replaced with a pointer rather than retained as a second
> contradictory current description. Reconcile a roadmap item with source and
> [README.md](README.md) before implementing it; do not treat archival statuses as
> a present TODO/completion checklist.

## 1. Purpose and product goal

CM5 Audio should become a real-time audio routing fabric with a visual patchbay interface. The system must allow an operator to connect any individual audio source channel to any individual audio destination channel across local hardware, browser/native clients, and other CM5 Audio server instances.

The system is not limited to one-to-one channel assignment and is not limited to routing through the server's local DAC8x8 hardware. The desired topology is an arbitrary directed audio graph:

```text
any source channel -> any destination channel
```

A source or destination may belong to:

- The CM5 server's local hardware capture device (ADC/input channels).
- The CM5 server's local hardware playback device (DAC/output channels).
- A connected browser client.
- A connected native client.
- Another CM5 Audio instance or future compatible audio node.
- Future virtual/software endpoints hosted by the routing instance.

The graph must support all of the following:

- One source channel routed to multiple destinations (fan-out).
- Multiple source channels routed to one destination (mix/sum).
- A route between two clients without touching the local DAC8x8 output.
- A route from one client directly to another client.
- A route from one server instance directly to another server instance, subject to the selected network topology.
- Any source routed to local hardware when desired.
- Per-route enable/mute, gain, and removal.
- Independent routing in each direction.
- Live graph changes without restarting the audio device or disconnecting unrelated nodes.
- Monitoring and diagnostics sufficient to identify latency, packet loss, clock drift, and overload.

The example “route all eight channels to a client's left channel” is one valid graph configuration, not a special case and not the primary model. The same system must support arbitrary channel counts and arbitrary source/destination combinations.

## 2. Historical baseline — superseded

The original fixed duplex-device, input-N-to-output-N, mono-client-tap baseline
has been removed from the current status description: it does **not** describe
the source now. There is no implicit matching-channel route or mandatory local
DAC transit. Use the current P0 register above and [README.md](README.md) for the
implemented independent endpoints, graph, controls, ownership and limitations.

## 3. Core concepts and terminology

### 3.1 Node

A **node** is a routable audio participant. Examples:

- `cm5-local-hardware`.
- `browser-client-<id>`.
- `native-client-<id>`.
- `cm5-instance-<node-id>`.
- A future virtual mixer, monitor, recorder, or software source.

A node has a stable identity for the lifetime of a connection and a human-readable name. Remote nodes should have an instance identity separate from an individual transport connection so reconnects can be represented cleanly.

### 3.2 Port and channel

Each node exposes input and output ports. A port has a direction and a channel count. At the first implementation level, a port may be the entire interleaved stream, with individual channels addressable beneath it:

```text
node / port / direction / channel index
```

Examples:

```text
cm5-local-hardware / capture / output / channel 0
cm5-local-hardware / playback / input / channel 3
browser-client-a / microphone / output / channel 0
browser-client-a / speakers / input / channel 1
```

Use zero-based indices in the protocol and display one-based channel labels in the UI.

### 3.3 Route edge

A **route edge** connects one source channel to one destination channel:

```text
source node + source port + source channel
    -> destination node + destination port + destination channel
```

Each edge has independent parameters:

- Stable route ID.
- Enabled/muted state.
- Linear gain or dB gain.
- Optional name/label.
- Creation/update metadata.
- Optional priority or ownership information.

One destination may have any number of incoming edges, subject to configured safety limits. One source may have any number of outgoing edges.

### 3.4 Bus mixing

The destination signal is the sum of all enabled incoming routes after per-route gain:

```text
destination[d][frame] = sum(source[s][frame] * route_gain[s,d])
```

The implementation must not overwrite a destination each time it processes a route. It must explicitly clear/initialize each destination bus and accumulate all active sources.

The destination bus needs an overload policy. The first version should provide configurable per-route gain and enough headroom; a later version may add a limiter or clip policy per destination.

### 3.5 Graph versus transport

The **graph** describes logical routes. A **transport** carries audio between nodes. A route may be local, or may cross a network transport. These concerns must be separated:

- The graph decides what is connected.
- The transport decides how a stream is delivered.
- The audio engine applies the graph to local and received blocks.

This separation permits direct client-to-client audio routing even when the dashboard/control plane is hosted by one CM5 instance.

## 4. Required routing behavior

### 4.1 Arbitrary local routing

The local engine must support any combination of local capture and local playback channels:

```text
local capture 1 -> local playback 8
local capture 1 -> local playback 2
local capture 2 -> local playback 2
local capture 1..8 -> local playback 1
```

The existing one-to-one behavior should become the initial default graph, preserving current behavior after migration.

### 4.2 Client-to-client routing

The graph must support:

```text
client A output channel 0 -> client B input channel 0
client A output channel 0 -> client B input channel 1
client A output channel 1 -> client C input channel 0
```

This route must not require the samples to be rendered through the CM5's local DAC output. There are two valid implementation levels:

1. **Server-mediated direct graph route:** audio packets enter the routing/fabric process and are sent directly to client B, without entering local hardware playback. This is the recommended first milestone because it centralizes graph ownership and routing policy.
2. **Transport peer path:** after graph negotiation, client A sends directly to client B. This can reduce server bandwidth but requires discovery, authentication, NAT/firewall handling, path management, and stronger distributed state. It is a later optimization, not a prerequisite for the desired logical behavior.

The architecture and terminology must distinguish “does not use the local DAC” from “does not pass through the routing process.” The first is required immediately; the second is optional and should not drive initial complexity.

### 4.3 Client-to-hardware and hardware-to-client routing

Examples that must be expressible:

```text
client A microphone 0 -> local playback 3
client A microphone 0 -> local playback 3 and 4
local capture 0..7 -> client A speakers 0
local capture 0..7 -> client A speakers 1
local capture 0..7 -> client A speakers 0 and 1
```

### 4.4 Instance-to-instance routing

Compatible CM5 Audio instances should eventually appear as nodes with their own ports. The graph must permit:

```text
CM5 instance A local capture 4 -> CM5 instance B playback 2
CM5 instance A client output -> CM5 instance B client input
```

The initial deployment may use a single routing coordinator. The protocol should nevertheless use globally identifiable nodes and ports so multiple coordinators or peer transports can be added without redesigning the route model.

### 4.5 Unrouting

Removing a route must remove only that edge. It must not mute the entire source or destination and must not affect other routes sharing either endpoint.

Required operations:

- Create one route.
- Delete one route by route ID.
- Enable/disable one route.
- Change one route's gain.
- Enumerate all routes for a node, port, channel, or complete graph.
- Replace a complete graph atomically for saved-scene loading.

## 5. Proposed architecture

### 5.1 Separate control plane and media plane

Use two logical planes:

**Control plane**

- Node/session registration.
- Capabilities and channel discovery.
- Graph mutations.
- Route gain/mute/unroute.
- Authentication/authorization.
- Graph snapshots and events.
- Diagnostics and statistics.

**Media plane**

- Framed real-time audio blocks.
- Per-stream channel data.
- Sequence/timestamp tracking.
- Jitter buffering and loss handling.
- Local audio block exchange.

The existing HTTP API and dashboard WebSocket can carry control messages. The existing WSS endpoint can be evolved for media during the prototype stage, but the media framing must be explicit. A future RTP/UDP or other real-time transport should be possible without changing the graph API.

### 5.2 Audio engine layers

Recommended native layers:

1. **Hardware endpoint layer**
   - Owns the miniaudio device.
   - Provides fixed-size local capture blocks.
   - Consumes final local playback blocks.
   - Performs no network operations.

2. **Endpoint/session layer**
   - Represents each connected client or remote instance.
   - Converts transport packets into timestamped audio blocks.
   - Converts outgoing blocks into transport packets.
   - Owns jitter buffers and connection state.

3. **Graph/routing layer**
   - Stores the control-plane graph.
   - Validates mutations.
   - Compiles routes into an audio-thread-friendly representation.
   - Mixes source blocks into destination buses.

4. **Audio block scheduler**
   - Aligns local and remote blocks to a common processing timeline.
   - Applies a defined policy when a source is late, early, disconnected, or silent.

5. **Control/API layer**
   - Provides REST/HTTP endpoints and a live event channel.
   - Does not modify audio structures unsafely.

6. **Web UI layer**
   - Renders nodes, channels, and route edges.
   - Sends mutations and displays state/diagnostics.

### 5.3 Direct client-to-client behavior

For the first practical version, all connected nodes may use the CM5 routing engine as a media relay, but the audio must go from source endpoint to destination endpoint through the graph and must bypass the local hardware playback endpoint unless an explicit route to that endpoint exists.

Example:

```text
Client A microphone 0
  -> route graph
Client B speakers 1
```

There should be no implicit path:

```text
Client A microphone 0 -> local DAC -> Client B
```

Local hardware is just another node. It receives samples only when one or more explicit edges target its playback channels.

## 6. Audio format and media protocol

### 6.1 Canonical internal format

Choose one canonical format for the initial engine:

- Sample type: `float32`.
- Sample rate: 48,000 Hz.
- Interleaved or planar representation defined explicitly; planar blocks are often easier for matrix mixing, while interleaved packets may be more convenient for browser transport.
- Fixed processing quantum, initially 10 ms / 480 frames or the hardware callback quantum if safely normalized.
- Channel count negotiated per stream, from 1 through 8 for the initial supported range.

The engine may resample or reblock at endpoint boundaries later, but the first implementation should reject unsupported sample rates rather than silently applying poor conversion.

### 6.2 Packet framing

Replace raw float payloads with a versioned frame format. The exact binary encoding can be finalized during implementation, but it must include at least:

- Protocol/version.
- Message type: audio, hello, capability, control acknowledgement, statistics, etc.
- Node/endpoint or stream ID.
- Sequence number.
- Timestamp or sample position.
- Sample rate.
- Channel count.
- Frames per packet.
- Sample format.
- Payload length.
- Audio payload.

Use a binary audio header for efficiency and a separate JSON/text control message format. Never infer channel count from the current client build or packet byte length.

### 6.3 Clock and jitter policy

Network audio cannot assume that independent client clocks run at exactly the same speed. The implementation plan must include:

- Per-stream jitter buffer.
- Target buffering latency.
- Late packet discard behavior.
- Missing packet concealment, initially silence or last-value policy.
- Sequence and timestamp statistics.
- Drift measurement.
- Long-term correction, eventually via resampling or controlled buffer adjustment.

The first milestone may use a fixed LAN environment and a modest jitter buffer, but the interfaces must expose these limitations instead of hiding them.

### 6.4 Browser constraints

A browser client may physically expose mono microphone input and stereo speaker output. This limits the browser endpoint's physical channel counts but does not limit the routing graph:

```text
server inputs 0..7 -> browser speaker left
```

is implemented as eight incoming routes mixed into the browser's output channel 0. The browser can also send any number of logical channels only if its local capture path actually provides them.

Browser client capabilities must be negotiated rather than hard-coded. The UI should show physical/logical channel counts and prevent unsupported capture routes while still allowing arbitrary fan-in and fan-out to supported destination channels.

## 7. Graph data model

### 7.1 Node record

A node record should contain fields similar to:

```text
node_id
node_type
display_name
instance_id
connection_id
state
address/identity metadata
capabilities
last_seen
```

Do not use a transient vector index as the public node identity. The current numeric session ID can remain an internal identifier but should be mapped to a stable protocol ID.

### 7.2 Endpoint/port record

```text
endpoint_id
node_id
name
direction
channel_count
sample_rate
format
transport_stream_id
availability
```

The graph should reference endpoint IDs and channel indices, not assumptions such as “all clients always have stereo output.”

### 7.3 Route record

```text
route_id
source_endpoint_id
source_channel
destination_endpoint_id
destination_channel
enabled
gain_db or linear_gain
label
created_by
created_at
updated_at
```

A route ID is necessary so a UI can remove exactly one edge even when several edges connect the same pair of nodes.

### 7.4 Graph versioning

Maintain a monotonically increasing graph revision. Every mutation should identify the revision it was based on, or use a server-side compare-and-swap mechanism. Clients should receive:

- Full graph snapshot on initial connection.
- Incremental graph events thereafter.
- A resynchronization response if their revision is stale.

Audio processing should use an immutable compiled graph snapshot. Control-plane mutations build a new snapshot and publish it atomically at an audio-block boundary.

## 8. API and control protocol plan

### 8.1 Discovery endpoints

Add endpoints conceptually equivalent to:

```text
GET /api/nodes
GET /api/endpoints
GET /api/graph
GET /api/clients
```

`/api/clients` can be retained as a convenience view, but the graph model should use general nodes/endpoints so clients and remote instances are not special cases.

### 8.2 Graph mutation endpoints

Provide operations equivalent to:

```text
POST   /api/routes
DELETE /api/routes/{route_id}
PATCH  /api/routes/{route_id}
POST   /api/graph/replace
```

A route-create body should identify source endpoint/channel and destination endpoint/channel, plus optional gain and enabled state. The server must validate:

- Both endpoints exist.
- Direction is valid.
- Channel indices are within endpoint capabilities.
- The route is permitted by policy.
- The graph will not create a forbidden cycle for a given node/stream policy.
- Resource limits are not exceeded.

### 8.3 Live graph events

Add a dashboard/control WebSocket or server-sent event stream for:

- Node connected/disconnected.
- Endpoint capability changes.
- Route created/deleted/updated.
- Graph revision changes.
- Meter/diagnostic updates as appropriate.

Polling may be used initially, but a live event mechanism is preferred for responsive graph UI and accurate client boxes.

### 8.4 Client media/control handshake

When a media client connects, exchange a control handshake before accepting audio:

```json
{
  "type": "hello",
  "protocol": 1,
  "node_id": "...",
  "endpoints": [
    {
      "id": "microphone",
      "direction": "output",
      "channels": 1,
      "sample_rate": 48000,
      "format": "f32"
    },
    {
      "id": "speakers",
      "direction": "input",
      "channels": 2,
      "sample_rate": 48000,
      "format": "f32"
    }
  ]
}
```

The server responds with the assigned identity, accepted capabilities, stream IDs, graph revision, and media parameters.

A client may be allowed to request changes to routes involving its own endpoints. Administrative UI should retain authority to inspect and change all routes. Authorization rules must be explicit before exposing this beyond a trusted LAN.

## 9. Web UI plan

### 9.1 Visual model

The dashboard should evolve from fixed capture/playback columns into a patchbay view with:

- One box/card per active node.
- Separate input and output channel lists.
- A distinct socket/handle for every channel.
- Labels showing node, endpoint, direction, channel number, and format.
- SVG or Canvas route lines.
- Pan/zoom for larger graphs.
- Search/filter for nodes and channels.
- A route inspector for gain, mute, enable, and delete.
- Connection state and media statistics.

A client card should appear when a client registers and disappear or become disconnected when it leaves. The UI must not confuse a stale session with an active route graph.

### 9.2 Interaction design

The primary operation should be:

1. Drag from a source channel socket.
2. Drop on a destination channel socket.
3. Confirm/create the edge.
4. Render the edge with its route ID and state.
5. Permit selecting the edge to change gain/mute or delete it.

Multiple edges may terminate at one channel. The UI must make this visually clear rather than replacing the previous edge.

For large fan-in examples, provide a route list/matrix inspector in addition to individual lines. Useful views include:

- Graph view.
- Selected endpoint matrix view.
- Per-node route list.
- Full graph JSON/debug view.

### 9.3 Metering

Keep the existing hardware meters, but add endpoint/channel meters where practical:

- Source level.
- Destination/bus level after mixing.
- Clip/overload status.
- Packet loss and jitter for remote sources.
- Underrun/overrun status.

Meters should be read-only diagnostics and must not be confused with route gain. Route controls belong to edges; endpoint gain, if added, must be a separate clearly named control.

### 9.4 Mobile and scale considerations

The existing dashboard is optimized for eight fixed columns. A graph with multiple nodes will need:

- Responsive layout.
- Horizontal/vertical scrolling or pan/zoom.
- Virtualized rendering if node count grows.
- Avoidance of excessive DOM/canvas redraw rates.
- Stable positions retained by node ID.

## 10. Real-time implementation strategy

### 10.1 Do not mutate the active graph in the callback

Control threads should construct and validate a new immutable compiled route plan. Publish it atomically or through a lock-free pointer swap. The audio thread reads one plan for the entire processing quantum.

A route-plan snapshot should contain precomputed operations such as:

```text
for destination bus D:
    clear D
    for operation O in D:
        accumulate source buffer S into D using gain G
```

The callback should not parse JSON, allocate vectors, enumerate sessions under a mutex, or perform network sends.

### 10.2 Preallocation

Preallocate buffers for:

- Local capture block.
- Local playback block.
- Each endpoint's input/output block.
- Destination buses.
- Jitter buffers.
- Packet serialization buffers where possible.

Set explicit limits for maximum nodes, endpoints, channels, routes, and queued audio blocks. Reject or degrade gracefully when limits are reached.

### 10.3 Queue design

Replace mutex-protected callback ring buffers with suitable single-producer/single-consumer or multi-producer structures:

- Audio callback to network thread: lock-free SPSC per stream where possible.
- Network thread to audio engine: lock-free SPSC per stream where possible.
- Control thread to audio engine: atomic route-plan publication.
- Avoid sharing one queue among unrelated clients.

Dropped/late data should be counted and exposed through diagnostics.

### 10.4 Processing order

A deterministic block should follow an order similar to:

1. Read local hardware capture.
2. Collect ready remote/client source blocks.
3. Apply endpoint input processing if configured.
4. Mix all source channels into destination buses using the compiled graph.
5. Apply destination policies such as gain, mute, headroom, and optional limiter.
6. Send local hardware playback from its destination bus.
7. Queue destination blocks for remote/client endpoints.
8. Update meters/statistics.

Local hardware is one destination endpoint among many, not the central mandatory path.

### 10.5 Audio safety

The engine must define behavior for:

- No route: destination is silence.
- Source disconnected: source becomes silence and routes remain defined or are marked inactive according to policy.
- Destination disconnected: outgoing work is discarded without blocking other destinations.
- Late packet: conceal or silence according to stream policy.
- Excessive summed level: clip flag, limiter, or configured gain policy.
- Route cycle: reject or require explicit delay/feedback support; do not accidentally create instantaneous feedback.

## 11. Network topology and mesh strategy

### 11.1 Initial topology: centralized coordinator and relay

Start with one CM5 Audio instance acting as:

- Control-plane authority.
- Graph owner.
- Media relay/fabric for connected endpoints.

This still provides a mesh-like user experience: any visible source channel can connect to any visible destination channel. It also guarantees client-to-client routes bypass local DAC channels when no hardware route is present.

### 11.2 Later topology: multiple routing instances

Add remote CM5 nodes as negotiated endpoints. Initially, remote traffic may be relayed through a designated coordinator. Later, the graph compiler can select direct node-to-node transports where available.

A true distributed mesh requires additional work:

- Node discovery and stable identity.
- Authentication and trust.
- Route ownership and conflict resolution.
- Loop detection.
- Path selection.
- Reconnection and failover.
- Distributed graph revisioning.
- Network clock synchronization.
- Bandwidth accounting.

Do not implement distributed graph consensus in the first routing milestone.

### 11.3 Bandwidth

At 48 kHz, float32, eight channels require approximately:

```text
8 * 48,000 * 4 = 1,536,000 bytes/second
```

per direction, before packet and transport overhead. Fan-out can multiply this. The system should track bandwidth per stream and avoid sending unused channels. If eight channels are routed into one client left channel, the server may mix them before transmission and send only the client's negotiated output channel count.

## 12. Security and access control

The current HTTPS/WSS setup is appropriate for encrypted transport but currently has no meaningful application authorization. Before exposing graph control to an untrusted network, add:

- Node/client authentication.
- Dashboard authentication.
- Authorization for graph changes.
- Per-client ownership rules if clients can alter their own routes.
- Input validation and hard limits.
- Protection against route-flooding and connection-flooding.
- Audit logging for graph mutations.

A trusted-LAN development mode may remain available, but it should be explicit and documented.

## 13. Persistence and scenes

The graph should be serializable to a versioned JSON document. Support later:

- Save current graph as a scene.
- Load a scene atomically.
- Restore routes when a known node reconnects.
- Keep routes involving unavailable nodes in a pending state.
- Export/import for testing and deployment.

Persist logical endpoint identity, not ephemeral socket IDs. For browser clients, restoration may require a client-provided name or token because a browser tab is not a durable device identity by default.

## 14. Testing and acceptance criteria

### 14.1 Unit tests

Test:

- Route validation.
- Channel index validation.
- Graph revision conflicts.
- Add/delete/update of individual edges.
- Multiple fan-in summing.
- Fan-out.
- Per-route gain and mute.
- No-route silence.
- Endpoint disconnect/reconnect behavior.
- Cycle rejection.
- Graph serialization/deserialization.
- Packet header parsing and malformed packet rejection.

### 14.2 Audio correctness tests

Use deterministic generated signals and verify:

- One-to-one route preserves expected samples.
- Eight sources mixed to one destination contain all eight contributions.
- Removing one route removes only that contribution.
- A source can feed multiple destinations independently.
- Client A to client B does not change local DAC output unless a separate explicit route targets the DAC.
- Client A to client B remains possible when the local hardware output is muted or unavailable.
- Route gain produces the expected amplitude.
- Destination meters reflect the mixed destination signal.

### 14.3 Integration tests

Create test endpoints that simulate 1–8 channel clients. Verify:

- Discovery and handshake.
- Multiple clients connected simultaneously.
- Arbitrary client-to-client routes.
- Client-to-hardware and hardware-to-client routes.
- Reordering/loss/delay behavior.
- Reconnect and stale route handling.
- Graph updates while audio is running.
- No unrelated route interruption when one client disconnects.

### 14.4 Performance tests

Measure:

- Audio callback duration and worst-case duration.
- Number of active routes.
- Number of nodes/clients.
- CPU cost per route and per channel.
- Memory per endpoint and jitter buffer.
- Network bandwidth under fan-out.
- Added latency for local, relayed, and future direct routes.
- Queue underruns/overruns.

The callback must remain below the available processing budget with a meaningful safety margin.

### 14.5 UI acceptance tests

Verify that an operator can:

1. See every connected client as a separate box.
2. See every endpoint and channel supported by each client.
3. Connect any source channel to any valid destination channel.
4. Create eight independent routes into one destination channel.
5. Create one source-to-many-destinations fan-out.
6. Select and delete only one route.
7. Change only one route's gain or mute state.
8. Route client A to client B without selecting or using the local DAC endpoint.
9. Observe disconnected nodes and route state clearly.
10. Reload the graph and recover from a missed event.

## 15. Historical implementation phases — archived statuses

### Phase 0 — Documentation and decisions

**Status:** PLANNED

- Confirm the graph semantics described here.
- Confirm that local hardware is an ordinary endpoint, not an obligatory transit path.
- Choose initial maximum channels per endpoint: 8.
- Choose canonical sample rate/format/block size.
- Choose initial centralized relay topology.
- Define route ownership/security expectations.
- Record decisions in this file as they are made.

### Phase 1 — Refactor local audio into endpoint and matrix abstractions

**Status:** PARTIAL — prototype implemented

- Introduce endpoint/channel identifiers.
- Represent local capture and playback as graph endpoints.
- Implement a local 8x8 route matrix.
- Preserve existing input-N-to-output-N behavior as the default graph.
- Implement fan-in, fan-out, per-route gain, mute, delete, and atomic graph updates.
- Remove hard-coded client monitor tap from the core routing path.
- Add deterministic audio tests using generated blocks.

### Phase 2 — Make the client transport channel-aware

**Status:** PARTIAL — capability handshake added; protocol work remains

- Define versioned handshake and media packet formats.
- Negotiate channel count, sample rate, format, and block size.
- Replace mono/stereo assumptions in `WebClientSession`.
- Separate client source and destination endpoints.
- Add sequence numbers, timestamps, and stream IDs.
- Add network-thread send/receive loops independent of the audio callback.
- Add basic jitter buffering and underrun/overrun statistics.
- Keep browser physical capabilities honest while supporting logical fan-in/fan-out.

### Phase 3 — Add client nodes and centralized arbitrary routing

**Status:** PARTIAL — prototype implemented

- Register each client as a node with stable connection metadata.
- Expose `/api/nodes`, `/api/endpoints`, `/api/clients`, and `/api/graph`.
- Add route create/update/delete APIs.
- Compile routes into the audio engine.
- Support hardware-to-client, client-to-hardware, and client-to-client routes.
- Verify that client-to-client traffic does not enter local DAC output unless explicitly routed there.
- Add graph revisioning and live graph events.

### Phase 4 — Build the visual patchbay UI

**Status:** PARTIAL — client cards and route controls implemented; full graph UI remains

- Replace or complement fixed meter layout with graph view.
- Render node boxes and channel sockets.
- Draw route edges and indicate selected/disabled/error states.
- Implement drag-to-connect and edge selection/removal.
- Add route gain/mute controls.
- Add node/channel search, pan/zoom, and matrix inspector.
- Integrate endpoint meters and network diagnostics.
- Retain a useful hardware meter view for the local 8x8 device.

### Phase 5 — Production real-time hardening

**Status:** PLANNED

- Remove callback allocations.
- Remove callback mutexes and session-vector copies.
- Use preallocated/lock-free queues where appropriate.
- Fix shutdown/join ownership for all server threads.
- Add overload, clipping, and route-limit policies.
- Add comprehensive packet-loss, jitter, and clock-drift handling.
- Benchmark route counts and endpoint counts on the target CM5 hardware.

### Phase 6 — Remote CM5 instances

**Status:** PLANNED

- Add remote instance node identity and capability exchange.
- Implement instance-to-instance media streams.
- Represent remote inputs/outputs in the same graph model.
- Start with coordinator-relayed paths.
- Add reconnect and pending-route behavior.
- Add authentication and per-node permissions.

### Phase 7 — Optional direct peer media paths

**Status:** NEEDREVIEW

- Evaluate whether direct client-to-client or instance-to-instance media is needed after relay performance is measured.
- Add transport negotiation and path selection.
- Preserve the same logical graph API.
- Ensure direct paths still bypass local DAC when no hardware route exists.
- Add loop, authorization, NAT/firewall, and failure handling before enabling by default.

## 16. Decisions and non-goals

### Decisions

- **The routing unit is an individual channel-to-channel edge.** A route is not limited to matching channel numbers.
- **Fan-in is required.** Multiple source channels can be summed into one destination channel.
- **Fan-out is required.** One source channel can feed multiple destination channels.
- **Local DAC is optional.** Client-to-client and instance-to-instance routes must not be forced through the local hardware output.
- **Left-channel routing is only an example.** The implementation must support arbitrary destinations and channel counts.
- **The first network topology should be a centralized graph owner/media relay.** The UI can still represent a mesh of nodes.
- **Logical graph capabilities and physical device capabilities are distinct.** A browser with stereo output can receive an arbitrary mix of server sources into left/right; it is not automatically an eight-channel physical sink.

### Non-goals for the first milestone

- Full distributed graph consensus.
- Automatic peer-to-peer path optimization.
- Unlimited channel counts.
- Transparent sample-rate conversion between arbitrary devices.
- Feedback/zero-delay cycles.
- Production exposure to untrusted networks without authentication.
- Treating WebSocket/TCP as the final low-latency transport without measurement.

## 17. Archived TODO / PLANNED / DEPRECATING / NEEDREVIEW register

Use these tags consistently in this file and in future planning notes:

- **TODO:** Concrete work item not yet assigned to an implementation phase.
- **PLANNED:** Accepted direction or work item intended for implementation.
- **DEPRECATING:** Existing behavior/API that should remain temporarily but is scheduled for replacement.
- **NEEDREVIEW:** Open design choice requiring validation, measurement, or an explicit decision.

### TODO

- TODO: Decide whether route gain is represented canonically as dB, linear gain, or both.
- TODO: Define the exact binary media header and endianness.
- TODO: Define an initial maximum route/node/endpoint limit for the CM5 target.
- TODO: Select or implement the initial lock-free queue primitive.
- TODO: Extend browser client naming and reconnect identity handling beyond the current localStorage/hello prototype.
- TODO: Extend the existing JSON settings schema for future scene/version migrations.
- TODO: Add route and graph test coverage, including client-to-client paths that bypass local DAC output.
- TODO: Replace the prototype callback allocations and mutex-protected queues with a production-safe audio block pipeline.

### PLANNED

- PLANNED: Complete the arbitrary local matrix implementation with immutable graph snapshots and audio-block-boundary updates.
- PLANNED: Complete the framed, channel-aware media protocol and jitter/clock handling.
- PLANNED: Complete the visual channel-level patchbay with draggable per-channel sockets and route lines.
- PLANNED: Add graph revisioning, live graph events, and scene loading beyond the current settings file.
- PLANNED: Add remote CM5-instance nodes and instance-to-instance media streams.

### HISTORICAL COMPLETED-IN-PROTOTYPE NOTES — not current UI claims

- COMPLETED: User-confirmed fix for hardware raw-value checkbox visibility logic; unchecked hides raw values and checked shows them.
- COMPLETED: User-confirmed compact client meter formatting and client per-channel history graphs.

- COMPLETED: Added a server-side route model with fan-in, fan-out, per-route gain, mute, and individual unroute operations.
- COMPLETED: Added local hardware capture/playback endpoints and client source/destination endpoints to the prototype graph.
- COMPLETED: Added centralized client-to-client routing that does not require local DAC output.
- COMPLETED: Added `/api/clients`, `/api/graph`, and prototype route mutation endpoints.
- COMPLETED: Added connected-client cards and route controls to the dashboard.
- COMPLETED: Prevented automatic dashboard refreshes from overwriting active selections and route-gain edits.
- COMPLETED: Added one SAVE SETTINGS workflow for hardware controls, tone settings, client names, and routes.
- COMPLETED: Added startup settings loading and automatic restoration of saved routes when matching client identities reconnect.
- COMPLETED: Added browser client identity persistence, human-readable names, and simultaneous-identity conflict handling.
- COMPLETED: Added `--plain-text` CLI mode with HTTP/WS transport and client-specific Chrome workaround guidance.

### DEPRECATING

- DEPRECATING: Fixed `hardware input N -> hardware output N` as the only local routing behavior.
- DEPRECATING: Fixed `local output 1/2 -> every client` behavior.
- DEPRECATING: Fixed `every client mono input -> local playback channel 1` behavior.
- DEPRECATING: Raw unframed float payloads on the media WebSocket.
- DEPRECATING: Mutex-protected, dynamically allocated audio callback routing.

### NEEDREVIEW

- NEEDREVIEW: Whether the media plane should remain WSS/TCP or move to RTP/UDP, QUIC, or another transport.
- NEEDREVIEW: Whether route cycles should ever be supported with explicit delay.
- NEEDREVIEW: Whether client-originated route changes are allowed beyond routes involving that client's own endpoints.
- NEEDREVIEW: Whether meters should be transported over the control channel or a separate telemetry stream.
- NEEDREVIEW: Whether direct peer media paths provide enough benefit to justify their operational complexity.
