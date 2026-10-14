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
    
    // Pre-allocate the node buffer pool: MAX_NODES * (BUFFER_FRAMES * INTERNAL_WIDTH)
    m_nodeBufferPool.resize(MAX_NODES * BUFFER_FRAMES * INTERNAL_WIDTH, 0.0f);
    
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

void DspEngine::update_meters(float* buffer, unsigned channels, const std::string& endpointId, bool isCapture) {
    if (!buffer) return;
    
    auto& targetMeters = isCapture ? m_metrics.capture[endpointId] : m_metrics.playback[endpointId];

    for (unsigned ch = 0; ch < std::min(channels, (unsigned)CM5_MAX_CHANNELS); ++ch) {
        float sumSq = 0.0f;
        float peak = 0.0f;
        
        for (size_t f = 0; f < BUFFER_FRAMES; ++f) {
            float sample = buffer[f * INTERNAL_WIDTH + ch];
            float absS = std::abs(sample);
            sumSq += sample * sample;
            if (absS > peak) peak = absS;
        }
        
        float rms = std::sqrt(sumSq / BUFFER_FRAMES);
        float rmsDb = (rms > 0.00001f) ? 20.0f * std::log10(rms) : -60.0f;
        float peakDb = (peak > 0.00001f) ? 20.0f * std::log10(peak) : -60.0f;

        targetMeters[ch].raw_value.store(rms, std::memory_order_relaxed);
        targetMeters[ch].raw_peak.store(peak, std::memory_order_relaxed);
        targetMeters[ch].rms_db.store(rmsDb, std::memory_order_relaxed);
        targetMeters[ch].peak_db.store(peakDb, std::memory_order_relaxed);
        targetMeters[ch].clipped.store(peak > 1.0f, std::memory_order_relaxed);
    }
}

void DspEngine::rebuild_graph() {
    const auto routes_ptr = m_clientManager.route_snapshot();
    if (!routes_ptr) return;
    const auto& routes = *routes_ptr;

    std::unordered_set<NodeId> activeNodes;
    std::unordered_map<NodeId, std::vector<NodeId>> adj;
    std::unordered_map<NodeId, int> inDegree;

    for (const auto& route : routes) {
        if (route.source_node == NODE_INVALID || route.destination_node == NODE_INVALID) continue;
        activeNodes.insert(route.source_node);
        activeNodes.insert(route.destination_node);
        adj[route.source_node].push_back(route.destination_node);
        inDegree[route.destination_node]++;
    }

    // Add hardware and tone nodes to the set of nodes to process
    auto activeDevices = m_deviceManager.get_active_devices();
    for (auto& dev : activeDevices) {
        const auto& info = dev->get_info();
        // We rely on the ClientManager to have registered these and assigned them IDs
        NodeId captureId = m_clientManager.get_node_id("hardware/" + std::string(info.id.alsa) + "/capture");
        NodeId playbackId = m_clientManager.get_node_id("hardware/" + std::string(info.id.alsa) + "/playback");
        if (captureId != NODE_INVALID) activeNodes.insert(captureId);
        if (playbackId != NODE_INVALID) activeNodes.insert(playbackId);
    }
    activeNodes.insert(NODE_TONE_GENERATOR);

    // Kahn's Algorithm for Topological Sort
    std::vector<NodeId> sortedNodes;
    std::queue<NodeId> q;
    for (const auto& node : activeNodes) {
        if (inDegree[node] == 0) q.push(node);
    }

    while (!q.empty()) {
        NodeId u = q.front(); q.pop();
        sortedNodes.push_back(u);
        for (const auto& v : adj[u]) {
            if (--inDegree[v] == 0) q.push(v);
        }
    }

    m_executionOrder = std::move(sortedNodes);
    m_graphDirty.store(false, std::memory_order_release);
}

