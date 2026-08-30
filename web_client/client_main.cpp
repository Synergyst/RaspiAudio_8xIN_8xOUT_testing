#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>

#include "../include/miniaudio.h"

#define SAMPLE_RATE 48000
#define MIC_CHANNELS 1
#define SPK_CHANNELS 2
#define FRAME_SIZE 480 // 10ms block @ 48kHz

struct PcmRingBuffer {
    std::vector<uint8_t> buffer;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    size_t capacity = 0;

    bool init(size_t capacityBytes) {
        buffer.resize(capacityBytes);
        capacity = capacityBytes;
        head = 0;
        tail = 0;
        count = 0;
        return true;
    }

    void uninit() {
        buffer.clear();
        capacity = 0;
        head = 0;
        tail = 0;
        count = 0;
    }

    size_t write(const void* data, size_t bytes) {
        if (capacity == 0 || bytes == 0) return 0;
        size_t bytesToWrite = std::min(bytes, capacity - count);
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytesToWrite; ++i) {
            buffer[head] = src[i];
            head = (head + 1) % capacity;
        }
        count += bytesToWrite;
        return bytesToWrite;
    }

    size_t read(void* data, size_t bytes) {
        if (capacity == 0 || bytes == 0) return 0;
        size_t bytesToRead = std::min(bytes, count);
        uint8_t* dst = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < bytesToRead; ++i) {
            dst[i] = buffer[tail];
            tail = (tail + 1) % capacity;
        }
        count -= bytesToRead;
        return bytesToRead;
    }

    size_t available_read() const {
        return count;
    }
};

static ma_device g_device;
static PcmRingBuffer g_capture_rb;   // Mic audio waiting to be sent to server
static PcmRingBuffer g_playback_rb;  // Server audio waiting to be played on speakers

static EMSCRIPTEN_WEBSOCKET_T g_ws = 0;
static bool g_audio_running = false;

void client_audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (pInput != NULL) {
        size_t bytesToWrite = frameCount * MIC_CHANNELS * sizeof(float);
        g_capture_rb.write(pInput, bytesToWrite);
    }

    if (pOutput != NULL) {
        size_t reqBytes = frameCount * SPK_CHANNELS * sizeof(float);
        size_t bytesToRead = g_playback_rb.read(pOutput, reqBytes);

        if (bytesToRead < reqBytes) {
            size_t missingBytes = reqBytes - bytesToRead;
            std::memset(static_cast<char*>(pOutput) + bytesToRead, 0, missingBytes);
        }
    }
}

EM_BOOL on_ws_open(int eventType, const EmscriptenWebSocketOpenEvent* websocketEvent, void* userData) {
    std::cout << "[WASM Audio] WebSocket Connected to CM5 Server" << std::endl;
    return EM_TRUE;
}

EM_BOOL on_ws_message(int eventType, const EmscriptenWebSocketMessageEvent* websocketEvent, void* userData) {
    if (websocketEvent->isText == EM_FALSE && websocketEvent->numBytes > 0) {
        g_playback_rb.write(websocketEvent->data, websocketEvent->numBytes);
    }
    return EM_TRUE;
}

EM_BOOL on_ws_error(int eventType, const EmscriptenWebSocketErrorEvent* websocketEvent, void* userData) {
    std::cerr << "[WASM Audio] WebSocket Error" << std::endl;
    return EM_TRUE;
}

EM_BOOL on_ws_close(int eventType, const EmscriptenWebSocketCloseEvent* websocketEvent, void* userData) {
    std::cout << "[WASM Audio] WebSocket Closed" << std::endl;
    g_ws = 0;
    return EM_TRUE;
}

void network_tick() {
    if (!g_audio_running || g_ws <= 0) return;

    size_t reqBytes = FRAME_SIZE * MIC_CHANNELS * sizeof(float);
    if (g_capture_rb.available_read() >= reqBytes) {
        float sendBuffer[FRAME_SIZE * MIC_CHANNELS];
        size_t readBytes = g_capture_rb.read(sendBuffer, reqBytes);
        if (readBytes > 0) {
            emscripten_websocket_send_binary(g_ws, sendBuffer, readBytes);
        }
    }
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
int start_web_audio(const char* ws_url) {
    if (g_audio_running) return 0;

    g_capture_rb.init(SAMPLE_RATE * MIC_CHANNELS * sizeof(float));
    g_playback_rb.init(SAMPLE_RATE * SPK_CHANNELS * sizeof(float));

    EmscriptenWebSocketCreateAttributes attr = {
        ws_url,
        NULL,
        EM_TRUE
    };
    g_ws = emscripten_websocket_new(&attr);
    emscripten_websocket_set_onopen_callback(g_ws, NULL, on_ws_open);
    emscripten_websocket_set_onmessage_callback(g_ws, NULL, on_ws_message);
    emscripten_websocket_set_onerror_callback(g_ws, NULL, on_ws_error);
    emscripten_websocket_set_onclose_callback(g_ws, NULL, on_ws_close);

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format   = ma_format_f32;
    config.capture.channels = MIC_CHANNELS;
    config.playback.format  = ma_format_f32;
    config.playback.channels= SPK_CHANNELS;
    config.sampleRate       = SAMPLE_RATE;
    config.dataCallback     = client_audio_callback;

    if (ma_device_init(NULL, &config, &g_device) != MA_SUCCESS) {
        std::cerr << "[WASM Audio] Failed to initialize miniaudio device" << std::endl;
        return -1;
    }

    if (ma_device_start(&g_device) != MA_SUCCESS) {
        std::cerr << "[WASM Audio] Failed to start miniaudio device" << std::endl;
        ma_device_uninit(&g_device);
        return -2;
    }

    g_audio_running = true;
    emscripten_set_main_loop(network_tick, 100, EM_FALSE);

    return 0;
}

EMSCRIPTEN_KEEPALIVE
void stop_web_audio() {
    if (!g_audio_running) return;

    emscripten_cancel_main_loop();
    ma_device_uninit(&g_device);
    g_capture_rb.uninit();
    g_playback_rb.uninit();

    if (g_ws > 0) {
        emscripten_websocket_close(g_ws, 1000, "User disconnect");
        emscripten_websocket_delete(g_ws);
        g_ws = 0;
    }

    g_audio_running = false;
    std::cout << "[WASM Audio] Engine stopped" << std::endl;
}

}
