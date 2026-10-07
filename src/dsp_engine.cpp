#include "dsp_engine.h"
#include "web_server.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>

DspEngine::DspEngine(DeviceManager& deviceManager, ClientManager& clientManager) 
    : m_deviceManager(deviceManager), m_clientManager(clientManager) {
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
            if (read > 0) captureCaches[info.name] = { std::move(buf), info.channels };
        }
    }

    struct PlaybackBuffer { std::vector<float> buffer; unsigned channels; };
    std::unordered_map<std::string, PlaybackBuffer> playbackBuffers;
    for (auto& dev : activeDevices) {
        if (!dev->get_info().is_capture) {
            const auto& info = dev->get_info();
            playbackBuffers[info.name] = { std::vector<float>(frames * info.channels, 0.0f), info.channels };
        }
    }

    for (const auto& route : *routes) {
        if (!route.enabled) continue;
        const float* srcData = nullptr;
        unsigned srcStride = 0;
        bool srcFound = false;

        if (route.source_endpoint == "hardware/capture" && !captureCaches.empty()) {
            auto it = captureCaches.begin();
            if (route.source_channel < it->second.channels) {
                srcData = it->second.buffer.data(); srcStride = it->second.channels; srcFound = true;
            }
        } else if (route.source_endpoint.compare(0, 17, "hardware/capture/") == 0) {
            std::string name = route.source_endpoint.substr(17);
            if (captureCaches.count(name)) {
                const auto& cache = captureCaches[name];
                if (route.source_channel < cache.channels) {
                    srcData = cache.buffer.data(); srcStride = cache.channels; srcFound = true;
                }
            }
        }

        if (!srcFound) continue;

        if (route.destination_endpoint == "hardware/playback") {
            for (auto& pbPair : playbackBuffers) {
                auto& pb = pbPair.second;
                if (route.destination_channel < pb.channels) {
                    for (size_t f = 0; f < frames; ++f) 
                        pb.buffer[f * pb.channels + route.destination_channel] += srcData[f * srcStride + route.source_channel] * route.gain;
                }
            }
        } else if (route.destination_endpoint.compare(0, 18, "hardware/playback/") == 0) {
            std::string name = route.destination_endpoint.substr(18);
            if (playbackBuffers.count(name)) {
                auto& pb = playbackBuffers[name];
                if (route.destination_channel < pb.channels) {
                    for (size_t f = 0; f < frames; ++f)
                        pb.buffer[f * pb.channels + route.destination_channel] += srcData[f * srcStride + route.source_channel] * route.gain;
                }
            }
        }
    }

    for (auto& dev : activeDevices) {
        if (!dev->get_info().is_capture) {
            auto it = playbackBuffers.find(dev->get_info().name);
            if (it == playbackBuffers.end()) continue;
            auto& pb = it->second;
            m_gainProc->process(pb.buffer.data(), frames, pb.channels);
            m_compProc->process(pb.buffer.data(), frames, pb.channels);
            dev->get_output_buffer().write(pb.buffer.data(), pb.buffer.size() * sizeof(float));
        }
    }
}