void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started (Zero-Allocation Mode)." << std::endl;
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
    if (m_graphDirty.load(std::memory_order_acquire)) {
        rebuild_graph();
    }

    const auto routes_ptr = m_clientManager.route_snapshot();
    if (!routes_ptr) return;
    const auto& routes = *routes_ptr;

    // Clear all buffers in the pool for this cycle
    std::fill(m_nodeBufferPool.begin(), m_nodeBufferPool.end(), 0.0f);

    auto activeDevices = m_deviceManager.get_active_devices();

    for (NodeId node : m_executionOrder) {
        float* buf = get_buffer(node);
        if (!buf) continue;

        // --- INPUT PHASE ---
        if (node == NODE_TONE_GENERATOR) {
            if (m_tone.enabled) {
                float freq = m_tone.frequency_hz;
                float amp = m_tone.amplitude;
                for (size_t f = 0; f < BUFFER_FRAMES; ++f) {
                    float sample = std::sin(m_tonePhase * 2.0 * M_PI) * amp;
                    buf[f * INTERNAL_WIDTH + 0] = sample;
                    m_tonePhase += freq / m_engineSampleRate;
                    if (m_tonePhase > 1.0) m_tonePhase -= 1.0;
                }
            }
        }
        else {
            std::string endpoint = m_clientManager.get_endpoint_from_id(node);
            if (endpoint.find("/capture") != std::string::npos) {
                if (endpoint.find("hardware/") == 0) {
                    std::string id = endpoint.substr(9, endpoint.find("/capture") - 9);
                    for (auto& dev : activeDevices) {
                        if (dev->get_info().id.alsa == id && dev->get_info().is_capture) {
                            const auto& info = dev->get_info();
                            std::vector<float> tmpBuf(BUFFER_FRAMES * info.channels);
                            size_t read = dev->get_input_buffer().read(tmpBuf.data(), tmpBuf.size() * sizeof(float));
                            if (read > 0) {
                                for (size_t f = 0; f < BUFFER_FRAMES; ++f) {
                                    for (unsigned ch = 0; ch < info.channels && ch < INTERNAL_WIDTH; ++ch) {
                                        buf[f * INTERNAL_WIDTH + ch] = tmpBuf[f * info.channels + ch];
                                    }
                                }
                                update_meters(buf, info.channels, endpoint, true);
                            }
                            break;
                        }
                    }
                }
                else if (endpoint.find("client/") == 0) {
                    uint32_t clientId = std::stoul(endpoint.substr(7, endpoint.find("/capture") - 7));
                    auto sessions = m_clientManager.get_active_sessions();
                    for (auto& s : sessions) {
                        if (s->id == clientId) {
                            std::vector<float> tmpBuf(BUFFER_FRAMES * s->input_channels);
                            size_t read = s->incoming_rb.read(tmpBuf.data(), tmpBuf.size() * sizeof(float));
                            if (read > 0) {
                                for (size_t f = 0; f < BUFFER_FRAMES; ++f) {
                                    for (unsigned ch = 0; ch < s->input_channels && ch < INTERNAL_WIDTH; ++ch) {
                                        buf[f * INTERNAL_WIDTH + ch] = tmpBuf[f * s->input_channels + ch];
                                    }
                                }
                                update_meters(buf, s->input_channels, endpoint, true);
                            }
                            break;
                        }
                    }
                }
            }
        }

        // --- ROUTING PHASE ---
        for (const auto& route : routes) {
            if (route.source_node == node && route.enabled) {
                float* dstBuf = get_buffer(route.destination_node);
                if (dstBuf && route.source_channel < INTERNAL_WIDTH && route.destination_channel < INTERNAL_WIDTH) {
                    for (size_t f = 0; f < BUFFER_FRAMES; ++f) {
                        dstBuf[f * INTERNAL_WIDTH + route.destination_channel] += buf[f * INTERNAL_WIDTH + route.source_channel] * route.gain;
                    }
                }
            }
        }

        // --- OUTPUT PHASE ---
        std::string endpoint = m_clientManager.get_endpoint_from_id(node);
        if (endpoint.find("/playback") != std::string::npos) {
            if (endpoint.find("hardware/") == 0) {
                std::string id = endpoint.substr(9, endpoint.find("/playback") - 9);
                for (auto& dev : activeDevices) {
                    if (dev->get_info().id.alsa == id && !dev->get_info().is_capture) {
                        const auto& info = dev->get_info();
                        m_gainProc->process(buf, BUFFER_FRAMES, INTERNAL_WIDTH);
                        m_compProc->process(buf, BUFFER_FRAMES, INTERNAL_WIDTH);
                        update_meters(buf, info.channels, endpoint, false);
                        std::vector<float> outBuf(BUFFER_FRAMES * info.channels);
                        for (size_t f = 0; f < BUFFER_FRAMES; ++f) {
                            for (unsigned ch = 0; ch < info.channels && ch < INTERNAL_WIDTH; ++ch) {
                                outBuf[f * info.channels + ch] = buf[f * INTERNAL_WIDTH + ch];
                            }
                        }
                        dev->get_output_buffer().write(outBuf.data(), outBuf.size() * sizeof(float));
                        break;
                    }
                }
            }
            else if (endpoint.find("client/") == 0) {
                uint32_t clientId = std::stoul(endpoint.substr(7, endpoint.find("/playback") - 7));
                auto sessions = m_clientManager.get_active_sessions();
                for (auto& s : sessions) {
                    if (s->id == clientId) {
                        std::vector<float> outBuf(BUFFER_FRAMES * s->output_channels);
                        for (size_t f = 0; f < BUFFER_FRAMES; ++f) {
                            for (unsigned ch = 0; ch < s->output_channels && ch < INTERNAL_WIDTH; ++ch) {
                                outBuf[f * s->output_channels + ch] = buf[f * INTERNAL_WIDTH + ch];
                            }
                        }
                        s->outgoing_rb.write(outBuf.data(), outBuf.size() * sizeof(float));
                        update_meters(buf, s->output_channels, endpoint, false);
                        break;
                    }
                }
            }
        }
    }
}
