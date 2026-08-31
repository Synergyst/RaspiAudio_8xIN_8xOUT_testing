#include "web_server.h"
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

namespace {

std::string json_escape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char ch : value) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += ch; }
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

bool read_json_uint(const std::string& text, const char* key, unsigned& value) {
    try {
        const auto object = nlohmann::json::parse(text);
        if (!object.contains(key) || !object[key].is_number_unsigned()) return false;
        value = object[key].get<unsigned>();
        return true;
    } catch (...) { return false; }
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

void append_meter_json(std::ostringstream& json,
                       const std::array<ChannelMeter, CM5_MAX_CHANNELS>& meters,
                       unsigned channels) {
    channels = std::min(channels, CM5_MAX_CHANNELS);
    json << '[';
    for (unsigned i = 0; i < channels; ++i) {
        const auto& meter = meters[i];
        json << "{\"ch\":" << i
             << ",\"raw_value\":" << meter.raw_value.load(std::memory_order_relaxed)
             << ",\"raw_peak\":" << meter.raw_peak.load(std::memory_order_relaxed)
             << ",\"rms_db\":" << meter.rms_db.load(std::memory_order_relaxed)
             << ",\"peak_db\":" << meter.peak_db.load(std::memory_order_relaxed)
             << ",\"peak_hold_db\":" << meter.peak_hold_db.load(std::memory_order_relaxed)
             << ",\"clipped\":" << (meter.clipped.load(std::memory_order_relaxed) ? "true" : "false")
             << "}" << (i + 1 == channels ? "" : ",");
    }
    json << ']';
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
    std::vector<AudioRoute> defaults;
    for (unsigned channel = 0; channel < CM5_MAX_CHANNELS; ++channel) {
        defaults.push_back(AudioRoute{
            m_next_route_id.fetch_add(1), "hardware/capture", channel,
            "hardware/playback", channel, 1.0f, true
        });
    }
    m_routes = std::make_shared<const std::vector<AudioRoute>>(std::move(defaults));
}

std::shared_ptr<WebClientSession> ClientManager::create_session(uint32_t id, const std::string& remoteIp) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto session = std::make_shared<WebClientSession>();
    session->id = id;
    session->remote_ip = remoteIp;
    session->set_identity("connection-" + std::to_string(id), "Client " + std::to_string(id));
    session->incoming_rb.init(48000 * CM5_MAX_CHANNELS * sizeof(float));
    session->outgoing_rb.init(48000 * CM5_MAX_CHANNELS * sizeof(float));
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
        m_sessions.erase(it);
    }
    removed->stop_sender();
}

