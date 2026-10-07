#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <cstring>

// Simple Ring Buffer for the demo
struct SimpleRingBuffer {
    std::vector<float> data;
    size_t write_pos = 0;
    size_t read_pos = 0;
    size_t size = 0;

    SimpleRingBuffer(size_t capacity) : data(capacity), size(capacity) {}

    void write(const float* src, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            data[write_pos] = src[i];
            write_pos = (write_pos + 1) % size;
        }
    }

    void read(float* dst, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            dst[i] = data[read_pos];
            read_pos = (read_pos + 1) % size;
        }
    }
};

// Global state for the bridge
std::atomic<bool> g_running{true};
SimpleRingBuffer g_bridge_buffer(4800 * 2); 

// Input Callback: Hardware -> Ring Buffer
void capture_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (pInput) {
        // Note: we use the config's channels to calculate total samples
        g_bridge_buffer.write((const float*)pInput, frameCount * pDevice->playback.channels);
    }
}

// Output Callback: Ring Buffer -> Hardware
void playback_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (pOutput) {
        g_bridge_buffer.read((float*)pOutput, frameCount * pDevice->playback.channels);
    }
}

int main() {
    ma_context context;
    ma_result result = ma_context_init(NULL, 0, NULL, &context);
    if (result != MA_SUCCESS) return 1;

    const char* input_id = ":1,0";  // USB PnP
    const char* output_id = ":0,0"; // HiFiBerry

    ma_device_info in_info, out_info;
    ma_device_id in_id = {0}, out_id = {0};
    std::strncpy(in_id.alsa, input_id, 255);
    std::strncpy(out_id.alsa, output_id, 255);

    // DEMO 1: Probe Capabilities
    ma_context_get_device_info(&context, ma_device_type_capture, &in_id, &in_info);
    ma_context_get_device_info(&context, ma_device_type_playback, &out_id, &out_info);

    // Access the first native format for the "default" hardware settings
    ma_uint32 inRate = in_info.nativeDataFormats[0].sampleRate;
    ma_uint32 inChan = in_info.nativeDataFormats[0].channels;
    ma_uint32 outRate = out_info.nativeDataFormats[0].sampleRate;
    ma_uint32 outChan = out_info.nativeDataFormats[0].channels;

    std::cout << "PROBE RESULTS:" << std::endl;
    std::cout << "Input (" << input_id << "): " << in_info.name << " | " << inRate << "Hz | " << inChan << " ch" << std::endl;
    std::cout << "Output (" << output_id << "): " << out_info.name << " | " << outRate << "Hz | " << outChan << " ch" << std::endl;

    // DEMO 2: The Bridge
    ma_device_config capture_config = ma_device_config_init(ma_device_type_capture);
    capture_config.capture.format = ma_format_f32;
    capture_config.capture.channels = inChan;
    capture_config.sampleRate = inRate;
    capture_config.dataCallback = capture_callback;
    capture_config.pUserData = NULL;

    ma_device capture_device;
    if (ma_device_init(&context, &capture_config, &capture_device) != MA_SUCCESS) {
        std::cerr << "Failed to init capture device" << std::endl;
        return 1;
    }

    ma_device_config playback_config = ma_device_config_init(ma_device_type_playback);
    playback_config.playback.format = ma_format_f32;
    playback_config.playback.channels = outChan;
    playback_config.sampleRate = outRate;
    playback_config.dataCallback = playback_callback;
    playback_config.pUserData = NULL;

    ma_device playback_device;
    if (ma_device_init(&context, &playback_config, &playback_device) != MA_SUCCESS) {
        std::cerr << "Failed to init playback device" << std::endl;
        return 1;
    }

    ma_device_start(&capture_device);
    ma_device_start(&playback_device);

    std::cout << "\nBRIDGE ACTIVE: Audio is flowing from " << input_id << " to " << output_id << std::endl;
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();

    ma_device_uninit(&capture_device);
    ma_device_uninit(&playback_device);
    ma_context_uninit(&context);

    return 0;
}
