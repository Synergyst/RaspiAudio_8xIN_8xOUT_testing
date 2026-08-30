#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

# Ensure output directory exists.
mkdir -p web_client/dist

if ! command -v emcc >/dev/null 2>&1; then
    echo "[ERROR] Emscripten compiler (emcc) not found in PATH." >&2
    echo "Please install/activate the Emscripten SDK first." >&2
    exit 1
fi

echo "=================================================="
echo "Compiling Miniaudio WASM Client with Emscripten..."
echo "=================================================="

# client_main.cpp contains the browser bridge; miniaudio_impl.cpp supplies the
# miniaudio implementation. Both are required at link time.
emcc web_client/client_main.cpp src/miniaudio_impl.cpp \
    -o web_client/dist/client_audio.js \
    -Iinclude \
    -O3 \
    -s WASM=1 \
    -lwebsocket \
    -s EXPORTED_FUNCTIONS='["_start_web_audio", "_set_web_audio_identity", "_stop_web_audio"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap", "allocateUTF8"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s NO_EXIT_RUNTIME=1

echo "[SUCCESS] Build complete! Output generated in web_client/dist/"
