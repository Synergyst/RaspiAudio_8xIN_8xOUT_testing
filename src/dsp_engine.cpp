#include "dsp_engine.h"
#include "web_server.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>

DspEngine::DspEngine(DeviceManager& deviceManager, ClientManager& clientManager) 
    : m_deviceManager(deviceManager), m_clientManager(clientManager) {
    // Pre-allocate mixing buffer (approx 100ms of stereo at 48kHz)
    m_mixBuffer.resize(m_engineSampleRate * m_engineChannels, 0.0f);

    // Initialize default processor chain
    m_gainProc = std::make_unique<GainProcessor>(1.0f);
    m_compProc = std::make_unique<CompressorProcessor>(0.7f, 4.0f); // Default threshold 0.7, ratio 4:1
}

DspEngine::~DspEngine() {
    stop();
}

void DspEngine::start() {
    if (m_running) return;
    m_running = true;
    m_workerThread = std::thread(&DspEngine::processing_loop, this);
}

void DspEngine::stop() {
    m_running = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started with FX chain." << std::endl;
    
    while (m_running) {
        auto start_time = std::chrono::steady_clock::now();
        
        process_audio();
        
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        auto sleep_time = std::chrono::milliseconds(10) - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) {
            std::this_thread::sleep_for(sleep_time);
        }
    }
    
    std::cout << "[DspEngine] Processing loop stopped." << std::endl;
}

void DspEngine::process_audio() {
    auto activeDevices = m_deviceManager.get_active_devices();
    if (activeDevices.empty()) return;

    const auto routes = m_clientManager.route_snapshot();
    if (!routes) return;

    // Use 10ms frames (480 samples at 48kHz)
    const size_t frames = 480;
    const size_t totalSamples = frames * m_engineChannels;
    std::vector<float> masterMix(totalSamples, 0.0f);
    
    // 1. Capture Stage: Read from all active hardware capture devices into a temporary cache
    // Since we have multiple devices, we'll sum them into a "virtual hardware capture" buffer
    std::vector<float> hwCaptureCache(totalSamples, 0.0f);
    bool has_input = false;

    for (auto& dev : activeDevices) {
        if (dev->get_info().is_capture) {
            PcmRingBuffer& rb = dev->get_input_buffer();
            std::vector<float> temp(totalSamples);
            size_t read = rb.read(temp.data(), totalSamples * sizeof(float));
            if (read > 0) {
                has_input = true;
                size_t numSamples = read / sizeof(float);
                for (size_t i = 0; i < numSamples; ++i) {
                    hwCaptureCache[i] += temp[i];
                }
            }
        }
    }

    // 2. Routing Stage: Apply routes from hardware/capture to hardware/playback
    bool routed_audio = false;
    for (const auto& route : *routes) {
        if (!route.enabled) continue;
        if (route.source_endpoint == "hardware/capture" && route.destination_endpoint == "hardware/playback") {
            if (route.source_channel >= m_engineChannels || route.destination_channel >= m_engineChannels) continue;
            
            for (size_t f = 0; f < frames; ++f) {
                float sample = hwCaptureCache[f * m_engineChannels + route.source_channel];
                masterMix[f * m_engineChannels + route.destination_channel] += sample * route.gain;
            }
            routed_audio = true;
        }
    }

    if (!routed_audio) return;

    // 3. DSP STAGE: Apply the processor chain to the routed mix
    m_gainProc->process(masterMix.data(), frames, m_engineChannels);
    m_compProc->process(masterMix.data(), frames, m_engineChannels);

    // 4. Output Stage: Write to all active hardware playback devices
    for (auto& dev : activeDevices) {
        if (!dev->get_info().is_capture) {
            PcmRingBuffer& rb = dev->get_output_buffer();
            rb.write(masterMix.data(), totalSamples * sizeof(float));
        }
    }
}
