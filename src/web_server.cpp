#include "web_server.h"
#include "dsp_engine.h"
#include "device_manager.h"
#include <nlohmann/json.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace {

// Internal cache to track DSP chain state for persistence since RouteDsp is owned by DspEngine
struct RouteDspState {
    bool active = false;
    float gain = 1.0f;
    std::vector<nlohmann::json> chain; // Stores { "type": "gain", "params": {...} }
};
std::unordered_map<uint32_t, RouteDspState> g_dspSettingsCache;
std::mutex g_dspCacheLock;

std::string json_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '\"' || ch == '\\') { result += '\\'; result += ch; }
        else if (ch == '\n') result += "\\n";
        else if (ch == '\r') result += "\\r";
        else result += ch;
    }
    return result;
}

unsigned query_uint(const httplib::Request& request, const char* name, unsigned fallback) {
    if (!request.has_param(name)) return fallback;
    try { return static_cast<unsigned>(std::stoul(request.get_param_value(name))); }
    catch (...) { return fallback; }
}

float query_float(const httplib::Request& request, const char* name, float fallback) {
    if (!request.has_param(name)) return fallback;
    try { return std::stof(request.get_param_value(name)); }
    catch (...) { return fallback; }
}

bool query_bool(const httplib::Request& request, const char* name, bool fallback) {
    if (!request.has_param(name)) return fallback;
    const auto value = request.get_param_value(name);
    return value == "1" || value == "true" || value == "on";
}

bool parse_identity_message(const std::string& text, std::string& key, std::string& name,
                            unsigned& inputChannels, unsigned& outputChannels) {
    try {
        const auto object = nlohmann::json::parse(text);
        if (object.value("type", "") != "hello") return false;
        key = object.value("client_id", object.value("client_key", ""));
        name = object.value("client_name", object.value("name", ""));
        if (object.contains("input_channels")) inputChannels = object["input_channels"].get<unsigned>();
        if (object.contains("output_channels")) outputChannels = object["output_channels"].get<unsigned>();
        return !key.empty();
    } catch (...) { return false; }
}

std::string read_dashboard() {
    std::ifstream file("./web_client/dashboard.html", std::ios::in | std::ios::binary);
    if (!file) return "<!doctype html><html><body><h1>Dashboard unavailable</h1></body></html>";
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string read_favicon() {
    std::ifstream file("./web_client/favicon.ico", std::ios::in | std::ios::binary);
    if (!file) return ""; 
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

nlohmann::json build_meter_json(const std::array<ChannelMeter, CM5_MAX_CHANNELS>& meters, unsigned channels) {
    nlohmann::json arr = nlohmann::json::array();
    unsigned count = std::min(channels, CM5_MAX_CHANNELS);
    for (unsigned i = 0; i < count; ++i) {
        const auto& m = meters[i];
        nlohmann::json mj;
        mj["ch"] = i;
        mj["raw_value"] = m.raw_value.load(std::memory_order_relaxed);
        mj["raw_peak"] = m.raw_peak.load(std::memory_order_relaxed);
        mj["rms_db"] = m.rms_db.load(std::memory_order_relaxed);
        mj["peak_db"] = m.peak_db.load(std::memory_order_relaxed);
        mj["peak_hold_db"] = m.peak_hold_db.load(std::memory_order_relaxed);
        mj["clipped"] = m.clipped.load(std::memory_order_relaxed);
        arr.push_back(mj);
    }
    return arr;
}

std::unique_ptr<AudioProcessor> create_processor(const std::string& type, const nlohmann::json& params) {
    if (type == "gain") {
        return std::make_unique<GainProcessor>(params.value("gain", 1.0f));
    } else if (type == "compressor") {
        return std::make_unique<CompressorProcessor>(
            params.value("threshold", 0.5f), 
            params.value("ratio", 4.0f)
        );
    } else {
        return std::make_unique<PassthroughProcessor>();
    }
}

} // namespace

WebClientSession::~WebClientSession() { stop_sender(); }

std::string WebClientSession::get_client_key() const {
    const auto key = std::atomic_load(&client_key);
    return key ? *key : std::string();
}

std::string WebClientSession::get_client_name() const {
    const auto name = std::atomic_load(&client_name);
    return name ? *name : std::string();
}

void WebClientSession::set_identity(const std::string& key, const std::string& name) {
    std::atomic_store(&client_key, std::make_shared<const std::string>(key));
    std::atomic_store(&client_name, std::make_shared<const std::string>(name));
}

void WebClientSession::start_sender(const std::shared_ptr<ix::WebSocket>& socket) {
    sender_thread = std::thread([this, socket] {
        std::vector<uint8_t> block(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CHANNELS * sizeof(float));
        while (active.load(std::memory_order_relaxed)) {
            const unsigned channels = std::clamp(output_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
            const size_t bytes = CM5_MAX_AUDIO_FRAMES * channels * sizeof(float);
            const size_t got = outgoing_rb.read(block.data(), bytes);
            if (got) socket->sendBinary(std::string(reinterpret_cast<char*>(block.data()), got));
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
}

void WebClientSession::stop_sender() {
    active.store(false, std::memory_order_relaxed);
    if (!sender_thread.joinable()) return;
    if (sender_thread.get_id() == std::this_thread::get_id()) {
        sender_thread.detach();
        return;
    }
    sender_thread.join();
}

ClientManager::ClientManager()
    : m_routes(std::make_shared<const std::vector<AudioRoute>>()) {
}

NodeId ClientManager::get_node_id(const std::string& endpoint) const {
    std::lock_guard<std::mutex> lock(m_node_lock);
    if (endpoint == "tone/generator") return NODE_TONE_GENERATOR;
    auto it = m_endpoint_to_id.find(endpoint);
    return (it != m_endpoint_to_id.end()) ? it->second : NODE_INVALID;
}

std::string ClientManager::get_endpoint_from_id(NodeId id) const {
    std::lock_guard<std::mutex> lock(m_node_lock);
    if (id == NODE_TONE_GENERATOR) return "tone/generator";
    auto it = m_id_to_endpoint.find(id);
    return (it != m_id_to_endpoint.end()) ? it->second : "";
}

NodeId ClientManager::register_node(const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(m_node_lock);
    if (endpoint == "tone/generator") return NODE_TONE_GENERATOR;
    
    auto it = m_endpoint_to_id.find(endpoint);
    if (it != m_endpoint_to_id.end()) return it->second;
    
    NodeId id = m_next_node_id.fetch_add(1);
    m_endpoint_to_id[endpoint] = id;
    m_id_to_endpoint[id] = endpoint;
    
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return id;
}

void ClientManager::unregister_node(const std::string& endpoint) {
    std::lock_guard<std::mutex> lock(m_node_lock);
    if (endpoint == "tone/generator") return;
    
    auto it = m_endpoint_to_id.find(endpoint);
    if (it == m_endpoint_to_id.end()) return;
    
    NodeId id = it->second;
    m_id_to_endpoint.erase(id);
    m_endpoint_to_id.erase(it);
    
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
}

void ClientManager::update_activated_device(const std::string& id, bool active) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = std::find(m_activatedDevices.begin(), m_activatedDevices.end(), id);
    if (active && it == m_activatedDevices.end()) {
        m_activatedDevices.push_back(id);
        register_node("hardware/" + id + "/capture");
        register_node("hardware/" + id + "/playback");
    } else if (!active && it != m_activatedDevices.end()) {
        m_activatedDevices.erase(it);
        unregister_node("hardware/" + id + "/capture");
        unregister_node("hardware/" + id + "/playback");
    }
}

std::shared_ptr<WebClientSession> ClientManager::create_session(uint32_t id, const std::string& remoteIp) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto session = std::make_shared<WebClientSession>();
    session->id = id;
    session->remote_ip = remoteIp;
    session->set_identity("connection-" + std::to_string(id), "Client " + std::to_string(id));
    session->incoming_rb.init(ma_format_f32, 1, 48000);
    session->outgoing_rb.init(ma_format_f32, 2, 48000);
    session->io_buffer.resize(CM5_BUFFER_FRAMES * CM5_MAX_CHANNELS);
    session->input_block.resize(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CHANNELS);
    session->output_block.resize(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CHANNELS);
    session->packet_block.resize(CM5_MAX_AUDIO_FRAMES * CM5_MAX_CHANNELS);
    session->active.store(true, std::memory_order_relaxed);
    m_sessions.push_back(session);
    return session;
}

void ClientManager::remove_session(uint32_t id) {
    std::shared_ptr<WebClientSession> removed;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
            [id](const auto& session) { return session->id == id; });
        if (it == m_sessions.end()) return;
        removed = *it;
        std::string key = removed->get_client_key();
        unregister_node("client/" + key + "/capture");
        unregister_node("client/" + key + "/playback");
        m_sessions.erase(it);
    }
    removed->stop_sender();
}

std::vector<std::shared_ptr<WebClientSession>> ClientManager::get_active_sessions() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::shared_ptr<WebClientSession>> active;
    for (auto& session : m_sessions)
        if (session->active.load(std::memory_order_relaxed)) active.push_back(session);
    return active;
}

