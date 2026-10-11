#include "dsp_engine.h"
#include "web_server.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <queue>

DspEngine::DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics, ToneControls& tone) 
    : m_deviceManager(deviceManager), m_clientManager(clientManager), m_metrics(metrics), m_tone(tone) {
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

void DspEngine::update_meters(const std::vector<float>& buffer, unsigned channels, const std::string& endpointId, bool isCapture) {
    if (buffer.empty()) return;
    
    size_t frames = buffer.size() / 8; 
    auto& targetMeters = isCapture ? m_metrics.capture[endpointId] : m_metrics.playback[endpointId];

    for (unsigned ch = 0; ch < std::min(channels, (unsigned)CM5_MAX_CHANNELS); ++ch) {
        float sumSq = 0.0f;
        float peak = 0.0f;
        
        for (size_t f = 0; f < frames; ++f) {
            float sample = buffer[f * 8 + ch];
            float absS = std::abs(sample);
            sumSq += sample * sample;
            if (absS > peak) peak = absS;
        }
        
        float rms = std::sqrt(sumSq / frames);
        float rmsDb = (rms > 0.00001f) ? 20.0f * std::log10(rms) : -60.0f;
        float peakDb = (peak > 0.00001f) ? 20.0f * std::log10(peak) : -60.0f;

        targetMeters[ch].raw_value.store(rms, std::memory_order_relaxed);
        targetMeters[ch].raw_peak.store(peak, std::memory_order_relaxed);
        targetMeters[ch].rms_db.store(rmsDb, std::memory_order_relaxed);
        targetMeters[ch].peak_db.store(peakDb, std::memory_order_relaxed);
        targetMeters[ch].clipped.store(peak > 1.0f, std::memory_order_relaxed);
    }
}

void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started with Node-Centric DAG Routing." << std::endl;
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
    const auto routes_ptr = m_clientManager.route_snapshot();
    if (!routes_ptr) return;
    const auto& routes = *routes_ptr;

    const size_t frames = 480; 
    const size_t internalWidth = 8; 

    std::unordered_set<std::string> allNodes;
    std::unordered_map<std::string, std::vector<std::string>> adj;
    std::unordered_map<std::string, int> inDegree;

    for (const auto& route : routes) {
        allNodes.insert(route.source_endpoint);
        allNodes.insert(route.destination_endpoint);
        adj[route.source_endpoint].push_back(route.destination_endpoint);
        inDegree[route.destination_endpoint]++;
    }

    for (auto& dev : activeDevices) {
        const auto& info = dev->get_info();
        if (info.is_capture) allNodes.insert(std::string("hardware/") + info.id.alsa + "/capture");
        if (!info.is_capture) allNodes.insert(std::string("hardware/") + info.id.alsa + "/playback");
    }
    
    allNodes.insert("tone/generator");

    std::vector<std::string> sortedNodes;
    std::queue<std::string> q;
    for (const auto& node : allNodes) {
        if (inDegree[node] == 0) q.push(node);
    }

    while (!q.empty()) {
        std::string u = q.front(); q.pop();
        sortedNodes.push_back(u);
        for (const auto& v : adj[u]) {
            if (--inDegree[v] == 0) q.push(v);
        }
    }

    std::unordered_map<std::string, std::vector<float>> nodeBuffers;
    for (const auto& node : allNodes) {
        nodeBuffers[node].assign(frames * internalWidth, 0.0f);
    }

    for (const auto& node : sortedNodes) {
        // --- INPUT PHASE ---
        if (node == "tone/generator") {
            if (m_tone.enabled) {
                float freq = m_tone.frequency_hz;
                float amp = m_tone.amplitude;
                for (size_t f = 0; f < frames; ++f) {
                    float sample = std::sin(m_tonePhase * 2.0 * M_PI) * amp;
                    nodeBuffers[node][f * internalWidth + 0] = sample; // Tone is mono on channel 0
                    m_tonePhase += freq / m_engineSampleRate;
                    if (m_tonePhase > 1.0) m_tonePhase -= 1.0;
                }
            }
        }
        else if (node.find("/capture") != std::string::npos && node.find("hardware/") == 0) {
            std::string id = node.substr(9, node.find("/capture") - 9);
            for (auto& dev : activeDevices) {
                if (dev->get_info().id.alsa == id && dev->get_info().is_capture) {
                    const auto& info = dev->get_info();
                    std::vector<float> tmpBuf(frames * info.channels);
                    size_t read = dev->get_input_buffer().read(tmpBuf.data(), tmpBuf.size() * sizeof(float));
                    if (read > 0) {
                        for (size_t f = 0; f < frames; ++f) {
                            for (unsigned ch = 0; ch < info.channels; ++ch) {
                                nodeBuffers[node][f * internalWidth + ch] = tmpBuf[f * info.channels + ch];
                            }
                        }
                        update_meters(nodeBuffers[node], info.channels, node, true);
                    }
                    break;
                }
            }
        }
        else if (node.find("/capture") != std::string::npos && node.find("client/") == 0) {
            uint32_t clientId = std::stoul(node.substr(7, node.find("/capture") - 7));
            auto session = m_clientManager.get_active_sessions(); // Note: This returns a vector, need a specific lookup
            for (auto& s : session) {
                if (s->id == clientId) {
                    std::vector<float> tmpBuf(frames * s->input_channels);
                    size_t read = s->incoming_rb.read(tmpBuf.data(), tmpBuf.size() * sizeof(float));
                    if (read > 0) {
                        for (size_t f = 0; f < frames; ++f) {
                            for (unsigned ch = 0; ch < s->input_channels; ++ch) {
                                nodeBuffers[node][f * internalWidth + ch] = tmpBuf[f * s->input_channels + ch];
                            }
                        }
                        update_meters(nodeBuffers[node], s->input_channels, node, true);
                    }
                    break;
                }
            }
        }

        // --- ROUTING PHASE ---
        for (const auto& route : routes) {
            if (route.source_endpoint == node && route.enabled) {
                auto& srcBuf = nodeBuffers[node];
                auto& dstBuf = nodeBuffers[route.destination_endpoint];
                if (route.source_channel < internalWidth && route.destination_channel < internalWidth) {
                    for (size_t f = 0; f < frames; ++f) {
                        dstBuf[f * internalWidth + route.destination_channel] += srcBuf[f * internalWidth + route.source_channel] * route.gain;
                    }
                }
            }
        }

        // --- OUTPUT PHASE ---
        if (node.find("/playback") != std::string::npos && node.find("hardware/") == 0) {
            std::string id = node.substr(9, node.find("/playback") - 9);
            for (auto& dev : activeDevices) {
                if (dev->get_info().id.alsa == id && !dev->get_info().is_capture) {
                    const auto& info = dev->get_info();
                    auto& buf = nodeBuffers[node];
                    m_gainProc->process(buf.data(), frames, internalWidth);
                    m_compProc->process(buf.data(), frames, internalWidth);
                    update_meters(buf, info.channels, node, false);
                    std::vector<float> outBuf(frames * info.channels);
                    for (size_t f = 0; f < frames; ++f) {
                        for (unsigned ch = 0; ch < info.channels; ++ch) {
                            outBuf[f * info.channels + ch] = buf[f * internalWidth + ch];
                        }
                    }
                    dev->get_output_buffer().write(outBuf.data(), outBuf.size() * sizeof(float));
                    break;
                }
            }
        }
        else if (node.find("/playback") != std::string::npos && node.find("client/") == 0) {
            uint32_t clientId = std::stoul(node.substr(7, node.find("/playback") - 7));
            auto sessions = m_clientManager.get_active_sessions();
            for (auto& s : sessions) {
                if (s->id == clientId) {
                    auto& buf = nodeBuffers[node];
                    std::vector<float> outBuf(frames * s->output_channels);
                    for (size_t f = 0; f < frames; ++f) {
                        for (unsigned ch = 0; ch < s->output_channels; ++ch) {
                            outBuf[f * s->output_channels + ch] = buf[f * internalWidth + ch];
                        }
                    }
                    s->outgoing_rb.write(outBuf.data(), outBuf.size() * sizeof(float));
                    update_meters(buf, s->output_channels, node, false);
                    break;
                }
            }
        }
    }
}
