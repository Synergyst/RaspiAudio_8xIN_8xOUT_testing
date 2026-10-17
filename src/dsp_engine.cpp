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
#include <cstring>

#define CM5_ENABLE_PROBES 0

#if CM5_ENABLE_PROBES
static std::atomic<uint32_t> g_processCount{0};
#define SAMPLED_TRACE(msg, val) \
    if (++g_processCount % 100 == 0) { std::cout << "[TRACE] " << msg << " | Value: " << val << std::endl; }
#else
#define SAMPLED_TRACE(msg, val)
#endif

DspEngine::DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics, ToneControls& tone) 
    : m_deviceManager(deviceManager), m_clientManager(clientManager), m_metrics(metrics), m_tone(tone) {
    m_nodeBufferPool.resize(MAX_NODES * CM5_BUFFER_FRAMES * CM5_INTERNAL_WIDTH, 0.0f);
    m_feedbackPool.resize(MAX_NODES * CM5_BUFFER_FRAMES * CM5_INTERNAL_WIDTH, 0.0f);
    m_nextFeedbackPool.resize(MAX_NODES * CM5_BUFFER_FRAMES * CM5_INTERNAL_WIDTH, 0.0f);
    m_routeScratchBuffer.resize(CM5_BUFFER_FRAMES, 0.0f);
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

void DspEngine::set_route_dsp_active(uint32_t routeId, bool active) {
    if (routeId < MAX_ROUTES) m_routeDsp[routeId].active.store(active, std::memory_order_release);
}

void DspEngine::set_route_gain(uint32_t routeId, float gain) {
    if (routeId < MAX_ROUTES) m_routeDsp[routeId].gain.store(gain, std::memory_order_release);
}

void DspEngine::update_route_chain(uint32_t routeId, std::vector<std::unique_ptr<AudioProcessor>> newChain) {
    if (routeId >= MAX_ROUTES) return;
    RouteDsp& rdsp = m_routeDsp[routeId];
    int write_idx = 1 - rdsp.current_idx.load(std::memory_order_relaxed);
    for (unsigned i = 0; i < CM5_MAX_PROCESSORS_PER_ROUTE; ++i) rdsp.chain[write_idx][i] = nullptr;
    int len = std::min((int)newChain.size(), (int)CM5_MAX_PROCESSORS_PER_ROUTE);
    for (int i = 0; i < len; ++i) rdsp.chain[write_idx][i] = std::move(newChain[i]);
    rdsp.length.store(len, std::memory_order_release);
    rdsp.current_idx.store(write_idx, std::memory_order_release);
}

void DspEngine::update_meters(float* buffer, unsigned channels, const std::string& endpointId, bool isCapture) {
    if (!buffer) return;
    auto& targetMeters = isCapture ? m_metrics.capture[endpointId] : m_metrics.playback[endpointId];
    for (unsigned ch = 0; ch < std::min(channels, (unsigned)CM5_MAX_CHANNELS); ++ch) {
        float sumSq = 0.0f, peak = 0.0f;
        for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) {
            float sample = buffer[f * CM5_INTERNAL_WIDTH + ch];
            float absS = std::abs(sample);
            sumSq += sample * sample;
            if (absS > peak) peak = absS;
        }
        float rms = std::sqrt(sumSq / CM5_BUFFER_FRAMES);
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
    auto activeDevices = m_deviceManager.get_active_devices();
    for (auto& dev : activeDevices) {
        std::string origId = dev->get_original_id();
        NodeId captureId = m_clientManager.get_node_id("hardware/" + origId + "/capture");
        NodeId playbackId = m_clientManager.get_node_id("hardware/" + origId + "/playback");
        if (captureId != NODE_INVALID) activeNodes.insert(captureId);
        if (playbackId != NODE_INVALID) activeNodes.insert(playbackId);
    }
    activeNodes.insert(NODE_TONE_GENERATOR);
    std::vector<NodeId> sortedNodes;
    std::queue<NodeId> q;
    for (const auto& node : activeNodes) if (inDegree[node] == 0) q.push(node);
    while (!q.empty()) {
        NodeId u = q.front(); q.pop();
        sortedNodes.push_back(u);
        for (const auto& v : adj[u]) if (--inDegree[v] == 0) q.push(v);
    }
    m_executionOrder = std::move(sortedNodes);
    std::fill(std::begin(m_nodePosition), std::end(m_nodePosition), -1);
    for (size_t i = 0; i < m_executionOrder.size(); ++i) {
        NodeId node = m_executionOrder[i];
        if (node >= 0 && node < (int)MAX_NODES) m_nodePosition[node] = (int)i;
    }
    for (auto& h : m_nodeHandlers) h.active = false;
    for (NodeId node : m_executionOrder) {
        if (node < 0 || node >= (int)MAX_NODES) continue;
        std::string endpoint = m_clientManager.get_endpoint_from_id(node);
        if (node == NODE_TONE_GENERATOR) {
            m_nodeHandlers[node] = {NodeHandler::Type::TONE, nullptr, CM5_INTERNAL_WIDTH, "tone/generator", true};
        } else if (endpoint.find("hardware/") == 0) {
            size_t firstSlash = 9, secondSlash = endpoint.find("/", firstSlash);
            if (secondSlash != std::string::npos) {
                std::string id = endpoint.substr(firstSlash, secondSlash - firstSlash);
                bool isCapture = (endpoint.find("/capture") != std::string::npos);
                for (auto& dev : activeDevices) {
                    if ((std::string(dev->get_info().id.alsa) == id || dev->get_original_id() == id) &&
                        (dev->get_info().is_capture == isCapture)) {
                        m_nodeHandlers[node] = {isCapture ? NodeHandler::Type::HARDWARE_CAPTURE : NodeHandler::Type::HARDWARE_PLAYBACK, dev.get(), dev->get_info().channels, endpoint, true};
                        break;
                    }
                }
            }
        } else if (endpoint.find("client/") == 0) {
            size_t firstSlash = 7, secondSlash = endpoint.find("/", firstSlash);
            if (secondSlash != std::string::npos) {
                std::string key = endpoint.substr(firstSlash, secondSlash - firstSlash);
                bool isCapture = (endpoint.find("/capture") != std::string::npos);
                auto sessions = m_clientManager.get_active_sessions();
                for (auto& s : sessions) {
                    if (s->get_client_key() == key) {
                        m_nodeHandlers[node] = {isCapture ? NodeHandler::Type::CLIENT_CAPTURE : NodeHandler::Type::CLIENT_PLAYBACK, s.get(), isCapture ? s->input_channels.load() : s->output_channels.load(), endpoint, true};
                        break;
                    }
                }
            }
        }
        if (!m_nodeHandlers[node].active) m_nodeHandlers[node] = {NodeHandler::Type::VIRTUAL, nullptr, CM5_INTERNAL_WIDTH, "virtual/" + std::to_string(node), true};
    }
    m_currentRoutes = routes_ptr;
    for (size_t i = 0; i < MAX_NODES; ++i) m_adjList[i].clear();
    for (const auto& route : routes) if (route.source_node >= 0 && route.source_node < (int)MAX_NODES && route.enabled) m_adjList[route.source_node].push_back(&route);
    m_graphDirty.store(false, std::memory_order_release);
}

/*void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started (True Zero-Allocation)." << std::endl;
    while (m_running) {
        auto start_time = std::chrono::steady_clock::now();
        process_audio();
        auto end_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        auto sleep_time = std::chrono::milliseconds(10) - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) std::this_thread::sleep_for(sleep_time);
    }
}*/
/*void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started (True Zero-Allocation)." << std::endl;

    auto next_wakeup = std::chrono::steady_clock::now();
    const auto interval = std::chrono::microseconds(20000); // 20 ms (matches periodSizeInFrames = 960)

    while (m_running) {
        next_wakeup += interval;

        process_audio();

        auto now = std::chrono::steady_clock::now();
        if (next_wakeup > now) {
            std::this_thread::sleep_until(next_wakeup);
        } else {
            // If processing took longer than 20 ms, reset baseline to prevent CPU spin locking
            next_wakeup = now;
        }
    }
}*/
/*void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started (Dynamic Timing)." << std::endl;

    // Dynamically calculate exact microsecond interval from constants & sample rate
    // e.g., (960 frames / 48000 Hz) * 1,000,000 = 20,000 microseconds (20ms)
    auto interval = std::chrono::microseconds(
        (static_cast<long long>(CM5_BUFFER_FRAMES) * 1000000LL) / m_engineSampleRate
    );

    auto next_wakeup = std::chrono::steady_clock::now();

    while (m_running) {
        next_wakeup += interval;

        process_audio();

        auto now = std::chrono::steady_clock::now();
        if (next_wakeup > now) {
            std::this_thread::sleep_until(next_wakeup);
        } else {
            next_wakeup = now; // Prevent CPU spinlock if a cycle runs long
        }
    }
}*/
void DspEngine::processing_loop() {
    std::cout << "[DspEngine] Processing loop started (Dynamic Unified Timing)." << std::endl;

    // Mathematical interval: (Frames / SampleRate) * 1,000,000 microseconds
    auto interval = std::chrono::microseconds(
        (static_cast<long long>(CM5_BUFFER_FRAMES) * 1000000LL) / m_engineSampleRate
    );

    auto next_wakeup = std::chrono::steady_clock::now();

    while (m_running) {
        next_wakeup += interval;

        process_audio();

        auto now = std::chrono::steady_clock::now();
        if (next_wakeup > now) {
            std::this_thread::sleep_until(next_wakeup);
        } else {
            next_wakeup = now; // Prevent spinlock if an iteration runs long
        }
    }
}

void DspEngine::process_audio() {
    if (m_graphDirty.load(std::memory_order_acquire)) rebuild_graph();
    if (!m_currentRoutes) return;
    std::fill(m_nodeBufferPool.begin(), m_nodeBufferPool.end(), 0.0f);

    for (NodeId node : m_executionOrder) {
        float* buf = get_buffer(node);
        if (!buf || node < 0 || node >= (int)MAX_NODES || !m_nodeHandlers[node].active) continue;
        const NodeHandler& handler = m_nodeHandlers[node];

        if (handler.type == NodeHandler::Type::TONE) {
            if (m_tone.enabled) {
                float freq = m_tone.frequency_hz, amp = m_tone.amplitude;
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) {
                    float sample = std::sin(m_tonePhase * 2.0 * M_PI) * amp;
                    buf[f * CM5_INTERNAL_WIDTH + 0] += sample;
                    m_tonePhase += freq / m_engineSampleRate;
                    if (m_tonePhase > 1.0) m_tonePhase -= 1.0;
                }
            }
        } else if (handler.type == NodeHandler::Type::HARDWARE_CAPTURE) {
            auto* dev = static_cast<HardwareDevice*>(handler.ptr);
            size_t read = dev->get_input_buffer().read(dev->io_buffer.data(), CM5_BUFFER_FRAMES * dev->get_info().channels * sizeof(float));
            if (read > 0) {
                float peak = 0.0f;
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) {
                    for (unsigned ch = 0; ch < dev->get_info().channels && ch < CM5_INTERNAL_WIDTH; ++ch) {
                        float s = dev->io_buffer[f * dev->get_info().channels + ch];
                        buf[f * CM5_INTERNAL_WIDTH + ch] += s;
                        if (std::abs(s) > peak) peak = std::abs(s);
                    }
                }
                SAMPLED_TRACE("PROBE B: Capture Node " + handler.endpointId + " read", peak);
                update_meters(buf, dev->get_info().channels, handler.endpointId, true);
            }
        } else if (handler.type == NodeHandler::Type::CLIENT_CAPTURE) {
            auto* s = static_cast<WebClientSession*>(handler.ptr);
            size_t read = s->incoming_rb.read(s->io_buffer.data(), CM5_BUFFER_FRAMES * s->input_channels.load() * sizeof(float));
            if (read > 0) {
                unsigned chs = s->input_channels.load();
                for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) {
                    for (unsigned ch = 0; ch < chs && ch < CM5_INTERNAL_WIDTH; ++ch) {
                        buf[f * CM5_INTERNAL_WIDTH + ch] += s->io_buffer[f * chs + ch];
                    }
                }
                update_meters(buf, chs, handler.endpointId, true);
            }
        }

        if (node >= 0 && node < (int)MAX_NODES) {
            for (const auto* route : m_adjList[node]) {
                float* dstBuf = get_buffer(route->destination_node);
                if (dstBuf && route->source_channel < CM5_INTERNAL_WIDTH && route->destination_channel < CM5_INTERNAL_WIDTH) {
                    bool isFeedback = false;
                    if (m_nodePosition[node] != -1 && m_nodePosition[route->destination_node] != -1) {
                        if (m_nodePosition[node] >= m_nodePosition[route->destination_node]) isFeedback = true;
                    }
                    float* srcData = isFeedback ? get_feedback_buffer(node) : buf;
                    for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) m_routeScratchBuffer[f] = srcData[f * CM5_INTERNAL_WIDTH + route->source_channel];
                    float currentGain = route->gain;
                    if (route->id >= 0 && (size_t)route->id < MAX_ROUTES) {
                        RouteDsp& rdsp = m_routeDsp[route->id];
                        if (rdsp.active.load(std::memory_order_acquire)) {
                            int idx = rdsp.current_idx.load(std::memory_order_acquire), len = rdsp.length.load(std::memory_order_acquire);
                            for (int i = 0; i < len; ++i) if (rdsp.chain[idx][i]) rdsp.chain[idx][i]->process(m_routeScratchBuffer.data(), CM5_BUFFER_FRAMES, 1);
                            currentGain = rdsp.gain.load(std::memory_order_acquire);
                        }
                    }
                    for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) dstBuf[f * CM5_INTERNAL_WIDTH + route->destination_channel] += m_routeScratchBuffer[f] * currentGain;
                }
            }
        }

        if (handler.type == NodeHandler::Type::HARDWARE_PLAYBACK) {
            auto* dev = static_cast<HardwareDevice*>(handler.ptr);
            m_gainProc->process(buf, CM5_BUFFER_FRAMES, CM5_INTERNAL_WIDTH);
            m_compProc->process(buf, CM5_BUFFER_FRAMES, CM5_INTERNAL_WIDTH);
            float peak = 0.0f;
            unsigned chs = dev->get_info().channels;
            for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) {
                for (unsigned ch = 0; ch < chs && ch < CM5_INTERNAL_WIDTH; ++ch) {
                    float s = buf[f * CM5_INTERNAL_WIDTH + ch];
                    dev->io_buffer[f * chs + ch] = s;
                    if (std::abs(s) > peak) peak = std::abs(s);
                }
            }
            SAMPLED_TRACE("PROBE C: Playback Node " + handler.endpointId + " writing", peak);
            update_meters(buf, chs, handler.endpointId, false);
            dev->get_output_buffer().write(dev->io_buffer.data(), CM5_BUFFER_FRAMES * chs * sizeof(float));
        } else if (handler.type == NodeHandler::Type::CLIENT_PLAYBACK) {
            auto* s = static_cast<WebClientSession*>(handler.ptr);
            unsigned chs = s->output_channels.load();
            for (size_t f = 0; f < CM5_BUFFER_FRAMES; ++f) {
                for (unsigned ch = 0; ch < chs && ch < CM5_INTERNAL_WIDTH; ++ch) s->io_buffer[f * chs + ch] = buf[f * CM5_INTERNAL_WIDTH + ch];
            }
            s->outgoing_rb.write(s->io_buffer.data(), CM5_BUFFER_FRAMES * chs * sizeof(float));
            update_meters(buf, chs, handler.endpointId, false);
        }
        float* nextFb = get_next_feedback_buffer(node);
        if (nextFb) std::memcpy(nextFb, buf, CM5_BUFFER_FRAMES * CM5_INTERNAL_WIDTH * sizeof(float));
    }
    std::swap(m_feedbackPool, m_nextFeedbackPool);
}
