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

### `POST /api/routes/{id}/dsp`

Accepts JSON, for example:

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

This replaces the whole route DSP configuration, not a partial parameter patch.
Omitted top-level fields default to `active:false`, `gain:1`, `chain:[]`.
Each processor must supply a string `type` and a `params` object; use `params:{}`
to select all defaults (also for passthrough). Missing keys inside `params` are
filled from registry defaults. DSP master gain must be finite and 0..8. Maximum
chain length is reported by discovery (currently 16).
Successful response is `{ "status":"ok", "active":..., "gain":...,
"chain":[...] }` with all default parameters materialized. Validation fails with
400 and leaves the previous chain/cache untouched; missing route returns 404.

### Related unchanged endpoints

- `GET /api/graph` now includes `dsp_active` on each route for cable highlighting.
- `POST /api/routes/update` still controls the ordinary route gain/enabled state.
  Active DSP gain now **multiplies** ordinary route gain rather than replacing it.
- `POST /api/settings/save` persists canonical chain parameters, route state and
  other settings; restart restores chains after the engine is constructed/linked.
- `GET/POST /api/dsp` is the existing fixed global hardware-playback DSP control,
  separate from the registry-backed per-route chain editor. This work does not
  replace that global chain with a dynamic plugin rack.

## Dashboard behavior

Click a cable to open Route Inspector & DSP Chain. The dropdown lists only
registered processors. Add/remove/reorder processors and edit generated controls.
Existing values come from the route API; new instances use discovery defaults.
All inspector edits are drafts until **APPLY**. **CANCEL** discards the draft.
Errors leave the inspector open. Use the dashboard's top-level **SAVE** to persist
the applied state to disk. Discovery failures disable adding processors and show
an error instead of substituting hardcoded placeholders.

## Chain replacement and testing

Chain publication uses three preallocated slots per route with a pinned audio
reader and serialized control-side writers. Processor creation/destruction and
metadata/JSON work occur on control threads, not during audio processing.
This fixes the old two-slot reuse hazard; it does not claim to harden every other
existing audio/graph/hardware lifecycle path. DSP chain/gain/active changes are
separate publications and can be observed across an audio block boundary; the
API serializes writers but does not make those three fields one atomic snapshot.

Run `make test-plugins` for registry/validation/processing tests, reader-lifetime
stress, HTTP discovery and persistence checks, and dashboard DOM-logic tests.
Tests use ports **28182/28183**, a temporary settings directory, and no physical
audio device activation. They do not restart a running cm5audio service.
Node.js is required for the dashboard tests only. These are focused automated
checks, not an end-to-end listening or visual browser test.
