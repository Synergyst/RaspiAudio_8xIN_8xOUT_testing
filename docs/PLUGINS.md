# Built-in C++ processor discovery

The backend registry in `include/dsp_engine.h` is the source of truth for route
processor names, parameters, defaults, ranges and construction. The dashboard
contains no effect-specific menu entries or parameter defaults.

This is a **compiled-in processor registry**, not a VST/LV2 loader and not runtime
shared-library hot loading. Rebuild and restart the program after changing C++.
Reload the dashboard (or use REFRESH); opening a route inspector also refreshes
its discovery metadata.

## Available processors

- **Gain**: linear gain, default 1, range 0..8.
- **Compressor**: linear threshold, default 0.5, range 0..1; ratio, default 4,
  range 1..100; attack, default 0.01 seconds; release, default 0.1 seconds.
  Attack/release range 0.0001..10 seconds. This is a basic hard-knee compressor
  with smoothed gain reduction, not a soft-knee or dB-domain compressor.
- **Passthrough**: no parameters.

EQ and Limiter are not implemented and are not advertised. Unknown types no
longer silently become passthrough. Existing saved chains with unsupported types
or invalid values (including the old UI's compressor threshold `-20`) are
preserved but **bypassed**, logged at startup and shown with a warning in the
inspector. Remove the unavailable processor or correct the invalid values and
APPLY. To use -20 dBFS with the current compressor, enter the equivalent linear
threshold **0.1**. Settings files are never automatically rewritten by discovery.

## Add a processor entirely in C++

1. Implement an `AudioProcessor` with `process(float*, size_t frames, unsigned
   channels)`. Route processors currently receive a single channel at 48 kHz.
   Never allocate, lock, do I/O, or throw from `process()`.
2. Add a `register_processor` entry in the `ProcessorRegistry` constructor beside
   the built-ins. Supply the stable unique `type`, display name, description,
   parameter descriptors and factory. The factory receives validated, complete
   parameters: omitted values are filled from the descriptors, not the UI.
3. Rebuild with `make` and restart cm5audio. Reload the dashboard or click
   REFRESH. No HTML/JavaScript/API factory edits are needed.

For example, after implementing `MyFilterProcessor`, a registration looks like:

```cpp
register_processor({
    "my_filter", "My Filter", "A custom filter.",
    {{"frequency", "Frequency", "number", 1000.0,
      20.0, 20000.0, 1.0, "Hz", {}}},
    [](const nlohmann::json& params) {
        return std::make_unique<MyFilterProcessor>(
            params.at("frequency").get<float>());
    }
});
```

Alternatively call `ProcessorRegistry::instance().register_processor(...)` from
C++ startup code **before** loading settings or starting any worker/server
threads. Registration is startup-only; concurrent registration is not supported.
Descriptors and factories are both required: C++ cannot infer meaningful UI
ranges/units from an arbitrary DSP implementation. Registration is the one place
to describe them. Duplicate type/parameter IDs and invalid defaults are rejected.
A new separate `.cpp` implementation must also be included in the Makefile's
`APP_SRCS` (or otherwise compiled/linked).

### Parameter descriptor fields

`ProcessorParameter` fields, in aggregate-initializer order:

| Field | Meaning |
|---|---|
| `id` | Stable JSON parameter key used by the factory and settings |
| `name` | Human-readable label |
| `type` | `number`, `integer`, `boolean`, `string`, or `enum` |
| `default_value` | JSON default; must satisfy its own descriptor |
| `min`, `max` | Optional numeric bounds (`{}` means absent) |
| `step` | Optional positive numeric UI increment; integer defaults to 1 in UI |
| `unit` | Display-only units, e.g. `Hz`, `s`, `linear` |
| `choices` | For enum only: `{{"Label", jsonValue}, ...}` |

Example nonnumeric descriptors:

```cpp
{"enabled", "Enabled", "boolean", false, {}, {}, {}, "", {}},
{"mode", "Mode", "enum", "fast", {}, {}, {}, "",
    {{"Fast", "fast"}, {"Smooth", "smooth"}}},
{"label", "Label", "string", "", {}, {}, {}, "", {}}
```

Numeric values must be finite and within bounds; integer values must be whole.
`step` controls editing increments, not backend quantization. Units are not
converted automatically. Boolean/string/enum values retain their JSON types.
The API rejects unknown parameter keys instead of ignoring them. If a parameter
is renamed or removed, old saved configurations require correction; there is no
automatic schema migration.

## HTTP API

All discovery/route DSP responses are JSON.

### `GET /api/dsp/available`

Returns current compiled-in processors, sorted by type, with `Cache-Control:
no-store`. Example (abbreviated):

```json
{
  "schema_version": 1,
  "max_chain_length": 16,
  "processors": [
    {
      "type": "gain",
      "name": "Gain",
      "description": "Linear amplitude multiplier (1 = unity, 0 = silence).",
      "defaults": {"gain": 1.0},
      "parameters": [
        {"id": "gain", "name": "Gain", "type": "number", "default": 1.0,
         "min": 0.0, "max": 8.0, "step": 0.01, "unit": "linear"}
      ]
    }
  ]
}
```

### `GET /api/routes/{id}/dsp`

Returns `{ "active": false, "gain": 1.0, "chain": [] }` for an existing route
without configured DSP, or its saved/canonical configuration. An optional
`warning` describes bypassed invalid saved chains. Malformed IDs return 400;
nonexistent routes return 404. Existing routes outside DSP capacity return 400.

### `POST /api/routes/{id}/apply` — whole inspector acceptance

The dashboard now uses **one JSON request** for ordinary route gain/enabled and
the complete route DSP state. For example:

```json
{
  "gain": 0.5,
  "enabled": true,
  "dsp": {
    "active": true,
    "gain": 1.0,
    "chain": [
      {"type": "gain", "params": {"gain": 0.5}},
      {"type": "compressor", "params": {"threshold": 0.1, "ratio": 4.0}}
    ]
  }
}
```

All three outer fields are required. Ordinary route `gain` must be numeric,
finite and 0..8; `enabled` must be a JSON boolean; `dsp` must be an object using
the schema below. The handler parses/validates the **whole request**, fills
processor defaults and constructs processors before publishing route or DSP
state. Invalid DSP cannot partially accept a valid ordinary gain edit.

A 200 response contains canonical DSP fields at the **top level** (`active`,
DSP `gain`, `chain`), ordinary `route_gain` and `enabled`, `status:"ok"`, and the
completion fields below. It confirms **engine adoption**, not disk saving.
Validation rejection is 400 and leaves both route and DSP field groups unchanged;
nonexistent route is 404. Malformed/zero/overflowing numeric IDs are 400; the
route DSP capacity is still IDs **1..1023**, not an unlimited route rack.

### DSP object schema and legacy `POST /api/routes/{id}/dsp`

A DSP object replaces the **whole** route DSP configuration, not a partial
parameter patch:

```json
{
  "active": true,
  "gain": 1.0,
  "chain": [
    {"type": "gain", "params": {"gain": 0.5}},
    {"type": "compressor", "params": {"threshold": 0.1, "ratio": 4.0}}
  ]
}
```

Omitted DSP fields default to `active:false`, `gain:1`, `chain:[]`. `active`, if
supplied, must be boolean. Each processor must supply a string `type` and a
`params` object; use `params:{}` to select all defaults (also for passthrough).
Missing keys inside `params` are filled from registry defaults. DSP master gain
must be finite and 0..8. Maximum chain length is reported by discovery (currently
16). Unknown processor types/parameter keys, wrong types and out-of-range values
are rejected, not silently ignored or clamped.

The legacy DSP-only endpoint accepts this object directly and still works for
API clients; it does **not** change ordinary route gain/enabled. Its 200 response
is `{status:"ok",active,gain,chain,...completionFields}` with defaults materialized.
Validation fails with 400 and leaves the prior chain/cache untouched; missing
route returns 404. The dashboard no longer sends this request followed by a
separate `/api/routes/update` request for APPLY.

### Acceptance, adoption, and persistence are distinct

POST handlers are wrapped by a shared recursive **control mutex**. The mutex
serializes validation/publication, the engine completion wait and any save;
other accepted writers cannot interleave those steps. Successful handlers request
an engine revision and wait up to **1,500 ms** (or until the engine stops) for a
completed processing block to acknowledge it. Accepted POST responses contain:

| Field | Meaning |
|---|---|
| `accepted_revision` | Target requested for this operation |
| `adopted_revision` | Highest revision acknowledged after a completed engine block |
| `adopted` | Whether this target's adoption was confirmed |
| `persisted` | Whether this request wrote settings successfully; false for APPLY/DSP-only updates |
| `persisted_revision` | Last revision successfully saved through this WebServer; initially 0 |

**200 APPLY** means `status:"ok"`, `adopted:true`, `persisted:false`. Saving is
separate. **503 pending** means the request was accepted but adoption was not
confirmed; it **may still apply later**. This is neither cancellation nor rollback.
Refresh graph/DSP state before retrying, particularly after a lost response. A
stopping server can also reject a new request with 503 before acceptance, without
these fields. Validation/lookup/unsupported errors need not carry revision fields.

`GET /api/graph` now returns real changing `revision` (requested engine revision),
`adopted_revision`, and `persisted_revision`, plus `dsp_active` on each route for
cable highlighting. Counters are process-local, can advance for barriers or
endpoint changes without a visible route edit, and are not disk schema versions,
ETags or compare-and-swap tokens. Restoring settings does not restore historical
revision counters. Acknowledgement is an engine-block completion guarantee, not
proof that a downstream hardware/network buffer has drained.

`POST /api/settings/save` waits for adoption before writing the server's accepted
state. It never reads browser fields/drafts. A successful response has **both**
`adopted:true` and `persisted:true`. If adoption is unconfirmed it returns **503**,
`persisted:false`, and **does not call the settings serializer**. If adoption
succeeds but saving fails it returns **500**, `adopted:true`, `persisted:false`:
the applied state remains live; only persistence failed. Fix the path/storage and
retry SAVE rather than assuming live-state rollback.

### Related controls and restoration

- `POST /api/routes/update` remains the legacy ordinary gain/enabled-only update.
  Active DSP master gain **multiplies** ordinary route gain rather than replacing
  it. Bypassing DSP bypasses the chain and DSP master gain, not ordinary route gain.
- Route create/update/delete/APPLY, DSP-only, tone and fixed-global updates do not
  automatically save. Device activation/deactivation also save **after** adoption
  and surface pending-adoption or save failure; they can save other accepted state.
- Route DELETE uses the same mutex/completion wait and returns accepted/adopted
  fields and `persisted:false`, but currently omits `persisted_revision`; read it
  from the graph when needed.
- Canonical/default-filled route chain parameters are persisted and restored after
  the engine is constructed/linked. Invalid saved chains remain preserved and
  bypassed with a warning; discovery alone does not rewrite settings.
- `GET/POST /api/dsp` remains the fixed global hardware-playback Gain + Compressor,
  separate from this registry-backed route editor. It is now persisted/restored
  in optional top-level `global_dsp:{gain,threshold,ratio}`. Older files without
  that key retain defaults. It is not a dynamic global plugin rack and is not run
  on client playback.
- Hardware per-channel `POST /api/control` now explicitly returns **501 unsupported**
  because those legacy control banks do not affect samples. Previously saved banks
  are restored/preserved, not made audible. Use route gain or a Gain processor.
- Endpoint IDs remain bound to stable numeric nodes across disconnect/deactivation;
  settings loading registers saved endpoint strings even offline. Reconnecting the
  same identity refreshes its handler binding. Full strings, including embedded
  slashes, are matched. This has a finite limit of **255 non-tone endpoint identities**
  per process; identities are not recycled for unrelated endpoints. A client usually
  consumes two slots. New registration can fail at capacity.

Settings saves use a unique same-directory `PATH.tmp.XXXXXX` file, checked full
writes, file `fsync`, checked `close`, and checked rename under the control mutex.
They clean up temporary files on failure where possible and leave the destination
unchanged until rename. There is **no directory fsync**, crash/power-loss rollback,
automatic `.bak` recovery or previous-good snapshot selection. Multiple independent
processes are not coordinated by the in-process mutex. Keep one settings owner and
backups; crash rollback is deferred work.

## Dashboard behavior

Click a cable to open Route Inspector & DSP Chain. The dropdown lists only
registered processors. Add/remove/reorder processors and edit generated controls.
Existing values come from the route API; new instances use discovery defaults.

- All inspector edits are **local drafts** until **APPLY**. **CLOSE** (formerly labelled CANCEL), close, or
  clicking the outside overlay discards the draft without sending it to the server.
  CLOSE is disabled while APPLY/delete is in flight; late load responses
  cannot repopulate a closed inspector.
- **APPLY** sends the whole `{gain,enabled,dsp}` request, checks `adopted:true`, uses
  canonical returned DSP values, refreshes the graph, and then closes. Validation,
  unconfirmed adoption or refresh errors keep the inspector open with an error.
  Review authoritative state before retrying: an accepted 503 request can still
  apply, and a graph-refresh failure after adoption does not undo the change.
- Top-level **SAVE** persists only accepted, engine-adopted server state. It does
  not apply an open draft or recover an abandoned one; it also ignores unapplied
  tone/global form values. Closing after acceptance discards only remaining local
  edits, not an already accepted request. A draft/applied comparison modal is a
  future requirement, not implemented behavior.
- Dashboard mutation requests are promise-queued so SAVE cannot overtake an earlier
  APPLY from the same page; rejection does not poison subsequent requests. This
  ordering is not a cross-browser stale-revision/CAS protocol.
- Discovery failures disable adding/applying processors and show an error instead
  of substituting hardcoded placeholders. Missing saved DSP state also disables
  APPLY to avoid overwriting unknown state.

## Publication, ownership, and real-time boundaries

DSP publication uses three preallocated **state** slots per route with an audio
reader pin and serialized control-side writers. Each slot contains chain, DSP
master gain and active/bypass state, published together; even bypassed readers pin
the slot. Processor creation/destruction and metadata/JSON work occur on control
threads, not during processing. Ordinary route gain/enabled reside in the separate
immutable route snapshot. Whole-route validation plus the completion barrier does
**not** promise a single instantaneous sample-level swap of both publications
while the request is in flight.

Graph rebuilds use dirty/revision tracking so a publication during a rebuild is
not erased by that rebuild's completion. Handler pointers are backed by shared
hardware/session owners retained with the graph and retired on control threads.
Session sender joins are deferred to a reaper, never done in IX close callbacks.
One lifecycle owner coordinates HTTP/DSP/WS shutdown; project-owned HTTP socket
cancellation wakes stalled accepted reads before joining HTTP workers. The local
matching-static-IX TLS/socket-server compatibility objects and accepted-worker
wrapper address stalled handshakes, accepted-fd ownership and late workers. The vendored-miniaudio uninit path also wakes synchronous
backend data loops before joining, without a preliminary stop wait when backend
and device states disagree. See [README ownership/shutdown](../README.md#ownership-and-shutdown).

The engine is still **64 frames at 48 kHz**, eight channels wide. Enabled cycles
remain **unsupported**: Kahn sorting can omit cyclic nodes; existing feedback
buffers are not reliable cycle support. Disabled/invalid routes no longer affect
topological dependencies. Active media-ready client endpoints are included even
without enabled routes so playback can receive silence; short captures are
zero-filled. These changes do not implement automatic cyclic graph scheduling.

This is **not zero-allocation or lock-free audio processing**. Graph rebuilding
still allocates/locks on the worker. Meter-map structures now have a mutex shared
by updates, reads and reset; meter channel values are atomic but map insertion may
allocate. Endpoint snapshots/retirement also use locks/containers. PCM rings have
RAII ownership and bounded wrap-aware SPSC reads/writes without application-side
locks/allocations, but init/destruction require both users quiesced. A plugin's
`process()` must still never allocate, lock, throw or do I/O.

## Checks and verification status

```bash
make test-plugins
make test-reliability
```

`test-plugins` builds/runs registry/typed-validation/processing tests, pinned-reader
lifetime stress, HTTP discovery/rejection, persistence and unavailable-plugin
recovery, then Node.js dashboard DOM-logic tests. It uses temporary settings,
loopback **28182/28183** and `DeviceManager(false)`, with **no backend initialization**
or physical audio activation. Node.js is required for dashboard tests only.

`test-reliability` builds the native application and `tests/reliability_test.cpp`,
runs that C++ test with a 45-second timeout, runs `tests/device_lifecycle_test.cpp`
with a 20-second timeout on null/synthetic backend loops, then runs `tests/shutdown_test.py`.
The C++ suite uses temporary settings/loopback **28184/28185** for ring, deterministic
rebuild-race, sample gain/bypass/disable, slash-ID/reconnect, full APPLY/validation,
adoption, concurrent-save/failure/no-adoption and fresh-restore checks. The Python
harness uses only its own `--no-audio` processes, disposable settings/certificates
and selected **nonzero** loopback ports. Its assertions cover WS samples,
plain/TLS shutdown under traffic/stalled handshakes, reconnect and hello restrictions,
repeated/startup exits, invalid TLS, bind failure and port reuse. It needs Python 3
and the `openssl` CLI. No normal service/settings/system configuration is changed.

**Observed verification (2026-10-17):** `make test-plugins` and the complete
`make test-reliability` target passed, including null-device lifecycle and a
controlled blocking-data-loop wakeup check. Process tests recorded **21 graceful
exits**, including loaded plain/TLS SIGINT/SIGTERM, connection churn and startup;
slowest exit was approximately **1.015 s**, below the test's five-second bound.
Actual WS samples confirm route gain, DSP/bypass, disable/silence and reconnect.
An ASan/UBSan C++ reliability build also passed without a runtime report; prebuilt
miniaudio/IX objects were not instrumented. This does not prove general memory
safety. Visual browser behavior, physical listening, multichannel hardware
mapping/teardown and end-to-end latency remain unverified by those isolated tests.

**User confirmation (2026-10-17):** Ctrl+C shutdown and runtime hot-apply/hot-save
were also tested successfully in the user's normal setup. Ctrl+C replaces the
previous need for Ctrl+\\. This confirms the original reported failures in that
setup, not comprehensive device/backend qualification.
