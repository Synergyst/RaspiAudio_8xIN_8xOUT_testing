#!/bin/bash
set -e

# Ensure output directory exists
mkdir -p web_client/dist

# Verify emcc availability
if ! command -v emcc &> /dev/null; then
    echo "[ERROR] Emscripten compiler (emcc) not found in PATH."
    echo "Please activate your Emscripten SDK environment first:"
    echo "  source /path/to/emsdk/emsdk_env.sh"
    exit 1
fi

echo "=================================================="
echo "Compiling Miniaudio WASM Client with Emscripten..."
echo "=================================================="

emcc web_client/client_main.cpp -o web_client/dist/client_audio.js \
    -Iinclude \
    -O3 \
    -s WASM=1 \
    -lwebsocket \
    -s EXPORTED_FUNCTIONS='["_start_web_audio", "_stop_web_audio", "_main"]' \
    -s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap", "allocateUTF8"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s NO_EXIT_RUNTIME=1

echo "[SUCCESS] Build complete! Output generated in web_client/dist/"
