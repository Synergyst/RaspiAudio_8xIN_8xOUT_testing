# CM5 Audio 8x8 Engine

A Raspberry Pi CM5 audio engine for 8-channel duplex capture/playback with a browser dashboard and a browser microphone/audio bridge.

## Features

- 8-channel capture and playback at 48 kHz using [miniaudio](https://miniaud.io/)
- Per-channel capture/playback gain and mute controls
- RMS and peak meters with clip latching and peak hold
- HTTP(S) dashboard and static web client
- WebSocket (WS/WSS) PCM bridge for browser clients
- Emscripten/WASM browser client
- Persistent server settings and route restoration
- Stable browser client identities with human-readable names
- Single **SAVE SETTINGS** workflow for all settings and routes

## Runtime endpoints

When started from the repository root with the supplied TLS certificate:

- Dashboard: `https://192.168.168.172:8182/`
- Browser audio client: `https://192.168.168.172:8182/client/index.html`
- Audio WebSocket: `wss://192.168.168.172:8183/ws/audio`
- Meter API: `GET /api/meters` (includes RMS/peak data and raw numeric fields)
- Raw port API: `GET /api/raw`
- Tone API: `GET /api/tone` and `POST /api/tone?enabled=0|1&frequency=440&amplitude=0.2`
- Control API: `POST /api/control?type=capture|playback&ch=0..7&gain=1.0&mute=0|1`
- Clip reset API: `POST /api/reset_clips?type=capture|playback&ch=0..7` or `POST /api/reset_clips?all=1`
- Settings save API: `POST /api/settings/save`

Settings are stored in `./cm5audio_settings.json` by default. Override the path with `CM5AUDIO_SETTINGS`. The file stores hardware controls, tone settings, client names/identities, and the complete route list. The server loads it at startup and keeps saved client routes available for reconnection. The dashboard uses one **SAVE SETTINGS** button; settings and routes otherwise apply live in memory.

The server uses `CM5AUDIO_TLS_CERT` and `CM5AUDIO_TLS_KEY` when set. Otherwise it looks for `./certs/server.crt` and `./certs/server.key`. If neither file is available, it falls back to plain HTTP/WS; microphone access from a LAN IP will then be blocked by Chromium's secure-context policy.

## Prerequisites

On Debian/Ubuntu-based systems, the setup helper installs or verifies:

- C++17 compiler and build tools
- OpenSSL development headers/libraries
- zlib development headers/libraries
- `pkg-config`
- Emscripten (`emcc`) for the browser client
- `ixwebsocket` development headers/library

`miniaudio` and `cpp-httplib` are vendored in `include/`. The current Makefile expects ixwebsocket under `/usr/local/include` and `/usr/local/lib`; it also links OpenSSL and zlib.

Run the setup helper from the repository root:

```bash
./scripts/setup.sh
```

If ixwebsocket is not installed, the helper prints the required `pkg-config` check and stops. To build and install the pinned TLS-enabled ixwebsocket release automatically, run:

```bash
./scripts/setup.sh --install-ixwebsocket
```

The installer is also available directly as `./scripts/install_ixwebsocket.sh`; it builds IXWebSocket `v12.0.1` with OpenSSL TLS support under `/usr/local`.

The helper also creates a self-signed certificate with an IP subject-alt-name for the CM5's LAN address. Set `CM5AUDIO_HOST` if the address is not `192.168.168.172`:

```bash
CM5AUDIO_HOST=192.168.168.172 ./scripts/setup.sh
```

The certificate and private key are generated locally and are ignored by Git. `setup.sh` validates an existing certificate's host name/IP SAN and regenerates it if `CM5AUDIO_HOST` changes. The certificate must be trusted separately on each browser client; installing it on the CM5 alone is not enough.

## Build

Build the native engine:

```bash
make clean
make
```

Build the browser WASM bundle:

```bash
./web_client/build_wasm.sh
```

This generates ignored deployment artifacts in `web_client/dist/`:

- `client_audio.js`
- `client_audio.wasm`

The WASM build script resolves the repository root automatically, so it can be run from any working directory.

## Run

Run the engine from the repository root because the web assets and default TLS paths are relative to the working directory:

```bash
./cm5audio
```

For custom certificate locations:

```bash
CM5AUDIO_TLS_CERT=/absolute/path/server.crt \
CM5AUDIO_TLS_KEY=/absolute/path/server.key \
./cm5audio
```

The process must be started from the repository root unless the web assets and default certificate paths are changed. Only one instance should use ports `8182` and `8183`; check existing instances with `ss -ltnp | grep -E ':8182|:8183'`.

The native process requires an available 8-channel, 48 kHz duplex audio device. On a headless CM5, verify the selected ALSA device and channel capabilities if initialization fails.

## Browser microphone access

`getUserMedia()` requires a secure browser context. Use the HTTPS client URL, not the HTTP URL:

```text
https://192.168.168.172:8182/client/index.html
```

Because the setup helper creates a self-signed certificate, each browser client must trust the exact certificate. The simplest Ubuntu procedure is:

1. Copy the certificate from the CM5 to the Ubuntu client:

   ```bash
   scp root@192.168.168.172:/root/Sources/cm5audio/certs/server.crt /tmp/cm5audio.crt
   ```

2. From the normal desktop user account—the same account that runs Brave/Chromium—run the helper. Do **not** run it after `sudo su -`. If the project is not checked out on the client, copy the helper too:

   ```bash
   scp root@192.168.168.172:/root/Sources/cm5audio/scripts/trust_server_cert.sh /tmp/trust_server_cert.sh
   chmod +x /tmp/trust_server_cert.sh
   /tmp/trust_server_cert.sh /tmp/cm5audio.crt
   ```

   The helper installs the certificate into both Ubuntu's system CA store and the current user's NSS database. It uses `P,,`, which trusts this exact peer certificate. If `certutil` is missing, install it and rerun:

   ```bash
   sudo apt install libnss3-tools
   ```

3. Fully exit Brave/Chromium and start it again. `killall brave` may be needed if background browser processes remain.

4. Open the HTTPS client URL:

   ```text
   https://192.168.168.172:8182/client/index.html
   ```

5. Allow microphone access and confirm the WebSocket field is:

   ```text
   wss://192.168.168.172:8183/ws/audio
   ```

You can verify the Ubuntu system trust before opening the browser:

```bash
curl -fsS https://192.168.168.172:8182/ -o /dev/null \
  -w 'HTTPS trust works: HTTP %{http_code}\n'
```

Expected output is `HTTPS trust works: HTTP 200`. Do not open `https://192.168.168.172:8183/` as a webpage: port `8183` is a WSS endpoint and is not an HTTP server, so browser errors such as `ERR_RESPONSE_HEADERS_TRUNCATED` are expected.

There is no separate browser permission for WebSockets. The page must be HTTPS and the audio endpoint must use WSS. For a permanent deployment, replace the self-signed certificate with one trusted by the client browser and containing the correct DNS name or IP SAN.

## Development-only Chromium flag

If testing a deliberately plain HTTP/WS deployment, Chromium can treat the origin as secure:

```text
chrome://flags/#unsafely-treat-insecure-origin-as-secure
```

Enable the flag and add exactly:

```text
http://192.168.168.172:8182
```

Relaunch Chromium, then use the HTTP page and `ws://192.168.168.172:8183/ws/audio`. This weakens browser security and is not recommended for normal browsing. HTTPS/WSS is the supported configuration.

## Repository hygiene

Build outputs, WASM distribution files, native private keys, and generated certificates are ignored. Do not commit private keys. Re-run `./scripts/setup.sh` and `./web_client/build_wasm.sh` after cloning a fresh checkout.

## Known limitations

- The current real-time audio callback still performs dynamic allocations and uses mutex-protected client ring buffers; this should be improved before demanding low-latency production use.
- The native server's web asset paths are relative to the process working directory.
- Browser microphone permissions and certificate trust are controlled by the browser/device, not by the WASM module.
- The dashboard displays hardware meters first and per-client channel meters below them, including per-channel history graphs.
- The raw-value checkbox explicitly shows `OFF — HIDDEN` or `ON — VISIBLE`; when enabled, hardware and client cards show current raw sample and block peak values.
- The MVP is intended for a trusted LAN with zero application authentication and zero manual enrollment; clients connect and appear automatically.
- Browser client IDs and names are stored in browser `localStorage`. Reconnecting reuses the saved identity; a simultaneous reuse receives an identity-conflict prompt before a new ID is created.

## Planning and status tracking

See [`PLANNER.md`](PLANNER.md) for the detailed networked audio patchbay plan, architecture, implementation phases, and the `TODO` / `PLANNED` / `DEPRECATING` / `NEEDREVIEW` register.
