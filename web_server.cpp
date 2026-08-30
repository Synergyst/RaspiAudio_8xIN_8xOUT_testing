#include "web_server.h"
#include "httplib.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

std::shared_ptr<WebClientSession> ClientManager::create_session(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto session = std::make_shared<WebClientSession>();
    session->id = id;
    session->incoming_rb.init(48000 * sizeof(float));
    session->outgoing_rb.init(48000 * 2 * sizeof(float));
    session->active.store(true);
    m_sessions.push_back(session);
    return session;
}

void ClientManager::remove_session(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = std::remove_if(m_sessions.begin(), m_sessions.end(),
        [id](const std::shared_ptr<WebClientSession>& session) {
            if (session->id != id) return false;
            session->active.store(false);
            session->incoming_rb.uninit();
            session->outgoing_rb.uninit();
            return true;
        });
    m_sessions.erase(it, m_sessions.end());
}

std::vector<std::shared_ptr<WebClientSession>> ClientManager::get_active_sessions() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::shared_ptr<WebClientSession>> active;
    for (const auto& session : m_sessions) {
        if (session->active.load()) active.push_back(session);
    }
    return active;
}

WebServer::WebServer(AudioMetrics& metrics, AudioControls& controls,
                     ClientManager& clientMgr, int httpPort, int wsPort)
    : m_metrics(metrics), m_controls(controls), m_clientMgr(clientMgr),
      m_httpPort(httpPort), m_wsPort(wsPort) {}

WebServer::~WebServer() { stop(); }

static std::string read_dashboard() {
    std::ifstream file("./web_client/dashboard.html", std::ios::in | std::ios::binary);
    if (!file) return "<!doctype html><html><body><h1>Dashboard unavailable</h1></body></html>";
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

bool WebServer::start() {
    if (m_running.exchange(true)) return true;

    m_httpThread = std::thread([this]() {
        httplib::Server server;
        server.set_mount_point("/client", "./web_client");

        auto meters = [this](const httplib::Request&, httplib::Response& response) {
            std::ostringstream json;
            json << "{\"capture\":[";
            for (size_t i = 0; i < 8; ++i) {
                const auto& meter = m_metrics.capture[i];
                json << "{\"ch\":" << i
                     << ",\"rms_db\":" << meter.rms_db.load(std::memory_order_relaxed)
                     << ",\"peak_db\":" << meter.peak_db.load(std::memory_order_relaxed)
                     << ",\"peak_hold_db\":" << meter.peak_hold_db.load(std::memory_order_relaxed)
                     << ",\"clipped\":" << (meter.clipped.load(std::memory_order_relaxed) ? "true" : "false")
                     << "}" << (i == 7 ? "" : ",");
            }
            json << "],\"playback\":[";
            for (size_t i = 0; i < 8; ++i) {
                const auto& meter = m_metrics.playback[i];
                json << "{\"ch\":" << i
                     << ",\"rms_db\":" << meter.rms_db.load(std::memory_order_relaxed)
                     << ",\"peak_db\":" << meter.peak_db.load(std::memory_order_relaxed)
                     << ",\"peak_hold_db\":" << meter.peak_hold_db.load(std::memory_order_relaxed)
                     << ",\"clipped\":" << (meter.clipped.load(std::memory_order_relaxed) ? "true" : "false")
                     << "}" << (i == 7 ? "" : ",");
            }
            json << "]}";
            response.set_content(json.str(), "application/json");
        };
        server.Get("/api/meters", meters);
        server.Get("/api/metrics", meters);

        server.Post("/api/control", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                const std::string type = request.has_param("type") ? request.get_param_value("type") : "";
                const int channel = request.has_param("ch") ? std::stoi(request.get_param_value("ch")) : -1;
                if ((type != "capture" && type != "playback") || channel < 0 || channel >= 8) {
                    response.status = 400;
                    response.set_content("{\"status\":\"error\"}", "application/json");
                    return;
                }
                auto& control = type == "playback" ? m_controls.playback[channel] : m_controls.capture[channel];
                if (request.has_param("gain")) control.gain.store(std::stof(request.get_param_value("gain")), std::memory_order_relaxed);
                if (request.has_param("mute")) control.mute.store(request.get_param_value("mute") == "1", std::memory_order_relaxed);
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (...) {
                response.status = 400;
                response.set_content("{\"status\":\"error\"}", "application/json");
            }
        });

        server.Post("/api/reset_clips", [this](const httplib::Request& request, httplib::Response& response) {
            try {
                if (request.has_param("all")) {
                    for (int i = 0; i < 8; ++i) {
                        m_metrics.capture[i].clipped.store(false, std::memory_order_relaxed);
                        m_metrics.capture[i].peak_hold_db.store(-60.0f, std::memory_order_relaxed);
                        m_metrics.playback[i].clipped.store(false, std::memory_order_relaxed);
                        m_metrics.playback[i].peak_hold_db.store(-60.0f, std::memory_order_relaxed);
                    }
                } else {
                    const std::string type = request.has_param("type") ? request.get_param_value("type") : "";
                    const int channel = request.has_param("ch") ? std::stoi(request.get_param_value("ch")) : -1;
                    if ((type != "capture" && type != "playback") || channel < 0 || channel >= 8) {
                        response.status = 400;
                        response.set_content("{\"status\":\"error\"}", "application/json");
                        return;
                    }
                    auto& meter = type == "playback" ? m_metrics.playback[channel] : m_metrics.capture[channel];
                    meter.clipped.store(false, std::memory_order_relaxed);
                    meter.peak_hold_db.store(-60.0f, std::memory_order_relaxed);
                }
                response.set_content("{\"status\":\"ok\"}", "application/json");
            } catch (...) {
                response.status = 400;
                response.set_content("{\"status\":\"error\"}", "application/json");
            }
        });

        server.Get("/", [](const httplib::Request&, httplib::Response& response) {
            response.set_content(read_dashboard(), "text/html; charset=UTF-8");
        });

        std::cout << "[HTTP] Dashboard listening on http://0.0.0.0:" << m_httpPort << std::endl;
        server.listen("0.0.0.0", m_httpPort);
    });

    m_wsThread = std::thread([this]() {
        ix::WebSocketServer server(m_wsPort, "0.0.0.0");
        static std::atomic<uint32_t> next_id{1};
        server.setOnConnectionCallback([this](std::weak_ptr<ix::WebSocket> weakSocket,
                                               std::shared_ptr<ix::ConnectionState>) {
            auto socket = weakSocket.lock();
            if (!socket) return;
            const uint32_t id = next_id.fetch_add(1);
            auto session = m_clientMgr.create_session(id);
            socket->setOnMessageCallback([this, weakSocket, session, id](const ix::WebSocketMessagePtr& message) {
                auto client = weakSocket.lock();
                if (!client) return;
                if (message->type == ix::WebSocketMessageType::Message && message->binary && !message->str.empty()) {
                    session->incoming_rb.write(message->str.data(), message->str.size());
                    uint8_t output[4096];
                    const size_t bytes = session->outgoing_rb.read(output, sizeof(output));
                    if (bytes) client->sendBinary(std::string(reinterpret_cast<char*>(output), bytes));
                } else if (message->type == ix::WebSocketMessageType::Close) {
                    m_clientMgr.remove_session(id);
                }
            });
        });
        const auto result = server.listen();
        if (!result.first) {
            std::cerr << "[WS] Failed to listen on port " << m_wsPort << ": " << result.second << std::endl;
            return;
        }
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
