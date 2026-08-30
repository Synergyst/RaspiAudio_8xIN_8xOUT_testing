#include "../miniaudio/miniaudio.h"
#include "web_server.h"
#include <iostream>
#include <cstring>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>

std::atomic<bool> g_running(true);
AudioMetrics g_metrics;
AudioControls g_controls;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

inline float lin_to_db(float lin) {
    if (lin <= 0.00001f) return -60.0f;
    float db = 20.0f * std::log10(lin);
    return db < -60.0f ? -60.0f : (db > 0.0f ? 0.0f : db);
}

void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (pInput == NULL || pOutput == NULL || frameCount == 0) return;

    const float* in = static_cast<const float*>(pInput);
    float* out = static_cast<float*>(pOutput);

    float cap_gains[8];
    float pb_gains[8];

    for (int c = 0; c < 8; ++c) {
        bool cap_mute = g_controls.capture[c].mute.load(std::memory_order_relaxed);
        cap_gains[c] = cap_mute ? 0.0f : g_controls.capture[c].gain.load(std::memory_order_relaxed);

        bool pb_mute = g_controls.playback[c].mute.load(std::memory_order_relaxed);
        pb_gains[c] = pb_mute ? 0.0f : g_controls.playback[c].gain.load(std::memory_order_relaxed);
    }

    float in_sum_sq[8] = {0};
    float in_peak[8]   = {0};
    float out_sum_sq[8] = {0};
    float out_peak[8]  = {0};

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        for (int c = 0; c < 8; ++c) {
            float raw_in = in[i * 8 + c];

            // 1. Software Capture DSP (Gain / Mute)
            float proc_in = raw_in * cap_gains[c];

            // Check clipping on capture
            float abs_in = std::abs(proc_in);
            if (abs_in >= 0.999f) {
                g_metrics.capture[c].clipped.store(true, std::memory_order_relaxed);
            }

            in_sum_sq[c] += proc_in * proc_in;
            if (abs_in > in_peak[c]) in_peak[c] = abs_in;

            // 2. Software Playback DSP (Gain / Mute)
            float final_out = proc_in * pb_gains[c];
            out[i * 8 + c] = final_out;

            // Check clipping on playback
            float abs_out = std::abs(final_out);
            if (abs_out >= 0.999f) {
                g_metrics.playback[c].clipped.store(true, std::memory_order_relaxed);
            }

            out_sum_sq[c] += final_out * final_out;
            if (abs_out > out_peak[c]) out_peak[c] = abs_out;
        }
    }

    // Update global atomic metrics & peak hold values
    for (int c = 0; c < 8; ++c) {
        // Capture Peak & Peak Hold Decay
        float in_rms = std::sqrt(in_sum_sq[c] / frameCount);
        float in_peak_db = lin_to_db(in_peak[c]);
        g_metrics.capture[c].rms_db.store(lin_to_db(in_rms), std::memory_order_relaxed);
        g_metrics.capture[c].peak_db.store(in_peak_db, std::memory_order_relaxed);

        float cap_cur_hold = g_metrics.capture[c].peak_hold_db.load(std::memory_order_relaxed);
        if (in_peak_db > cap_cur_hold) {
            g_metrics.capture[c].peak_hold_db.store(in_peak_db, std::memory_order_relaxed);
        } else {
            // Decay peak hold marker by 0.15 dB per buffer block
            g_metrics.capture[c].peak_hold_db.store(std::max(-60.0f, cap_cur_hold - 0.15f), std::memory_order_relaxed);
        }

        // Playback Peak & Peak Hold Decay
        float out_rms = std::sqrt(out_sum_sq[c] / frameCount);
        float out_peak_db = lin_to_db(out_peak[c]);
        g_metrics.playback[c].rms_db.store(lin_to_db(out_rms), std::memory_order_relaxed);
        g_metrics.playback[c].peak_db.store(out_peak_db, std::memory_order_relaxed);

        float pb_cur_hold = g_metrics.playback[c].peak_hold_db.load(std::memory_order_relaxed);
        if (out_peak_db > pb_cur_hold) {
            g_metrics.playback[c].peak_hold_db.store(out_peak_db, std::memory_order_relaxed);
        } else {
            g_metrics.playback[c].peak_hold_db.store(std::max(-60.0f, pb_cur_hold - 0.15f), std::memory_order_relaxed);
        }
    }
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "Starting CM5 Audio Engine..." << std::endl;

    WebServer webServer(g_metrics, g_controls, 8182);
    if (!webServer.start()) {
        std::cerr << "Failed to start HTTP server!" << std::endl;
    }

    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_duplex);
    deviceConfig.capture.format   = ma_format_f32;
    deviceConfig.capture.channels = 8;
    deviceConfig.playback.format  = ma_format_f32;
    deviceConfig.playback.channels= 8;
    deviceConfig.sampleRate      = 48000;
    deviceConfig.dataCallback    = data_callback;

    ma_device device;
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        std::cerr << "Failed to initialize audio device!" << std::endl;
        webServer.stop();
        return -1;
    }

    if (ma_device_start(&device) != MA_SUCCESS) {
        std::cerr << "Failed to start audio device!" << std::endl;
        ma_device_uninit(&device);
        webServer.stop();
        return -1;
    }

    std::cout << "Operational. Metering at http://192.168.168.172:8182/" << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutting down..." << std::endl;
    ma_device_uninit(&device);
    webServer.stop();

    return 0;
}