std::vector<std::shared_ptr<WebClientSession>> ClientManager::get_active_sessions() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::shared_ptr<WebClientSession>> active;
    for (const auto& session : m_sessions)
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
            tone.amplitude.store(std::clamp(value.value("amplitude", 0.2f), 0.0f, 1.0f), std::memory_order_relaxed);
        }
        if (settings.contains("hardware") && settings["hardware"].is_object()) {
            const auto& hardware = settings["hardware"];
            for (const char* type : {"capture", "playback"}) {
                if (!hardware.contains(type) || !hardware[type].is_array()) continue;
                auto& bank = std::string(type) == "capture" ? controls.capture : controls.playback;
                for (unsigned i = 0; i < CM5_MAX_CHANNELS && i < hardware[type].size(); ++i) {
                    bank[i].gain.store(std::clamp(hardware[type][i].value("gain", 1.0f), 0.0f, 8.0f), std::memory_order_relaxed);
                    bank[i].mute.store(hardware[type][i].value("mute", false), std::memory_order_relaxed);
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
                    route.source_endpoint = value.value("source", "");
                    route.source_channel = value.value("source_channel", CM5_MAX_CHANNELS);
                    route.destination_endpoint = value.value("destination", "");
                    route.destination_channel = value.value("destination_channel", CM5_MAX_CHANNELS);
                    route.gain = value.value("gain", 1.0f);
                    route.enabled = value.value("enabled", true);
                    if (!route.id || route.source_endpoint.empty() || route.destination_endpoint.empty() ||
                        route.source_channel >= CM5_MAX_CHANNELS || route.destination_channel >= CM5_MAX_CHANNELS ||
                        !std::isfinite(route.gain) || route.gain < 0.0f || route.gain > 8.0f) continue;
                    routes.push_back(route);
                    nextId = std::max(nextId, route.id + 1);
                }
            }
            std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
            m_next_route_id.store(nextId, std::memory_order_relaxed);
            m_known_client_names.clear();
            if (settings.contains("clients") && settings["clients"].is_object()) {
                for (auto it = settings["clients"].begin(); it != settings["clients"].end(); ++it)
                    if (it.value().is_string()) m_known_client_names[it.key()] = it.value().get<std::string>();
            }
        }
        return true;
    } catch (...) {
        return false;
    }
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
    for (const auto* type : {"capture", "playback"}) {
        const auto& bank = std::string(type) == "capture" ? controls.capture : controls.playback;
        settings["hardware"][type] = nlohmann::json::array();
        for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i)
            settings["hardware"][type].push_back({
                {"gain", bank[i].gain.load(std::memory_order_relaxed)},
                {"mute", bank[i].mute.load(std::memory_order_relaxed)}
            });
    }
    {
        std::lock_guard<std::mutex> routeLock(m_route_lock);
        std::lock_guard<std::mutex> sessionLock(m_lock);
        settings["routes"] = nlohmann::json::array();
        for (const auto& route : *std::atomic_load(&m_routes)) {
            settings["routes"].push_back({
                {"id", route.id}, {"source", route.source_endpoint}, {"source_channel", route.source_channel},
                {"destination", route.destination_endpoint}, {"destination_channel", route.destination_channel},
                {"gain", route.gain}, {"enabled", route.enabled}
            });
        }
        settings["clients"] = nlohmann::json::object();
        for (const auto& client : m_known_client_names) settings["clients"][client.first] = client.second;
        for (const auto& session : m_sessions) {
            const std::string key = session->get_client_key();
            if (!key.empty()) settings["clients"][key] = session->get_client_name();
        }
    }
    try {
        std::ofstream file(temporaryPath, std::ios::trunc);
        if (!file) return false;
        file << settings.dump(2) << '\n';
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
    if (source && (endpoint == "hardware/capture" || endpoint == "tone/generator")) return true;
    if (!source && endpoint == "hardware/playback") return true;
    const std::string prefix = "client/";
    const std::string suffix = source ? "/capture" : "/playback";
    if (endpoint.size() <= prefix.size() + suffix.size() || endpoint.compare(0, prefix.size(), prefix) != 0 ||
        endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::string key = endpoint.substr(prefix.size(), endpoint.size() - prefix.size() - suffix.size());
    if (key.empty()) return false;
    for (const auto& session : m_sessions)
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
    std::lock_guard<std::mutex> routeLock(m_route_lock);
    std::lock_guard<std::mutex> sessionLock(m_lock);
    if (!endpoint_exists(route.source_endpoint, true) || !endpoint_exists(route.destination_endpoint, false)) {
        error = "source or destination endpoint is unavailable"; return false;
    }
    auto routes = *std::atomic_load(&m_routes);
    route.id = assignedId = m_next_route_id.fetch_add(1);
    routes.push_back(std::move(route));
    std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
    return true;
}

bool ClientManager::remove_route(uint32_t routeId) {
    std::lock_guard<std::mutex> lock(m_route_lock);
    auto routes = *std::atomic_load(&m_routes);
    const auto oldSize = routes.size();
    routes.erase(std::remove_if(routes.begin(), routes.end(), [routeId](const auto& route) { return route.id == routeId; }), routes.end());
    if (routes.size() == oldSize) return false;
    std::atomic_store(&m_routes, std::make_shared<const std::vector<AudioRoute>>(std::move(routes)));
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
    return true;
}

WebServer::WebServer(AudioMetrics& metrics, AudioControls& controls, ToneControls& tone,
                     ClientManager& clientMgr, int httpPort, int wsPort, bool plainText)
    : m_metrics(metrics), m_controls(controls), m_tone(tone), m_clientMgr(clientMgr),
      m_httpPort(httpPort), m_wsPort(wsPort), m_plainText(plainText) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    if (m_running.exchange(true)) return true;
    const std::string certPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_CERT"); return value && *value ? std::string(value) : "./certs/server.crt"; }();
    const std::string keyPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_KEY"); return value && *value ? std::string(value) : "./certs/server.key"; }();
    const std::string p12Path = [] { const char* value = std::getenv("CM5AUDIO_TLS_P12"); return value && *value ? std::string(value) : "./certs/server.p12"; }();
    const bool tlsEnabled = !m_plainText && std::ifstream(certPath).good() && std::ifstream(keyPath).good();

    m_httpThread = std::thread([this, certPath, keyPath, p12Path, tlsEnabled] {
        std::unique_ptr<httplib::Server> server;
        if (tlsEnabled) {
            auto ssl = std::make_unique<httplib::SSLServer>(certPath.c_str(), keyPath.c_str());
            if (!ssl->is_valid()) { std::cerr << "[HTTP] Invalid TLS certificate/key" << std::endl; m_running = false; return; }
            server = std::move(ssl);
        } else {
            std::cerr << (m_plainText ? "[HTTP] Plain HTTP enabled" : "[HTTP] TLS certificate/key not found; serving plain HTTP") << std::endl;
            server = std::make_unique<httplib::Server>();
        }
        server->set_mount_point("/client", "./web_client");
        server->Get("/server.crt", [certPath](const httplib::Request&, httplib::Response& response) {
            std::ifstream certificate(certPath, std::ios::in | std::ios::binary);
            if (!certificate) {
                response.status = 404;
                response.set_content("Certificate unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << certificate.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.crt");
            response.set_content(contents.str(), "application/x-x509-ca-cert");
        });
        server->Get("/server.key", [keyPath](const httplib::Request&, httplib::Response& response) {
            std::ifstream privateKey(keyPath, std::ios::in | std::ios::binary);
            if (!privateKey) {
                response.status = 404;
                response.set_content("Private key unavailable", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << privateKey.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.key");
            response.set_content(contents.str(), "application/x-pem-file");
        });
        server->Get("/server.p12", [p12Path](const httplib::Request&, httplib::Response& response) {
            std::ifstream bundle(p12Path, std::ios::in | std::ios::binary);
            if (!bundle) {
                response.status = 404;
                response.set_content("PKCS#12 bundle unavailable; run setup.sh first", "text/plain; charset=UTF-8");
                return;
            }
            std::ostringstream contents;
            contents << bundle.rdbuf();
            response.set_header("Content-Disposition", "attachment; filename=server.p12");
            response.set_content(contents.str(), "application/x-pkcs12");
        });

        auto meters = [this](const httplib::Request&, httplib::Response& response) {
            std::ostringstream json;
            json << "{\"capture\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.capture[i];
                json << "{\"ch\":" << i
                     << ",\"raw_value\":" << meter.raw_value.load(std::memory_order_relaxed)
                     << ",\"raw_peak\":" << meter.raw_peak.load(std::memory_order_relaxed)
                     << ",\"rms_db\":" << meter.rms_db.load(std::memory_order_relaxed)
                     << ",\"peak_db\":" << meter.peak_db.load(std::memory_order_relaxed)
                     << ",\"peak_hold_db\":" << meter.peak_hold_db.load(std::memory_order_relaxed)
                     << ",\"clipped\":" << (meter.clipped.load(std::memory_order_relaxed) ? "true" : "false")
                     << "}" << (i + 1 == CM5_MAX_CHANNELS ? "" : ",");
            }
            json << "],\"playback\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.playback[i];
                json << "{\"ch\":" << i
                     << ",\"raw_value\":" << meter.raw_value.load(std::memory_order_relaxed)
                     << ",\"raw_peak\":" << meter.raw_peak.load(std::memory_order_relaxed)
                     << ",\"rms_db\":" << meter.rms_db.load(std::memory_order_relaxed)
                     << ",\"peak_db\":" << meter.peak_db.load(std::memory_order_relaxed)
                     << ",\"peak_hold_db\":" << meter.peak_hold_db.load(std::memory_order_relaxed)
                     << ",\"clipped\":" << (meter.clipped.load(std::memory_order_relaxed) ? "true" : "false")
                     << "}" << (i + 1 == CM5_MAX_CHANNELS ? "" : ",");
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        };
        server->Get("/api/meters", meters);
        server->Get("/api/metrics", meters);
        server->Get("/api/raw", [this](const httplib::Request&, httplib::Response& response) {
            std::ostringstream json;
            json << "{\"capture\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.capture[i];
                json << "{\"ch\":" << i
                     << ",\"value\":" << meter.raw_value.load(std::memory_order_relaxed)
                     << ",\"peak\":" << meter.raw_peak.load(std::memory_order_relaxed)
                     << "}" << (i + 1 == CM5_MAX_CHANNELS ? "" : ",");
            }
            json << "],\"playback\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.playback[i];
                json << "{\"ch\":" << i
                     << ",\"value\":" << meter.raw_value.load(std::memory_order_relaxed)
                     << ",\"peak\":" << meter.raw_peak.load(std::memory_order_relaxed)
                     << "}" << (i + 1 == CM5_MAX_CHANNELS ? "" : ",");
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        });

        server->Get("/api/tone", [this](const httplib::Request&, httplib::Response& response) {
            std::ostringstream json;
            json << "{\"enabled\":" << (m_tone.enabled.load() ? "true" : "false")
                 << ",\"frequency_hz\":" << m_tone.frequency_hz.load()
                 << ",\"amplitude\":" << m_tone.amplitude.load() << '}';
            response.set_content(json.str(), "application/json");
        });
        server->Post("/api/tone", [this](const httplib::Request& request, httplib::Response& response) {
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

        server->Get("/api/clients", [this](const httplib::Request&, httplib::Response& response) {
            const auto sessions = m_clientMgr.get_active_sessions();
            std::ostringstream json;
            json << "{\"clients\":[";
            for (size_t i = 0; i < sessions.size(); ++i) {
                const auto& s = sessions[i];
                json << "{\"id\":" << s->id << ",\"client_id\":\"" << json_escape(s->get_client_key())
                     << "\",\"name\":\"" << json_escape(s->get_client_name())
                     << "\",\"ip\":\"" << json_escape(s->remote_ip)
                     << "\",\"input_channels\":" << s->input_channels.load()
                     << ",\"output_channels\":" << s->output_channels.load()
                     << ",\"input_meters\":";
                append_meter_json(json, s->input_metrics, s->input_channels.load());
                json << ",\"output_meters\":";
                append_meter_json(json, s->output_metrics, s->output_channels.load());
                json << "}" << (i + 1 == sessions.size() ? "" : ",");
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        });

        server->Get("/api/graph", [this](const httplib::Request&, httplib::Response& response) {
            const auto sessions = m_clientMgr.get_active_sessions();
            const auto routes = m_clientMgr.get_routes();
            std::ostringstream json;
            json << "{\"revision\":1,\"endpoints\":[{\"id\":\"hardware/capture\",\"direction\":\"source\",\"channels\":8},{\"id\":\"tone/generator\",\"direction\":\"source\",\"channels\":8},{\"id\":\"hardware/playback\",\"direction\":\"destination\",\"channels\":8}";
            for (const auto& s : sessions) {
                json << ",{\"id\":\"client/" << json_escape(s->get_client_key()) << "/capture\",\"name\":\""
                     << json_escape(s->get_client_name()) << "\",\"connected\":true,\"direction\":\"source\",\"channels\":" << s->input_channels.load() << "}"
                     << ",{\"id\":\"client/" << json_escape(s->get_client_key()) << "/playback\",\"name\":\""
                     << json_escape(s->get_client_name()) << "\",\"connected\":true,\"direction\":\"destination\",\"channels\":" << s->output_channels.load() << "}";
            }
            for (const auto& known : m_clientMgr.known_client_names()) {
                bool active = false;
                for (const auto& s : sessions) active = active || s->get_client_key() == known.first;
                if (active) continue;
                json << ",{\"id\":\"client/" << json_escape(known.first) << "/capture\",\"name\":\""
                     << json_escape(known.second) << "\",\"connected\":false,\"direction\":\"source\",\"channels\":1}"
                     << ",{\"id\":\"client/" << json_escape(known.first) << "/playback\",\"name\":\""
                     << json_escape(known.second) << "\",\"connected\":false,\"direction\":\"destination\",\"channels\":2}";
            }
            json << "],\"routes\":[";
            for (size_t i = 0; i < routes.size(); ++i) {
                const auto& r = routes[i];
                json << "{\"id\":" << r.id << ",\"source\":\"" << json_escape(r.source_endpoint) << "\",\"source_channel\":" << r.source_channel
                     << ",\"destination\":\"" << json_escape(r.destination_endpoint) << "\",\"destination_channel\":" << r.destination_channel
                     << ",\"gain\":" << r.gain << ",\"enabled\":" << (r.enabled ? "true" : "false") << "}";
                if (i + 1 != routes.size()) json << ',';
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        });

        server->Post("/api/routes", [this](const httplib::Request& request, httplib::Response& response) {
            AudioRoute route;
            route.source_endpoint = request.has_param("source") ? request.get_param_value("source") : "";
            route.destination_endpoint = request.has_param("destination") ? request.get_param_value("destination") : "";
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
        server->Delete(R"(/api/routes/(\d+))", [this](const httplib::Request& request, httplib::Response& response) {
            const uint32_t id = query_uint(request, "id", 0);
            uint32_t routeId = id;
            if (request.matches.size() > 1) { try { routeId = static_cast<uint32_t>(std::stoul(request.matches[1])); } catch (...) {} }
            if (!m_clientMgr.remove_route(routeId)) { response.status = 404; response.set_content("{\"status\":\"error\"}", "application/json"); return; }
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });
        server->Post("/api/routes/update", [this](const httplib::Request& request, httplib::Response& response) {
            const uint32_t id = query_uint(request, "id", 0);
            if (!id || !m_clientMgr.update_route(id, query_float(request, "gain", 1.0f), query_bool(request, "enabled", true))) {
                response.status = 400; response.set_content("{\"status\":\"error\"}", "application/json"); return;
            }
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        server->Post("/api/settings/save", [this](const httplib::Request&, httplib::Response& response) {
            if (!m_clientMgr.save_settings(m_controls, m_tone)) {
                response.status = 500;
                response.set_content("{\"status\":\"error\",\"message\":\"settings could not be saved\"}", "application/json");
                return;
            }
            response.set_content("{\"status\":\"ok\"}", "application/json");
        });

        server->Post("/api/control", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const std::string type = request.has_param("type") ? request.get_param_value("type") : "";
                const int channel = request.has_param("ch") ? std::stoi(request.get_param_value("ch")) : -1;
                if ((type != "capture" && type != "playback") || channel < 0 || channel >= 8) throw std::invalid_argument("invalid channel");
                auto& control = type == "playback" ? m_controls.playback[channel] : m_controls.capture[channel];
                if (request.has_param("gain")) {
                    const float gain = std::stof(request.get_param_value("gain"));
                    if (!std::isfinite(gain) || gain < 0.0f || gain > 8.0f) throw std::invalid_argument("invalid gain");
                    control.gain.store(gain);
                }
                if (request.has_param("mute")) control.mute.store(request.get_param_value("mute") == "1");
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (...) { response.status = 400; response.set_content("{\"status\":\"error\"}", "application/json"); }
        });
        server->Post("/api/reset_clips", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                if (request.has_param("all")) for (unsigned i = 0; i < 8; ++i) {
                    m_metrics.capture[i].clipped.store(false); m_metrics.capture[i].peak_hold_db.store(-60.0f);
                    m_metrics.playback[i].clipped.store(false); m_metrics.playback[i].peak_hold_db.store(-60.0f);
                }
                else {
                    const std::string type = request.has_param("type") ? request.get_param_value("type") : "";
                    const int channel = request.has_param("ch") ? std::stoi(request.get_param_value("ch")) : -1;
                    if ((type != "capture" && type != "playback") || channel < 0 || channel >= 8) throw std::invalid_argument("invalid channel");
                    auto& meter = type == "playback" ? m_metrics.playback[channel] : m_metrics.capture[channel];
                    meter.clipped.store(false); meter.peak_hold_db.store(-60.0f);
                }
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (...) { response.status = 400; response.set_content("{\"status\":\"error\"}", "application/json"); }
        });
        server->Get("/", [](const httplib::Request&, httplib::Response& response) { response.set_content(read_dashboard(), "text/html; charset=UTF-8"); });
        std::cout << "[HTTP] Dashboard listening on " << (tlsEnabled ? "https" : "http") << "://0.0.0.0:" << m_httpPort << std::endl;
        server->listen("0.0.0.0", m_httpPort);
    });

    m_wsThread = std::thread([this, certPath, keyPath, tlsEnabled] {
        ix::WebSocketServer server(m_wsPort, "0.0.0.0");
        if (m_plainText) std::cerr << "[WS] Plain WS enabled" << std::endl;
        if (tlsEnabled) {
            ix::SocketTLSOptions tls;
            tls.certFile = certPath; tls.keyFile = keyPath; tls.caFile = "NONE"; tls.tls = true;
            server.setTLSOptions(tls);
        }
        static std::atomic<uint32_t> nextId{1};
        server.setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> weakSocket, std::shared_ptr<ix::ConnectionState> state) {
            auto socket = weakSocket.lock();
            if (!socket) return;
            const uint32_t id = nextId.fetch_add(1);
            const auto session = m_clientMgr.create_session(id, state ? state->getRemoteIp() : "");
            session->start_sender(socket);
            socket->sendText(nlohmann::json{{"type", "assigned"}, {"connection_id", id},
                                            {"client_id", session->get_client_key()},
                                            {"name", session->get_client_name()}}.dump());
            socket->setOnMessageCallback([this, session, id, socket](const ix::WebSocketMessagePtr& message) {
                if (message->type == ix::WebSocketMessageType::Message && message->binary && !message->str.empty()) {
                    session->incoming_rb.write(message->str.data(), message->str.size());
                } else if (message->type == ix::WebSocketMessageType::Message && !message->binary) {
                    std::string key, name;
                    unsigned input = session->input_channels.load(std::memory_order_relaxed);
                    unsigned output = session->output_channels.load(std::memory_order_relaxed);
                    if (parse_identity_message(message->str, key, name, input, output)) {
                        if (!m_clientMgr.claim_identity(session, key, name)) {
                            socket->sendText(nlohmann::json{{"type", "identity_conflict"}, {"client_id", key}}.dump());
                        } else {
                            session->input_channels.store(std::clamp(input, 1u, CM5_MAX_CHANNELS), std::memory_order_relaxed);
                            session->output_channels.store(std::clamp(output, 1u, CM5_MAX_CHANNELS), std::memory_order_relaxed);
                            socket->sendText(nlohmann::json{{"type", "identity_accepted"},
                                                            {"client_id", session->get_client_key()},
                                                            {"name", session->get_client_name()}}.dump());
                        }
                    }
                } else if (message->type == ix::WebSocketMessageType::Close) {
                    m_clientMgr.remove_session(id);
                }
            });
        });
        const auto result = server.listen();
        if (!result.first) { std::cerr << "[WS] Failed to listen on port " << m_wsPort << ": " << result.second << std::endl; return; }
        server.start();
        while (m_running.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();
    });
    return true;
}

void WebServer::stop() {
    if (!m_running.exchange(false)) return;
    if (m_httpThread.joinable()) m_httpThread.detach();
    if (m_wsThread.joinable()) m_wsThread.join();
}
