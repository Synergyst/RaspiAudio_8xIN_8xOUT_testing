#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>
#include "../include/miniaudio.h"

#define SAMPLE_RATE 48000
#define MIC_CHANNELS 1
#define SPK_CHANNELS 2
#define FRAME_SIZE 480

struct PcmRingBuffer {
    std::vector<uint8_t> buffer;
    size_t head = 0, tail = 0, count = 0, capacity = 0;
    bool init(size_t bytes) { buffer.resize(bytes); capacity = bytes; head = tail = count = 0; return true; }
    void uninit() { buffer.clear(); capacity = head = tail = count = 0; }
    size_t write(const void* data, size_t bytes) {
        if (!capacity || !bytes) return 0; const size_t n = std::min(bytes, capacity - count);
        const auto* src = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { buffer[head] = src[i]; head = (head + 1) % capacity; }
        count += n; return n;
    }
    size_t read(void* data, size_t bytes) {
        if (!capacity || !bytes) return 0; const size_t n = std::min(bytes, count);
        auto* dst = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { dst[i] = buffer[tail]; tail = (tail + 1) % capacity; }
        count -= n; return n;
    }
    size_t available_read() const { return count; }
};

static ma_device g_device;
static PcmRingBuffer g_capture_rb, g_playback_rb;
static EMSCRIPTEN_WEBSOCKET_T g_ws = 0;
static bool g_audio_running = false;

void client_audio_callback(ma_device*, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (pInput) g_capture_rb.write(pInput, frameCount * MIC_CHANNELS * sizeof(float));
    if (pOutput) {
        const size_t bytes = frameCount * SPK_CHANNELS * sizeof(float);
        const size_t got = g_playback_rb.read(pOutput, bytes);
        if (got < bytes) std::memset(static_cast<char*>(pOutput) + got, 0, bytes - got);
    }
}

EM_BOOL on_ws_open(int, const EmscriptenWebSocketOpenEvent*, void*) {
    const char hello[] = "{\"type\":\"hello\",\"input_channels\":1,\"output_channels\":2,\"sample_rate\":48000,\"format\":\"f32\"}";
    if (g_ws > 0) emscripten_websocket_send_utf8_text(g_ws, hello);
    std::cout << "[WASM Audio] Connected" << std::endl;
    return EM_TRUE;
}
EM_BOOL on_ws_message(int, const EmscriptenWebSocketMessageEvent* event, void*) {
    if (!event->isText && event->numBytes > 0) g_playback_rb.write(event->data, event->numBytes);
    return EM_TRUE;
}
EM_BOOL on_ws_error(int, const EmscriptenWebSocketErrorEvent*, void*) { std::cerr << "[WASM Audio] WebSocket error" << std::endl; return EM_TRUE; }
EM_BOOL on_ws_close(int, const EmscriptenWebSocketCloseEvent*, void*) { g_ws = 0; std::cout << "[WASM Audio] Closed" << std::endl; return EM_TRUE; }

void network_tick() {
    if (!g_audio_running || g_ws <= 0) return;
    const size_t bytes = FRAME_SIZE * MIC_CHANNELS * sizeof(float);
    if (g_capture_rb.available_read() >= bytes) {
        float block[FRAME_SIZE * MIC_CHANNELS];
        if (g_capture_rb.read(block, bytes)) emscripten_websocket_send_binary(g_ws, block, bytes);
    }
}

extern "C" {
EMSCRIPTEN_KEEPALIVE
int start_web_audio(const char* ws_url) {
    if (g_audio_running) return 0;
    g_capture_rb.init(SAMPLE_RATE * MIC_CHANNELS * sizeof(float));
    g_playback_rb.init(SAMPLE_RATE * SPK_CHANNELS * sizeof(float));
    EmscriptenWebSocketCreateAttributes attr = {ws_url, NULL, EM_TRUE};
    g_ws = emscripten_websocket_new(&attr);
    emscripten_websocket_set_onopen_callback(g_ws, NULL, on_ws_open);
    emscripten_websocket_set_onmessage_callback(g_ws, NULL, on_ws_message);
    emscripten_websocket_set_onerror_callback(g_ws, NULL, on_ws_error);
    emscripten_websocket_set_onclose_callback(g_ws, NULL, on_ws_close);
    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format = ma_format_f32; config.capture.channels = MIC_CHANNELS;
    config.playback.format = ma_format_f32; config.playback.channels = SPK_CHANNELS;
    config.sampleRate = SAMPLE_RATE; config.dataCallback = client_audio_callback;
    if (ma_device_init(NULL, &config, &g_device) != MA_SUCCESS) return -1;
    if (ma_device_start(&g_device) != MA_SUCCESS) { ma_device_uninit(&g_device); return -2; }
    g_audio_running = true;
    emscripten_set_main_loop(network_tick, 100, EM_FALSE);
    return 0;
}
EMSCRIPTEN_KEEPALIVE
void stop_web_audio() {
    if (!g_audio_running) return;
    emscripten_cancel_main_loop(); ma_device_uninit(&g_device);
    if (g_ws > 0) { emscripten_websocket_close(g_ws, 1000, "User disconnect"); emscripten_websocket_delete(g_ws); g_ws = 0; }
    g_capture_rb.uninit(); g_playback_rb.uninit(); g_audio_running = false;
}
}
