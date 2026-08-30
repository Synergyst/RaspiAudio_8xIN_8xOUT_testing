#include "web_server.h"
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
    const std::string needle = std::string("\"") + key + "\"";
    const auto keyPos = text.find(needle);
    if (keyPos == std::string::npos) return false;
    const auto colon = text.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return false;
    try { value = static_cast<unsigned>(std::stoul(text.substr(colon + 1))); return true; }
    catch (...) { return false; }
}

std::string read_dashboard() {
    std::ifstream file("./web_client/dashboard.html", std::ios::in | std::ios::binary);
    if (!file) return "<!doctype html><html><body><h1>Dashboard unavailable</h1></body></html>";
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

WebClientSession::~WebClientSession() { stop_sender(); }

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
    if (sender_thread.joinable()) sender_thread.join();
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

std::shared_ptr<const std::vector<AudioRoute>> ClientManager::route_snapshot() const {
    return std::atomic_load(&m_routes);
}

std::vector<AudioRoute> ClientManager::get_routes() const {
    const auto routes = route_snapshot();
    return *routes;
}

bool ClientManager::endpoint_exists(const std::string& endpoint, bool source) const {
    if (source && endpoint == "hardware/capture") return true;
    if (!source && endpoint == "hardware/playback") return true;
    const std::string prefix = "client/";
    const std::string suffix = source ? "/capture" : "/playback";
    if (endpoint.size() <= prefix.size() + suffix.size() || endpoint.compare(0, prefix.size(), prefix) != 0 ||
        endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    uint32_t id = 0;
    try { id = static_cast<uint32_t>(std::stoul(endpoint.substr(prefix.size(), endpoint.size() - prefix.size() - suffix.size()))); }
    catch (...) { return false; }
    for (const auto& session : m_sessions)
        if (session->id == id && session->active.load(std::memory_order_relaxed)) return true;
    return false;
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

WebServer::WebServer(AudioMetrics& metrics, AudioControls& controls, ClientManager& clientMgr, int httpPort, int wsPort)
    : m_metrics(metrics), m_controls(controls), m_clientMgr(clientMgr), m_httpPort(httpPort), m_wsPort(wsPort) {}

WebServer::~WebServer() { stop(); }

bool WebServer::start() {
    if (m_running.exchange(true)) return true;
    const std::string certPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_CERT"); return value && *value ? std::string(value) : "./certs/server.crt"; }();
    const std::string keyPath = [] { const char* value = std::getenv("CM5AUDIO_TLS_KEY"); return value && *value ? std::string(value) : "./certs/server.key"; }();
    const bool tlsEnabled = std::ifstream(certPath).good() && std::ifstream(keyPath).good();

    m_httpThread = std::thread([this, certPath, keyPath, tlsEnabled] {
        std::unique_ptr<httplib::Server> server;
        if (tlsEnabled) {
            auto ssl = std::make_unique<httplib::SSLServer>(certPath.c_str(), keyPath.c_str());
            if (!ssl->is_valid()) { std::cerr << "[HTTP] Invalid TLS certificate/key" << std::endl; m_running = false; return; }
            server = std::move(ssl);
        } else {
            std::cerr << "[HTTP] TLS certificate/key not found; serving plain HTTP" << std::endl;
            server = std::make_unique<httplib::Server>();
        }
        server->set_mount_point("/client", "./web_client");

        server->Get("/api/meters", [this](const httplib::Request&, httplib::Response& response) {
            std::ostringstream json;
            json << "{\"capture\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.capture[i];
                json << "{\"ch\":" << i << ",\"rms_db\":" << meter.rms_db.load()
                     << ",\"peak_db\":" << meter.peak_db.load() << ",\"peak_hold_db\":" << meter.peak_hold_db.load()
                     << ",\"clipped\":" << (meter.clipped.load() ? "true" : "false") << "}" << (i == 7 ? "" : ",");
            }
            json << "],\"playback\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.playback[i];
                json << "{\"ch\":" << i << ",\"rms_db\":" << meter.rms_db.load()
                     << ",\"peak_db\":" << meter.peak_db.load() << ",\"peak_hold_db\":" << meter.peak_hold_db.load()
                     << ",\"clipped\":" << (meter.clipped.load() ? "true" : "false") << "}" << (i == 7 ? "" : ",");
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        });
        server->Get("/api/metrics", [this](const httplib::Request& request, httplib::Response& response) {
            httplib::Request copy = request;
            (void)copy;
            std::ostringstream json;
            json << "{\"capture\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.capture[i];
                json << "{\"ch\":" << i << ",\"rms_db\":" << meter.rms_db.load() << ",\"peak_db\":" << meter.peak_db.load()
                     << ",\"peak_hold_db\":" << meter.peak_hold_db.load() << ",\"clipped\":" << (meter.clipped.load() ? "true" : "false") << "}" << (i == 7 ? "" : ",");
            }
            json << "],\"playback\":[";
            for (unsigned i = 0; i < CM5_MAX_CHANNELS; ++i) {
                const auto& meter = m_metrics.playback[i];
                json << "{\"ch\":" << i << ",\"rms_db\":" << meter.rms_db.load() << ",\"peak_db\":" << meter.peak_db.load()
                     << ",\"peak_hold_db\":" << meter.peak_hold_db.load() << ",\"clipped\":" << (meter.clipped.load() ? "true" : "false") << "}" << (i == 7 ? "" : ",");
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        });

        server->Get("/api/clients", [this](const httplib::Request&, httplib::Response& response) {
            const auto sessions = m_clientMgr.get_active_sessions();
            std::ostringstream json;
            json << "{\"clients\":[";
            for (size_t i = 0; i < sessions.size(); ++i) {
                const auto& s = sessions[i];
                json << "{\"id\":" << s->id << ",\"ip\":\"" << json_escape(s->remote_ip)
                     << "\",\"input_channels\":" << s->input_channels.load() << ",\"output_channels\":" << s->output_channels.load() << "}";
                if (i + 1 != sessions.size()) json << ',';
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        });

        server->Get("/api/graph", [this](const httplib::Request&, httplib::Response& response) {
            const auto sessions = m_clientMgr.get_active_sessions();
            const auto routes = m_clientMgr.get_routes();
            std::ostringstream json;
            json << "{\"revision\":1,\"endpoints\":[{\"id\":\"hardware/capture\",\"direction\":\"source\",\"channels\":8},{\"id\":\"hardware/playback\",\"direction\":\"destination\",\"channels\":8}";
            for (const auto& s : sessions) {
                json << ",{\"id\":\"client/" << s->id << "/capture\",\"direction\":\"source\",\"channels\":" << s->input_channels.load() << "}"
                     << ",{\"id\":\"client/" << s->id << "/playback\",\"direction\":\"destination\",\"channels\":" << s->output_channels.load() << "}";
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
            socket->sendText("{\"type\":\"assigned\",\"client_id\":" + std::to_string(id) + "}");
            socket->setOnMessageCallback([this, session, id](const ix::WebSocketMessagePtr& message) {
                if (message->type == ix::WebSocketMessageType::Message && message->binary && !message->str.empty()) {
                    session->incoming_rb.write(message->str.data(), message->str.size());
                } else if (message->type == ix::WebSocketMessageType::Message && !message->binary) {
                    unsigned input = 0, output = 0;
                    if (read_json_uint(message->str, "input_channels", input)) session->input_channels.store(std::clamp(input, 1u, CM5_MAX_CHANNELS));
                    if (read_json_uint(message->str, "output_channels", output)) session->output_channels.store(std::clamp(output, 1u, CM5_MAX_CHANNELS));
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
