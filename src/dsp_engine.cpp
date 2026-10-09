#include "dsp_engine.h"
#include "web_server.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>

DspEngine::DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics) 
    : m_deviceManager(deviceManager), m_clientManager(clientManager), m_metrics(metrics) {
    m_mixBuffer.resize(m_engineSampleRate * m_engineChannels, 0.0f);
    m_gainProc = std::make_unique<GainProcessor>(1.0f);
    m_compProc = std::make_unique<CompressorProcessor>(0.7f, 4.0f);
}

DspEngine::~DspEngine() { stop(); }

void DspEngine::start() {
    if (m_running) return;
    m_running = true;
    m_workerThread = std::thread(&DspEngine::processing_loop, this);
}

void DspEngine::stop() {
    m_running = false;
    if (m_workerThread.joinable()) m_workerThread.join();
}

void DspEngine::update_meters(const std::vector<float>& buffer, unsigned channels, bool isCapture) {
    if (buffer.empty()) return;
    
    size_t frames = buffer.size() / channels;
    for (unsigned ch = 0; ch < std::min(channels, (unsigned)CM5_MAX_CHANNELS); ++ch) {
        float sumSq = 0.0f;
        float peak = 0.0f;
        
        for (size_t f = 0; f < frames; ++f) {
            float sample = buffer[f * channels + ch];
            float absS = std::abs(sample);
            sumSq += sample * sample;
            if (absS > peak) peak = absS;
        }
        
        float rms = std::sqrt(sumSq / frames);
        float rmsDb = (rms > 0.00001f) ? 20.0f * std::log10(rms) : -60.0f;
        float peakDb = (peak > 0.00001f) ? 20.0f * std::log10(peak) : -60.0f;

        if (isCapture) {
            m_metrics.capture[ch].raw_value.store(rms, std::memory_order_relaxed);
            m_metrics.capture[ch].raw_peak.store(peak, std::memory_order_relaxed);
            m_metrics.capture[ch].rms_db.store(rmsDb, std::memory_order_relaxed);
            m_metrics.capture[ch].peak_db.store(peakDb, std::memory_order_relaxed);
            m_metrics.capture[ch].clipped.store(peak > 1.0f, std::memory_order_relaxed);
        } else {
            m_metrics.playback[ch].raw_value.store(rms, std::memory_order_relaxed);
            m_metrics.playback[ch].raw_peak.store(peak, std::memory_order_relaxed);
            m_metrics.playback[ch].rms_db.store(rmsDb, std::memory_order_relaxed);
            m_metrics.playback[ch].peak_db.store(peakDb, std::memory_order_relaxed);
            m_metrics.playback[ch].clipped.store(peak > 1.0f, std::memory_order_relaxed);
        }
    }
}

void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started with Granular Routing." << std::endl;
    while (m_running) {
        auto start_time = std::chrono::steady_clock::now();
        process_audio();
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        auto sleep_time = std::chrono::milliseconds(10) - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) std::this_thread::sleep_for(sleep_time);
    }
}

void DspEngine::process_audio() {
    auto activeDevices = m_deviceManager.get_active_devices();
    if (activeDevices.empty()) return;
    const auto routes = m_clientManager.route_snapshot();
    if (!routes) return;

    const size_t frames = 480; 
    struct DeviceCache { std::vector<float> buffer; unsigned channels; };
    std::unordered_map<std::string, DeviceCache> captureCaches;
    for (auto& dev : activeDevices) {
        if (dev->get_info().is_capture) {
            const auto& info = dev->get_info();
            size_t samplesNeeded = frames * info.channels;
            std::vector<float> buf(samplesNeeded, 0.0f);
            size_t read = dev->get_input_buffer().read(buf.data(), samplesNeeded * sizeof(float));
            if (read > 0) {
                // KEY FIX: Use ALSA ID instead of Name to match WebServer IDs
                captureCaches[info.id.alsa] = { std::move(buf), info.channels };
                update_meters(captureCaches[info.id.alsa].buffer, info.channels, true);
            }
        }
    }

    struct PlaybackBuffer { std::vector<float> buffer; unsigned channels; };
    std::unordered_map<std::string, PlaybackBuffer> playbackBuffers;
    for (auto& dev : activeDevices) {
        if (!dev->get_info().is_capture) {
            const auto& info = dev->get_info();
            playbackBuffers[info.id.alsa] = { std::vector<float>(frames * info.channels, 0.0f), info.channels };
        }
    }

    for (const auto& route : *routes) {
        if (!route.enabled) continue;
        const float* srcData = nullptr;
        unsigned srcStride = 0;
        bool srcFound = false;

        // Handle hardware capture endpoints: hardware/[id]/capture
        if (route.source_endpoint.compare(0, 9, "hardware/") == 0 && route.source_endpoint.find("/capture") != std::string::npos) {
            size_t start = 9;
            size_t end = route.source_endpoint.find("/capture");
            std::string id = route.source_endpoint.substr(start, end - start);
            
            if (captureCaches.count(id)) {
                const auto& cache = captureCaches[id];
                if (route.source_channel < cache.channels) {
                    srcData = cache.buffer.data(); srcStride = cache.channels; srcFound = true;
                }
            }
        }

        if (!srcFound) continue;

        // Handle hardware playback endpoints: hardware/[id]/playback
        if (route.destination_endpoint.compare(0, 9, "hardware/") == 0 && route.destination_endpoint.find("/playback") != std::string::npos) {
            size_t start = 9;
            size_t end = route.destination_endpoint.find("/playback");
            std::string id = route.destination_endpoint.substr(start, end - start);
            
            if (playbackBuffers.count(id)) {
                auto& pb = playbackBuffers[id];
                if (route.destination_channel < pb.channels) {
                    for (size_t f = 0; f < frames; ++f)
                        pb.buffer[f * pb.channels + route.destination_channel] += srcData[f * srcStride + route.source_channel] * route.gain;
                }
            }
        }
    }

    for (auto& dev : activeDevices) {
        if (!dev->get_info().is_capture) {
            auto it = playbackBuffers.find(dev->get_info().id.alsa);
            if (it == playbackBuffers.end()) continue;
            auto& pb = it->second;
            m_gainProc->process(pb.buffer.data(), frames, pb.channels);
            m_compProc->process(pb.buffer.data(), frames, pb.channels);
            update_meters(pb.buffer, pb.channels, false);
            dev->get_output_buffer().write(pb.buffer.data(), pb.buffer.size() * sizeof(float));
        }
    }
}
