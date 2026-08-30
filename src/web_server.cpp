#include "web_server.h"
#include "httplib.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <iostream>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;

// ============================================================================
// ClientManager Implementation
// ============================================================================

std::shared_ptr<WebClientSession> ClientManager::create_session(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto session = std::make_shared<WebClientSession>();
    session->id = id;
    
    session->incoming_rb.init(48000 * 1 * sizeof(float)); 
    session->outgoing_rb.init(48000 * 2 * sizeof(float)); 
    session->active.store(true);

    m_sessions.push_back(session);
    return session;
}

void ClientManager::remove_session(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = std::remove_if(m_sessions.begin(), m_sessions.end(),
        [id](const std::shared_ptr<WebClientSession>& s) {
            if (s->id == id) {
                s->active.store(false);
                s->incoming_rb.uninit();
                s->outgoing_rb.uninit();
                return true;
            }
            return false;
        });
    m_sessions.erase(it, m_sessions.end());
}

std::vector<std::shared_ptr<WebClientSession>> ClientManager::get_active_sessions() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::shared_ptr<WebClientSession>> active;
    for (const auto& s : m_sessions) {
        if (s->active.load()) {
            active.push_back(s);
        }
    }
    return active;
}

// ============================================================================
// WebServer Implementation
// ============================================================================

WebServer::WebServer(AudioMetrics& metrics, AudioControls& controls, ClientManager& clientMgr, int httpPort, int wsPort)
    : m_metrics(metrics), m_controls(controls), m_clientMgr(clientMgr), m_httpPort(httpPort), m_wsPort(wsPort) {}

WebServer::~WebServer() {
    stop();
}

bool WebServer::start() {
    if (m_running.load()) return true;
    m_running.store(true);

    // 1. HTTP Server Thread (cpp-httplib for static web content)
    m_httpThread = std::thread([this]() {
        httplib::Server http_server;

        // Mount common folders if present
        http_server.set_mount_point("/client", "./client");
        http_server.set_mount_point("/web", "./web");
        http_server.set_mount_point("/public", "./public");
        http_server.set_mount_point("/", "./client");
        http_server.set_mount_point("/", "./public");
        http_server.set_mount_point("/", "./web");
        http_server.set_mount_point("/", ".");

        http_server.Get("/", [](const httplib::Request&, httplib::Response& res) {
            if (fs::exists("./client/index.html")) {
                res.set_redirect("/client/index.html");
            } else if (fs::exists("./web/index.html")) {
                res.set_redirect("/web/index.html");
            } else if (fs::exists("./index.html")) {
                res.set_redirect("/index.html");
            } else {
                res.set_content("<html><body><h1>CM5 Audio Server Running</h1><p>Connect WebSocket client to ws://&lt;host&gt;:8183</p></body></html>", "text/html");
            }
        });

        std::cout << "[WebServer] Static HTTP server listening on http://0.0.0.0:" << m_httpPort << std::endl;
        http_server.listen("0.0.0.0", m_httpPort);
    });

    // 2. WebSocket Server Thread (IXWebSocket for audio streaming)
    m_wsThread = std::thread([this]() {
        ix::WebSocketServer ws_server(m_wsPort, "0.0.0.0");
        static std::atomic<uint32_t> nextClientId{1};

        ws_server.setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> webSocket,
                   std::shared_ptr<ix::ConnectionState> connectionState) {

                auto ws = webSocket.lock();
                if (!ws) return;

                uint32_t clientId = nextClientId.fetch_add(1);
                auto session = m_clientMgr.create_session(clientId);
                std::cout << "[WebServer] WS Client connected: " << clientId << std::endl;

                ws->setOnMessageCallback([this, webSocket, session, clientId](const ix::WebSocketMessagePtr& msg) {
                    auto ws = webSocket.lock();
                    if (!ws) return;

                    if (msg->type == ix::WebSocketMessageType::Message) {
                        if (msg->binary && !msg->str.empty()) {
                            session->incoming_rb.write(msg->str.data(), msg->str.size());

                            uint8_t outBuffer[4096];
                            size_t readBytes = session->outgoing_rb.read(outBuffer, sizeof(outBuffer));
                            if (readBytes > 0) {
                                ws->sendBinary(std::string((char*)outBuffer, readBytes));
                            }
                        }
                    } else if (msg->type == ix::WebSocketMessageType::Close) {
                        std::cout << "[WebServer] WS Client disconnected: " << clientId << std::endl;
                        m_clientMgr.remove_session(clientId);
                    }
                });
            }
        );

        auto res = ws_server.listen();
        if (!res.first) {
            std::cerr << "[WebServer] WS Failed to listen on port " << m_wsPort << ": " << res.second << std::endl;
            return;
        }

        std::cout << "[WebServer] Audio WebSocket endpoint active on ws://0.0.0.0:" << m_wsPort << std::endl;
        ws_server.start();

        while (m_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        ws_server.stop();
    });

    return true;
}

void WebServer::stop() {
    if (m_running.exchange(false)) {
        if (m_httpThread.joinable()) {
            m_httpThread.detach();
        }
        if (m_wsThread.joinable()) {
            m_wsThread.join();
        }
    }
}