bool ClientManager::claim_identity(const std::shared_ptr<WebClientSession>& session,
                                   const std::string& key, const std::string& name) {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(m_lock);
    for (const auto& other : m_sessions) {
        if (other.get() != session.get() && other->active.load(std::memory_order_relaxed) &&
            other->get_client_key() == key) return false;
    }
    const std::string displayName = name.empty() ? key : name;
    session->set_identity(key, displayName);
    m_known_client_names[key] = displayName;
    register_node("client/" + key + "/capture");
    register_node("client/" + key + "/playback");
    return true;
}

bool ClientManager::load_settings(AudioControls& controls, ToneControls& tone) {
    const char* configuredPath = std::getenv("CM5AUDIO_SETTINGS");
    const std::string path = configuredPath && *configuredPath ? configuredPath : "./cm5audio_settings.json";
    std::ifstream file(path);
    if (!file) return false;
    try {
        const auto settings = nlohmann::json::parse(file);
        if (settings.contains("tone") && settings["tone"].is_object()) {
            const auto& value = settings["tone"];
            tone.enabled.store(value.value("enabled", false), std::memory_order_relaxed);
            tone.frequency_hz.store(std::clamp(value.value("frequency_hz", 440.0f), 1.0f, 20000.0f), std::memory_order_relaxed);
            tone.amplitude.store(std::clamp(value.value("amplitude", 0.0f), 0.0f, 1.0f), std::memory_order_relaxed);
        }
        if (settings.contains("hardware") && settings["hardware"].is_object()) {
            const auto& hardware = settings["hardware"];
            for (const char* type : {"capture", "playback"}) {
                if (!hardware.contains(type) || !hardware[type].is_array()) continue;
                const auto& array = hardware[type];
                for (const auto& item : array) {
                    if (!item.contains("endpoint")) continue;
                    std::string epId = item["endpoint"];
                    auto& bank = (std::string(type) == "capture") ? controls.capture[epId] : controls.playback[epId];
                    unsigned ch = item.value("channel", 0u);
                    if (ch < CM5_MAX_CHANNELS) {
                        bank[ch].gain.store(std::clamp(item.value("gain", 1.0f), 0.0f, 8.0f), std::memory_order_relaxed);
                        bank[ch].mute.store(item.value("mute", false), std::memory_order_relaxed);
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_activatedDevices.clear();
            if (settings.contains("activated_devices") && settings["activated_devices"].is_array()) {
                for (const auto& id_val : settings["activated_devices"]) {
                    if (id_val.is_string()) {
                        std::string id = id_val.get<std::string>();
                        m_activatedDevices.push_back(id);
                        if (m_deviceManager) m_deviceManager->activate_device(id);
                        register_node("hardware/" + id + "/capture");
                        register_node("hardware/" + id + "/playback");
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> routeLock(m_route_lock);
            std::lock_guard<std::mutex> sessionLock(m_lock);
            std::vector<AudioRoute> routes;
            uint32_t nextId = 1;
            if (settings.contains("routes") && settings["routes"].is_array()) {
                for (const auto& value : settings["routes"]) {
                    AudioRoute route;
                    route.id = value.value("id", 0u);
                    std::string src_ep = value.value("source", "");
                    std::string dst_ep = value.value("destination", "");
                    route.source_node = get_node_id(src_ep);
                    route.destination_node = get_node_id(dst_ep);
                    route.source_channel = value.value("source_channel", CM5_MAX_CHANNELS);
                    route.destination_channel = value.value("destination_channel", CM5_MAX_CHANNELS);
                    route.gain = value.value("gain", 1.0f);
                    route.enabled = value.value("enabled", true);
                    if (!route.id || src_ep.empty() || dst_ep.empty() ||
                        route.source_channel >= CM5_MAX_CHANNELS || route.destination_channel >= CM5_MAX_CHANNELS ||
                        !std::isfinite(route.gain) || route.gain < 0.0f || route.gain > 8.0f) continue;
                    routes.push_back(route);
                    nextId = std::max(nextId, route.id + 1);

                    if (m_dspEngine && value.contains("dsp") && value["dsp"].is_object()) {
                        const auto& dsp = value["dsp"];
                        uint32_t rId = route.id;
                        
                        RouteDspState state;
                        state.active = dsp.value("active", false);
                        state.gain = dsp.value("gain", 1.0f);
                        if (dsp.contains("chain") && dsp["chain"].is_array()) {
                            state.chain = dsp["chain"].get<std::vector<nlohmann::json>>();
                        }
                        
                        {
                            std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
                            g_dspSettingsCache[rId] = state;
                        }

                        std::vector<std::unique_ptr<AudioProcessor>> chain;
                        if (dsp.contains("chain") && dsp["chain"].is_array()) {
                            for (const auto& procJson : dsp["chain"]) {
                                std::string type = procJson.value("type", "passthrough");
                                nlohmann::json params = procJson.value("params", nlohmann::json::object());
                                chain.push_back(create_processor(type, params));
                            }
                        }
                        m_dspEngine->update_route_chain(rId, std::move(chain));
                        m_dspEngine->set_route_dsp_active(rId, state.active);
                        m_dspEngine->set_route_gain(rId, state.gain);
                    }
                }
            }
            std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
            m_next_route_id.store(nextId, std::memory_order_relaxed);
            m_known_client_names.clear();
            if (settings.contains("clients") && settings["clients"].is_object()) {
                for (auto const& [key, name] : settings["clients"].items()) {
                    if (name.is_string()) m_known_client_names[key] = name.get<std::string>();
                }
            }
        }
        return true;
    } catch (...) { return false; }
}

bool ClientManager::save_settings(const AudioControls& controls, const ToneControls& tone) const {
    const char* configuredPath = std::getenv("CM5AUDIO_SETTINGS");
    const std::string path = configuredPath && *configuredPath ? configuredPath : "./cm5audio_settings.json";
    const std::string temporaryPath = path + ".tmp";
    nlohmann::json settings;
    settings["version"] = 1;
    settings["tone"] = {
        {"enabled", tone.enabled.load(std::memory_order_relaxed)},
        {"frequency_hz", tone.frequency_hz.load(std::memory_order_relaxed)},
        {"amplitude", tone.amplitude.load(std::memory_order_relaxed)}
    };
    settings["hardware"]["capture"] = nlohmann::json::array();
    for (auto const& [id, bank] : controls.capture) {
        for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
            settings["hardware"]["capture"].push_back({
                {"endpoint", id}, {"channel", i},
                {"gain", bank[i].gain.load(std::memory_order_relaxed)},
                {"mute", bank[i].mute.load(std::memory_order_relaxed)}
            });
        }
    }
    settings["hardware"]["playback"] = nlohmann::json::array();
    for (auto const& [id, bank] : controls.playback) {
        for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
            settings["hardware"]["playback"].push_back({
                {"endpoint", id}, {"channel", i},
                {"gain", bank[i].gain.load(std::memory_order_relaxed)},
                {"mute", bank[i].mute.load(std::memory_order_relaxed)}
            });
        }
    }
    {
        auto* mutableThis = const_cast<ClientManager*>(this);
        std::lock_guard<std::mutex> lock(mutableThis->m_lock);
        settings["activated_devices"] = nlohmann::json::array();
        for (const auto& id : mutableThis->m_activatedDevices) {
            settings["activated_devices"].push_back(id);
        }
    }
    {
        std::lock_guard<std::mutex> routeLock(m_route_lock);
        std::lock_guard<std::mutex> sessionLock(m_lock);
        settings["routes"] = nlohmann::json::array();
        for (const auto& route : *std::atomic_load(&m_routes)) {
            nlohmann::json rj;
            rj["id"] = route.id;
            rj["source"] = get_endpoint_from_id(route.source_node);
            rj["source_channel"] = route.source_channel;
            rj["destination"] = get_endpoint_from_id(route.destination_node);
            rj["destination_channel"] = route.destination_channel;
            rj["gain"] = route.gain;
            rj["enabled"] = route.enabled;

            {
                std::lock_guard<std::mutex> cacheLock(g_dspCacheLock);
                if (g_dspSettingsCache.count(route.id)) {
                    const auto& dsp = g_dspSettingsCache[route.id];
                    rj["dsp"] = {
                        {"active", dsp.active},
                        {"gain", dsp.gain},
                        {"chain", dsp.chain}
                    };
                }
            }
            settings["routes"].push_back(rj);
        }
        settings["clients"] = nlohmann::json::object();
        for (auto const& client : m_known_client_names) settings["clients"][client.first] = client.second;
        for (auto const& session : m_sessions) {
            const std::string key = session->get_client_key();
            if (!key.empty()) settings["clients"][key] = session->get_client_name();
        }
    }
    try {
        std::ofstream file(temporaryPath, std::ios::trunc);
        if (!file) return false;
        file << settings.dump(2) << "\n";
        file.close();
        return std::rename(temporaryPath.c_str(), path.c_str()) == 0;
    } catch (...) {
        std::remove(temporaryPath.c_str());
        return false;
    }
}

std::unordered_map<std::string, std::string> ClientManager::known_client_names() const {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_known_client_names;
}

std::shared_ptr<const std::vector<AudioRoute>> ClientManager::route_snapshot() const {
    return std::atomic_load(&m_routes);
}

std::vector<AudioRoute> ClientManager::get_routes() const {
    const auto routes = route_snapshot();
    return *routes;
}

bool ClientManager::endpoint_exists(const std::string& endpoint, bool source) const {
    if (source && endpoint == "tone/generator") return true;
    if (endpoint.compare(0, 9, "hardware/") == 0) {
        const std::string suffix = source ? "/capture" : "/playback";
        if (endpoint.size() < 10 + suffix.size() || endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
        return true; 
    }
    const std::string prefix = "client/";
    const std::string suffix = source ? "/capture" : "/playback";
    if (endpoint.size() <= prefix.size() + suffix.size() || endpoint.compare(0, prefix.size(), prefix) != 0 ||
        endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::string key = endpoint.substr(prefix.size(), endpoint.size() - prefix.size() - suffix.size());
    if (key.empty()) return false;
    for (auto& session : m_sessions)
        if (session->get_client_key() == key && session->active.load(std::memory_order_relaxed)) return true;
    return m_known_client_names.find(key) != m_known_client_names.end();
}

bool ClientManager::add_route(AudioRoute route, uint32_t& assignedId, std::string& error) {
    if (route.source_channel >= CM5_MAX_CHANNELS || route.destination_channel >= CM5_MAX_CHANNELS) {
        error = "channel must be between 0 and 7"; return false;
    }
    if (!std::isfinite(route.gain) || route.gain < 0.0f || route.gain > 8.0f) {
        error = "gain must be finite and between 0 and 8"; return false;
    }
    if (route.source_node == NODE_INVALID || route.destination_node == NODE_INVALID) {
        error = "source or destination endpoint is invalid or unregistered"; return false;
    }
    std::lock_guard<std::mutex> routeLock(m_route_lock);
    std::lock_guard<std::mutex> sessionLock(m_lock);
    auto routes = *std::atomic_load(&m_routes);
    for (const auto& existing : routes) {
        if (existing.source_node == route.source_node &&
            existing.source_channel == route.source_channel &&
            existing.destination_node == route.destination_node &&
            existing.destination_channel == route.destination_channel) {
            error = "This routing connection already exists";
            return false;
        }
    }
    route.id = assignedId = m_next_route_id.fetch_add(1);
    routes.push_back(std::move(route));
    std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

bool ClientManager::remove_route(uint32_t routeId) {
    std::lock_guard<std::mutex> lock(m_route_lock);
    auto routes = *std::atomic_load(&m_routes);
    const auto oldSize = routes.size();
    routes.erase(std::remove_if(routes.begin(), routes.end(), [routeId](const auto& route) { return route.id == routeId; }), routes.end());
    if (routes.size() == oldSize) return false;
    std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

bool ClientManager::update_route(uint32_t routeId, float gain, bool enabled) {
    if (!std::isfinite(gain) || gain < 0.0f || gain > 8.0f) return false;
    std::lock_guard<std::mutex> lock(m_route_lock);
    auto routes = *std::atomic_load(&m_routes);
    auto it = std::find_if(routes.begin(), routes.end(), [routeId](const auto& route) { return route.id == routeId; });
    if (it == routes.end()) return false;
    it->gain = gain;
    it->enabled = enabled;
    std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
    if (m_dspEngine) m_dspEngine->mark_graph_dirty();
    return true;
}

WebServer::WebServer(AudioMetrics& metrics, AudioControls& controls, ToneControls& tone,
                     ClientManager& clientMgr, DeviceManager& deviceMgr, DspEngine& dspEngine, int httpPort, int wsPort, bool plainText)
    : m_metrics(metrics), m_controls(controls), m_tone(tone), m_clientMgr(clientMgr), m_deviceMgr(deviceMgr), m_dspEngine(dspEngine),
      m_httpPort(httpPort), m_wsPort(wsPort), m_plainText(plainText) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    if (m_running.exchange(true)) return true;
    const std::string certPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_CERT"); return value && *value ? std::string(value) : "./certs/server.crt"; }();
    const std::string keyPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_KEY"); return value && *value ? std::string(value) : "./certs/server.key"; }();
    const std::string p12Path = [] { const char* value = std::getenv("CM5AUDIO_TLS_P12"); return value && *value ? std::string(value) : "./certs/server.p12"; }();
    const bool tlsEnabled = !m_plainText && std::ifstream(certPath).good() && std::ifstream(keyPath).good();

    m_httpThread = std::thread([this, certPath, keyPath, p12Path, tlsEnabled] {
        if (tlsEnabled) {
            auto ssl = std::make_unique<httplib::SSLServer>(certPath.c_str(), keyPath.c_str());
            if (!ssl->is_valid()) { std::cerr << "[HTTP] Invalid TLS certificate/key" << std::endl; m_running = false; return; }
            m_httpServer = std::move(ssl);
        } else {
            std::cerr << (m_plainText ? "[HTTP] Plain HTTP enabled" : "[HTTP] TLS certificate not found; serving plain HTTP") << std::endl;
            m_httpServer = std::make_unique<httplib::Server>();
        }
        m_httpServer->set_mount_point("/client", "./web_client");
        m_httpServer->Get("/server.crt", [certPath](const httplib::Request&, httplib::Response& response) {
            std::ifstream certificate(certPath, std::ios::in | std::ios::binary);
            if (!certificate) {
                response.status = 404;
                response.set_content("Certificate unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << certificate.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.crt");
            response.set_content(contents.str(), "application/x-signed-certificate");
        });
        m_httpServer->Get("/server.key", [keyPath](const httplib::Request&, httplib::Response& response) {
            std::ifstream privateKey(keyPath, std::ios::in | std::ios::binary);
            if (!privateKey) {
                response.status = 404;
                response.set_content("Private key unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << privateKey.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.key");
            response.set_content(contents.str(), "application/x-pem-private-key");
        });
        m_httpServer->Get("/server.p12", [p12Path](const httplib::Request&, httplib::Response& response) {
            std::ifstream bundle(p12Path, std::ios::in | std::ios::binary);
            if (!bundle) {
                response.status = 404;
                response.set_content("PKCS#12 bundle unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << bundle.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.p12");
            response.set_content(contents.str(), "application/x-pkcs12");
        });

        auto meters = [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json j;
            j["capture"] = nlohmann::json::object();
            for (auto const& [id, meters_array] : m_metrics.capture) {
                j["capture"][id] = build_meter_json(meters_array, CM5_MAX_CHANNELS);
            }
            j["playback"] = nlohmann::json::object();
            for (auto const& [id, meters_array] : m_metrics.playback) {
                j["playback"][id] = build_meter_json(meters_array, CM5_MAX_CHANNELS);
            }
            response.set_content(j.dump(2), "application/json");
        };
        m_httpServer->Get("/api/devices", [this](const httplib::Request&, httplib::Response& response) {
            m_deviceMgr.enumerate_devices();
            const auto devices = m_deviceMgr.get_available_devices();
            nlohmann::json j = nlohmann::json::array();
            for (const auto& d : devices) {
                nlohmann::json dev_obj;
                dev_obj["id"] = d.id.alsa;
                dev_obj["name"] = d.name;
                dev_obj["is_capture"] = d.is_capture;
                dev_obj["channels"] = d.channels;
                dev_obj["sample_rate"] = d.sample_rate;
                j.push_back(dev_obj);
            }
            response.set_content(j.dump(2), "application/json");
        });

        m_httpServer->Post("/api/devices/activate", [this](const httplib::Request& request, httplib::Response& response) {
            const std::string id = request.has_param("id") ? request.get_param_value("id") : "";
            if (id.empty()) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"id parameter is required\"}", "application/json");
                return;
            }
            if (m_deviceMgr.activate_device(id)) {
                m_clientMgr.update_activated_device(id, true);
                m_clientMgr.save_settings(m_controls, m_tone);
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } else {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"failed to activate device\"}", "application/json");
            }
        });

        m_httpServer->Post("/api/devices/deactivate", [this](const httplib::Request& request, httplib::Response& response) {
            const std::string id = request.has_param("id") ? request.get_param_value("id") : "";
            if (id.empty()) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"id parameter is required\"}", "application/json");
                return;
            }
            m_deviceMgr.deactivate_device(id);
            m_clientMgr.update_activated_device(id, false);
            m_clientMgr.save_settings(m_controls, m_tone);
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        m_httpServer->Get("/api/meters", meters);
        m_httpServer->Get("/api/metrics", meters);
        m_httpServer->Get("/api/raw", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json j;
            j["capture"] = nlohmann::json::object();
            for (auto const& [id, meters_array] : m_metrics.capture) {
                nlohmann::json arr = nlohmann::json::array();
                for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                    const auto& meter = meters_array[i];
                    arr.push_back({{"ch", i}, {"value", meter.raw_value.load(std::memory_order_relaxed)}, {"peak", meter.raw_peak.load(std::memory_order_relaxed)}});
                }
                j["capture"][id] = arr;
            }
            j["playback"] = nlohmann::json::object();
            for (auto const& [id, meters_array] : m_metrics.playback) {
                nlohmann::json arr = nlohmann::json::array();
                for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                    const auto& meter = meters_array[i];
                    arr.push_back({{"ch", i}, {"value", meter.raw_value.load(std::memory_order_relaxed)}, {"peak", meter.raw_peak.load(std::memory_order_relaxed)}});
                }
                j["playback"][id] = arr;
            }
            response.set_content(j.dump(2), "application/json");
        });

        m_httpServer->Get("/api/tone", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json j;
            j["enabled"] = m_tone.enabled.load();
            j["frequency_hz"] = m_tone.frequency_hz.load();
            j["amplitude"] = m_tone.amplitude.load();
            response.set_content(j.dump(2), "application/json");
        });
        m_httpServer->Post("/api/tone", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                if (request.has_param("enabled")) m_tone.enabled.store(query_bool(request, "enabled", false));
                if (request.has_param("frequency")) {
                    const float frequency = query_float(request, "frequency", -1.0f);
                    if (!std::isfinite(frequency) || frequency < 1.0f || frequency > 20000.0f) throw std::invalid_argument("frequency must be 1..20000 Hz");
                    m_tone.frequency_hz.store(frequency);
                }
                if (request.has_param("amplitude")) {
                    const float amplitude = query_float(request, "amplitude", -1.0f);
                    if (!std::isfinite(amplitude) || amplitude < 0.0f || amplitude > 1.0f) throw std::invalid_argument("amplitude must be 0..1");
                    m_tone.amplitude.store(amplitude);
                }
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(error.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Get("/api/dsp", [this](const httplib::Request&, httplib::Response& response) {
            auto* gainProc = m_dspEngine.get_gain_processor();
            auto* compProc = m_dspEngine.get_compressor();
            nlohmann::json j;
            j["gain"] = gainProc ? gainProc->get_gain() : 1.0f;
            j["threshold"] = compProc ? compProc->get_threshold() : 0.5f;
            j["ratio"] = compProc ? compProc->get_ratio() : 4.0f;
            response.set_content(j.dump(2), "application/json");
        });
        m_httpServer->Post("/api/dsp", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                auto* gainProc = m_dspEngine.get_gain_processor();
                auto* compProc = m_dspEngine.get_compressor();
                if (request.has_param("gain")) {
                    float g = query_float(request, "gain", -1.0f);
                    if (g < 0.0f || !std::isfinite(g)) throw std::invalid_argument("gain must be >= 0");
                    if (gainProc) gainProc->set_gain(g);
                }
                if (request.has_param("threshold")) {
                    float t = query_float(request, "threshold", -1.0f);
                    if (t < 0.0f || !std::isfinite(t)) throw std::invalid_argument("threshold must be >= 0");
                    if (compProc) compProc->set_threshold(t);
                }
                if (request.has_param("ratio")) {
                    float r = query_float(request, "ratio", -1.0f);
                    if (r < 1.0f || !std::isfinite(r)) throw std::invalid_argument("ratio must be >= 1");
                    if (compProc) compProc->set_ratio(r);
                }
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& e) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(e.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Get("/api/clients", [this](const httplib::Request&, httplib::Response& response) {
            const auto sessions = m_clientMgr.get_active_sessions();
            nlohmann::json j;
            j["clients"] = nlohmann::json::array();
            for (const auto& s : sessions) {
                nlohmann::json sj;
                sj["id"] = s->id;
                sj["client_id"] = s->get_client_key();
                sj["name"] = s->get_client_name();
                sj["ip"] = s->remote_ip;
                sj["input_channels"] = s->input_channels.load();
                sj["output_channels"] = s->output_channels.load();
                sj["input_meters"] = build_meter_json(s->input_metrics, s->input_channels.load());
                sj["output_meters"] = build_meter_json(s->output_metrics, s->output_channels.load());
                j["clients"].push_back(sj);
            }
            response.set_content(j.dump(2), "application/json");
        });

        m_httpServer->Get("/api/graph", [this](const httplib::Request&, httplib::Response& response) {
            nlohmann::json j;
            j["revision"] = 1;
            
            std::unordered_map<std::string, nlohmann::json> endpointsMap;

            const auto activeHw = m_deviceMgr.get_active_devices();
            for (const auto& dev : activeHw) {
                const std::string id = dev->get_original_id();
                const std::string name = dev->get_info().name;
                if (dev->get_info().is_capture) {
                    std::string epId = "hardware/" + id + "/capture";
                    nlohmann::json ep;
                    ep["id"] = epId; ep["direction"] = "source"; ep["channels"] = dev->get_info().channels; ep["name"] = name;
                    endpointsMap[epId] = ep;
                } else {
                    std::string epId = "hardware/" + id + "/playback";
                    nlohmann::json ep;
                    ep["id"] = epId; ep["direction"] = "destination"; ep["channels"] = dev->get_info().channels; ep["name"] = name;
                    endpointsMap[epId] = ep;
                }
            }
            
            nlohmann::json toneEp;
            toneEp["id"] = "tone/generator"; toneEp["direction"] = "source"; toneEp["channels"] = 8;
            endpointsMap["tone/generator"] = toneEp;
            
            const auto sessions = m_clientMgr.get_active_sessions();
            for (const auto& s : sessions) {
                std::string key = s->get_client_key();
                std::string name = s->get_client_name();
                
                std::string capId = "client/" + key + "/capture";
                nlohmann::json capEp;
                capEp["id"] = capId; capEp["name"] = name; capEp["connected"] = true; capEp["direction"] = "source"; capEp["channels"] = s->input_channels.load();
                endpointsMap[capId] = capEp;

                std::string playId = "client/" + key + "/playback";
                nlohmann::json playEp;
                playEp["id"] = playId; playEp["name"] = name; playEp["connected"] = true; playEp["direction"] = "destination"; playEp["channels"] = s->output_channels.load();
                endpointsMap[playId] = playEp;
            }
            for (const auto& known : m_clientMgr.known_client_names()) {
                const std::string capId = "client/" + known.first + "/capture";
                const std::string playId = "client/" + known.first + "/playback";
                if (endpointsMap.find(capId) == endpointsMap.end()) {
                    nlohmann::json capEp;
                    capEp["id"] = capId; capEp["name"] = known.second; capEp["connected"] = false; capEp["direction"] = "source"; capEp["channels"] = 1;
                    endpointsMap[capId] = capEp;
                }
                if (endpointsMap.find(playId) == endpointsMap.end()) {
                    nlohmann::json playEp;
                    playEp["id"] = playId; playEp["name"] = known.second; playEp["connected"] = false; playEp["direction"] = "destination"; playEp["channels"] = 2;
                    endpointsMap[playId] = playEp;
                }
            }

            nlohmann::json endpoints = nlohmann::json::array();
            for (auto const& [id, ep] : endpointsMap) endpoints.push_back(ep);
            j["endpoints"] = endpoints;

            nlohmann::json routes = nlohmann::json::array();
            const auto routeList = m_clientMgr.get_routes();
            for (const auto& r : routeList) {
                nlohmann::json rj;
                rj["id"] = r.id;
                rj["source"] = m_clientMgr.get_endpoint_from_id(r.source_node);
                rj["source_channel"] = r.source_channel;
                rj["destination"] = m_clientMgr.get_endpoint_from_id(r.destination_node);
                rj["destination_channel"] = r.destination_channel;
                rj["gain"] = r.gain;
                rj["enabled"] = r.enabled;
                routes.push_back(rj);
            }
            j["routes"] = routes;
            response.set_content(j.dump(2), "application/json");
        });

        m_httpServer->Post("/api/routes", [this](const httplib::Request& request, httplib::Response& response) {
            AudioRoute route;
            std::string src_ep = request.has_param("source") ? request.get_param_value("source") : "";
            std::string dst_ep = request.has_param("destination") ? request.get_param_value("destination") : "";
            route.source_node = m_clientMgr.get_node_id(src_ep);
            route.destination_node = m_clientMgr.get_node_id(dst_ep);
            route.source_channel = query_uint(request, "source_ch", 99);
            route.destination_channel = query_uint(request, "destination_ch", 99);
            route.gain = query_float(request, "gain", 1.0f);
            route.enabled = query_bool(request, "enabled", true);
            uint32_t id = 0; std::string error;
            if (!m_clientMgr.add_route(std::move(route), id, error)) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(error) + "\"}", "application/json");
                return;
            }
            response.set_content("{\"status\":\"ok\",\"id\":" + std::to_string(id) + "}", "application/json");
        });

        m_httpServer->Delete(R"(/api/routes/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t routeId = 0;
            if (request.matches.size() > 1) { try { routeId = std::stoul(request.matches[1]); } catch (...) {} }
            if (!m_clientMgr.remove_route(routeId)) { response.status = 404; response.set_content("{\"status\":\"error\"}", "application/json"); return; }
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        m_httpServer->Post("/api/routes/update", [this](const httplib::Request& request, httplib::Response& response) {
            const uint32_t id = query_uint(request, "id", 0);
            if (!id || !m_clientMgr.update_route(id, query_float(request, "gain", 1.0f), query_bool(request, "enabled", true))) {
                response.status = 400; response.set_content("{\"status\":\"error\"}", "application/json"); return;
            }
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        m_httpServer->Get(R"(/api/routes/(\d+)/dsp)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t routeId = 0;
            if (request.matches.size() > 1) { try { routeId = std::stoul(request.matches[1]); } catch (...) {} }
            if (routeId == 0) { response.status = 400; response.set_content("{\"status\":\"error\",\"message\":\"invalid route id\"}", "application/json"); return; }

            std::lock_guard<std::mutex> lock(g_dspCacheLock);
            if (g_dspSettingsCache.count(routeId)) {
                const auto& s = g_dspSettingsCache[routeId];
                nlohmann::json j;
                j["active"] = s.active;
                j["gain"] = s.gain;
                j["chain"] = s.chain;
                response.set_content(j.dump(2), "application/json");
            } else {
                response.set_content("{\"active\":false,\"gain\":1.0,\"chain\":[]}", "application/json");
            }
        });

        m_httpServer->Post(R"(/api/routes/(\d+)/dsp)", [this](const httplib::Request& request, httplib::Response& response) {
            uint32_t routeId = 0;
            if (request.matches.size() > 1) { try { routeId = std::stoul(request.matches[1]); } catch (...) {} }
            if (routeId == 0) { response.status = 400; response.set_content("{\"status\":\"error\",\"message\":\"invalid route id\"}", "application/json"); return; }

            try {
                auto body = nlohmann::json::parse(request.body);
                bool active = body.value("active", false);
                float gain = body.value("gain", 1.0f);
                
                std::vector<std::unique_ptr<AudioProcessor>> chain;
                std::vector<nlohmann::json> chainJson;

                if (body.contains("chain") && body["chain"].is_array()) {
                    for (const auto& proc : body["chain"]) {
                        std::string type = proc.value("type", "passthrough");
                        nlohmann::json params = proc.value("params", nlohmann::json::object());
                        chain.push_back(create_processor(type, params));
                        nlohmann::json pj;
                        pj["type"] = type; pj["params"] = params;
                        chainJson.push_back(pj);
                    }
                }

                m_dspEngine.set_route_dsp_active(routeId, active);
                m_dspEngine.set_route_gain(routeId, gain);
                m_dspEngine.update_route_chain(routeId, std::move(chain));

                {
                    std::lock_guard<std::mutex> lock(g_dspCacheLock);
                    g_dspSettingsCache[routeId] = { active, gain, chainJson };
                }

                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& e) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(e.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Post("/api/settings/save", [this](const httplib::Request&, httplib::Response& response) {
            if (!m_clientMgr.save_settings(m_controls, m_tone)) {
                response.status = 500;
                response.set_content("{\"status\":\"error\",\"message\":\"settings could not be saved\"}", "application/json");
                return;
            }
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        m_httpServer->Post("/api/control", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const std::string endpoint = request.has_param("endpoint") ? request.get_param_value("endpoint") : "";
                const std::string type = request.has_param("type") ? request.get_param_value("type") : "";
                const int channel = request.has_param("ch") ? std::stoi(request.get_param_value("ch")) : -1;
                if (endpoint.empty() || (type != "capture" && type != "playback") || channel < 0 || channel >= 8) throw std::invalid_argument("invalid parameters");
                auto& bank = (type == "playback") ? m_controls.playback[endpoint] : m_controls.capture[endpoint];
                if (request.has_param("gain")) {
                    const float gain = std::stof(request.get_param_value("gain"));
                    if (!std::isfinite(gain) || gain < 0.0f || gain > 8.0f) throw std::invalid_argument("invalid gain");
                    bank[channel].gain.store(gain);
                }
                if (request.has_param("mute")) bank[channel].mute.store(request.get_param_value("mute") == "1");
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(error.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Post("/api/reset_clips", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                if (request.has_param("all")) {
                    for (auto& [id, bank] : m_metrics.capture) {
                        for (auto& m : bank) { m.clipped.store(false); m.peak_hold_db.store(-60.0f); }
                    }
                    for (auto& [id, bank] : m_metrics.playback) {
                        for (auto& m : bank) { m.clipped.store(false); m.peak_hold_db.store(-60.0f); }
                    }
                }
                else {
                    const std::string endpoint = request.has_param("endpoint") ? request.get_param_value("endpoint") : "";
                    const std::string type = request.has_param("type") ? request.get_param_value("type") : "";
                    const int channel = request.has_param("ch") ? std::stoi(request.get_param_value("ch")) : -1;
                    if (endpoint.empty() || (type != "capture" && type != "playback") || channel < 0 || channel >= 8) throw std::invalid_argument("invalid parameters");
                    auto& bank = (type == "playback") ? m_metrics.playback[endpoint] : m_metrics.capture[endpoint];
                    bank[channel].clipped.store(false); bank[channel].peak_hold_db.store(-60.0f);
                }
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (const std::exception& error) {
                response.status = 400;
                response.set_content("{\"status\":\"error\",\"message\":\"" + json_escape(error.what()) + "\"}", "application/json");
            }
        });

        m_httpServer->Get("/favicon.ico", [](const httplib::Request&, httplib::Response& response) { response.set_content(read_favicon(), "image/x-icon"); });

        m_httpServer->Get("/", [](const httplib::Request&, httplib::Response& response) { response.set_content(read_dashboard(), "text/html"); });
        std::cout << "[HTTP] Dashboard listening on " << (tlsEnabled ? "https" : "http") << "://0.0.0.0:" << m_httpPort << std::endl;
        m_httpServer->listen("0.0.0.0", m_httpPort);
    });

    m_wsThread = std::thread([this, certPath, keyPath, p12Path, tlsEnabled] {
        m_wsServer = std::make_unique<ix::WebSocketServer>(m_wsPort, "0.0.0.0");
        if (m_plainText) std::cerr << "[WS] Plain WS enabled" << std::endl;
        if (tlsEnabled) {
            ix::SocketTLSOptions tls;
            tls.certFile = certPath; tls.keyFile = keyPath; tls.caFile = "NONE"; tls.tls = true;
            m_wsServer->setTLSOptions(tls);
        }
        static std::atomic<uint32_t> nextId{1};
        m_wsServer->setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> weakSocket, std::shared_ptr<ix::ConnectionState> state) {
            auto socket = weakSocket.lock();
            if (!socket) return;
            const uint32_t id = nextId.fetch_add(1);
            const auto session = m_clientMgr.create_session(id, state ? state->getRemoteIp() : "");
            session->start_sender(socket);
            
            nlohmann::json assigned;
            assigned["type"] = "assigned";
            assigned["connection_id"] = id;
            assigned["client_id"] = session->get_client_key();
            assigned["name"] = session->get_client_name();
            socket->sendText(assigned.dump());

            socket->setOnMessageCallback([this, session, id, socket](const ix::WebSocketMessagePtr& message) {
                if (message->type == ix::WebSocketMessageType::Message && message->binary && !message->str.empty()) {
                    session->incoming_rb.write(message->str.data(), message->str.size());
                } else if (message->type == ix::WebSocketMessageType::Message && !message->binary) {
                    std::string key, name;
                    unsigned input = session->input_channels.load(std::memory_order_relaxed);
                    unsigned output = session->output_channels.load(std::memory_order_relaxed);
                    if (parse_identity_message(message->str, key, name, input, output)) {
                        if (!m_clientMgr.claim_identity(session, key, name)) {
                            nlohmann::json conflict;
                            conflict["type"] = "identity_conflict";
                            conflict["client_id"] = key;
                            socket->sendText(conflict.dump());
                        } else {
                            session->input_channels.store(std::clamp(input, 1u, CM5_MAX_CHANNELS), std::memory_order_relaxed);
                            session->output_channels.store(std::clamp(output, 1u, CM5_MAX_CHANNELS), std::memory_order_relaxed);
                            nlohmann::json accepted;
                            accepted["type"] = "identity_accepted";
                            accepted["client_id"] = session->get_client_key();
                            accepted["name"] = session->get_client_name();
                            socket->sendText(accepted.dump());
                        }
                    }
                } else if (message->type == ix::WebSocketMessageType::Close) {
                    m_clientMgr.remove_session(id);
                }
            });
        });
        const auto result = m_wsServer->listen();
        if (!result.first) { std::cerr << "[WS] Failed to listen on port " << m_wsPort << ": " << result.second << std::endl; return; }
        m_wsServer->start();
        while (m_running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        m_wsServer->stop();
    });
    return true;
}

void WebServer::stop() {
    if (!m_running.exchange(false)) return;
    if (m_httpServer) m_httpServer->stop();
    if (m_wsServer) m_wsServer->stop();
    if (m_httpThread.joinable()) m_httpThread.join();
    if (m_wsThread.joinable()) m_wsThread.join();
}
