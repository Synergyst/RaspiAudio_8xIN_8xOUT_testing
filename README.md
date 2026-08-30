# CM5 Audio 8x8 Engine

A Raspberry Pi CM5 audio engine for 8-channel duplex capture/playback with a browser dashboard and a browser microphone/audio bridge.

## Features

- 8-channel capture and playback at 48 kHz using [miniaudio](https://miniaud.io/)
- Per-channel capture/playback gain and mute controls
- RMS and peak meters with clip latching and peak hold
- HTTP(S) dashboard and static web client
- WebSocket (WS/WSS) PCM bridge for browser clients
- Emscripten/WASM browser client

## Runtime endpoints

When started from the repository root with the supplied TLS certificate:

- Dashboard: `https://192.168.168.172:8182/`
- Browser audio client: `https://192.168.168.172:8182/client/index.html`
- Audio WebSocket: `wss://192.168.168.172:8183/ws/audio`
- Meter API: `GET /api/meters`
- Control API: `POST /api/control?type=capture|playback&ch=0..7&gain=1.0&mute=0|1`
- Clip reset API: `POST /api/reset_clips?type=capture|playback&ch=0..7` or `POST /api/reset_clips?all=1`

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

Generated private keys, certificates, and certificate configuration files are ignored by Git.

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

The native process requires an available 8-channel, 48 kHz duplex audio device. On a headless CM5, verify the selected ALSA device and channel capabilities if initialization fails.

## Browser microphone access

`getUserMedia()` requires a secure browser context. Use the HTTPS client URL, not the HTTP URL:

```text
https://192.168.168.172:8182/client/index.html
```

Because the setup helper creates a self-signed certificate, Chromium will initially show a certificate warning. For a development-only LAN setup:

1. Open the HTTPS URL.
2. Choose **Advanced**, then **Proceed to 192.168.168.172 (unsafe)**.
3. In the address-bar site settings, set **Microphone** to **Allow**.
4. Reload the page and confirm the WebSocket field is:

   ```text
   wss://192.168.168.172:8183/ws/audio
   ```

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
