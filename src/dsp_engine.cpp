#include "dsp_engine.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>

DspEngine::DspEngine(DeviceManager& deviceManager) : m_deviceManager(deviceManager) {
    // Pre-allocate mixing buffer (approx 100ms of stereo at 48kHz)
    m_mixBuffer.resize(m_engineSampleRate * m_engineChannels / 10, 0.0f);

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

    std::fill(m_mixBuffer.begin(), m_mixBuffer.end(), 0.0f);
    
    size_t totalSamplesProcessed = 0;
    bool has_input = false;

    // 1. SUMMING STAGE
    for (auto& dev : activeDevices) {
        if (dev->get_info().is_capture) {
            PcmRingBuffer& rb = dev->get_input_buffer();
            size_t samplesToRead = 480 * m_engineChannels; 
            std::vector<float> tempBuffer(samplesToRead);
            
            size_t read = rb.read(tempBuffer.data(), samplesToRead * sizeof(float));
            if (read > 0) {
                has_input = true;
                size_t numSamples = read / sizeof(float);
                for (size_t i = 0; i < numSamples && i < m_mixBuffer.size(); ++i) {
                    m_mixBuffer[i] += tempBuffer[i];
                }
                totalSamplesProcessed = std::max(totalSamplesProcessed, numSamples);
            }
        }
    }

    if (!has_input) return;

    // 2. DSP STAGE: Apply the processor chain to the mixed audio
    // First: Gain
    m_gainProc->process(m_mixBuffer.data(), totalSamplesProcessed / m_engineChannels, m_engineChannels);
    
    // Second: Compression
    m_compProc->process(m_mixBuffer.data(), totalSamplesProcessed / m_engineChannels, m_engineChannels);

    // 3. OUTPUT STAGE
    for (auto& dev : activeDevices) {
        if (!dev->get_info().is_capture) {
            PcmRingBuffer& rb = dev->get_output_buffer();
            rb.write(m_mixBuffer.data(), totalSamplesProcessed * sizeof(float));
        }
    }
}
