#include "web_server.h"
#include "httplib.h"
#include <ixwebsocket/IXWebSocketServer.h>
#include <iostream>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <sstream>
#include <iomanip>

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

    // 1. HTTP Server Thread (Full Dashboard UI & REST/JSON telemetry)
    m_httpThread = std::thread([this]() {
        httplib::Server http_server;

        // Mount web_client directory for /client/* requests
        http_server.set_mount_point("/client", "./web_client");

        // JSON Metrics endpoint for real-time dashboard polling
        http_server.Get("/api/metrics", [this](const httplib::Request&, httplib::Response& res) {
            std::ostringstream json;
            json << "{";
            
            // Capture channels
            json << "\"capture\":[";
            for (size_t i = 0; i < 8; ++i) {
                auto& m = m_metrics.capture[i];
                json << "{"
                     << "\"rms\":" << m.rms_db.load() << ","
                     << "\"peak\":" << m.peak_db.load() << ","
                     << "\"peak_hold\":" << m.peak_hold_db.load() << ","
                     << "\"clipped\":" << (m.clipped.load() ? "true" : "false")
                     << "}" << (i < 7 ? "," : "");
            }
            json << "],";

            // Playback channels
            json << "\"playback\":[";
            for (size_t i = 0; i < 8; ++i) {
                auto& m = m_metrics.playback[i];
                json << "{"
                     << "\"rms\":" << m.rms_db.load() << ","
                     << "\"peak\":" << m.peak_db.load() << ","
                     << "\"peak_hold\":" << m.peak_hold_db.load() << ","
                     << "\"clipped\":" << (m.clipped.load() ? "true" : "false")
                     << "}" << (i < 7 ? "," : "");
            }
            json << "]";
            json << "}";

            res.set_content(json.str(), "application/json");
        });

        // Full Interactive Dashboard UI
        http_server.Get("/", [](const httplib::Request&, httplib::Response& res) {
            const char* dashboard_html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>CM5 Audio Engine Dashboard</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; }
        h1, h2 { color: #38bdf8; border-bottom: 1px solid #334155; padding-bottom: 8px; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 20px; }
        .card { background: #1e293b; border: 1px solid #334155; border-radius: 8px; padding: 15px; box-shadow: 0 4px 6px -1px rgb(0 0 0 / 0.1); }
        .channel-row { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; background: #0f172a; padding: 8px 12px; border-radius: 4px; }
        .meter-bar-container { flex-grow: 1; height: 16px; background: #334155; border-radius: 4px; margin: 0 10px; overflow: hidden; position: relative; }
        .meter-bar { height: 100%; width: 0%; background: linear-gradient(90deg, #22c55e 0%, #eab308 75%, #ef4444 95%); transition: width 0.05s linear; }
        .clip-led { width: 12px; height: 12px; border-radius: 50%; background: #334155; display: inline-block; margin-left: 8px; }
        .clip-led.active { background: #ef4444; box-shadow: 0 0 8px #ef4444; }
        .controls { margin-top: 15px; display: flex; gap: 10px; }
        button { background: #0284c7; color: white; border: none; padding: 6px 12px; border-radius: 4px; cursor: pointer; font-weight: bold; }
        button:hover { background: #0369a1; }
        a { color: #38bdf8; text-decoration: none; }
        a:hover { text-decoration: underline; }
        .nav-bar { margin-bottom: 20px; background: #1e293b; padding: 10px 15px; border-radius: 8px; display: flex; justify-content: space-between; align-items: center; }
    </style>
</head>
<body>
    <div class="nav-bar">
        <h1>CM5 Audio Engine Dashboard</h1>
        <div><a href="/client/index.html" target="_blank">Open WASM Audio Client Terminal &rarr;</a></div>
    </div>

    <div class="grid">
        <div class="card">
            <h2>Capture Channels (Input 1-8)</h2>
            <div id="capture-meters">Loading meters...</div>
        </div>
        <div class="card">
            <h2>Playback Channels (Output 1-8)</h2>
            <div id="playback-meters">Loading meters...</div>
        </div>
    </div>

    <script>
        function dbToPercent(db) {
            // Map -60dB to 0dB range to 0% to 100%
            let pct = ((db + 60.0) / 60.0) * 100;
            return Math.max(0, Math.min(100, pct));
        }

        async function updateMetrics() {
            try {
                let res = await fetch('/api/metrics');
                let data = await res.json();

                let capHtml = '';
                for (let i = 0; i < 8; i++) {
                    let m = data.capture[i];
                    let pct = dbToPercent(m.peak);
                    let clipClass = m.clipped ? 'clip-led active' : 'clip-led';
                    capHtml += `
                        <div class="channel-row">
                            <span>CH ${i+1}</span>
                            <div class="meter-bar-container">
                                <div class="meter-bar" style="width: ${pct}%"></div>
                            </div>
                            <span style="font-size: 12px; width: 65px; text-align: right;">${m.peak.toFixed(1)} dB</span>
                            <span class="${clipClass}" title="Clipping Indicator"></span>
                        </div>
                    `;
                }
                document.getElementById('capture-meters').innerHTML = capHtml;

                let playHtml = '';
                for (let i = 0; i < 8; i++) {
                    let m = data.playback[i];
                    let pct = dbToPercent(m.peak);
                    let clipClass = m.clipped ? 'clip-led active' : 'clip-led';
                    playHtml += `
                        <div class="channel-row">
                            <span>CH ${i+1}</span>
                            <div class="meter-bar-container">
                                <div class="meter-bar" style="width: ${pct}%"></div>
                            </div>
                            <span style="font-size: 12px; width: 65px; text-align: right;">${m.peak.toFixed(1)} dB</span>
                            <span class="${clipClass}" title="Clipping Indicator"></span>
                        </div>
                    `;
                }
                document.getElementById('playback-meters').innerHTML = playHtml;
            } catch (err) {
                console.error('Failed to fetch metrics:', err);
            }
        }

        setInterval(updateMetrics, 100);
    </script>
</body>
</html>
            )HTML";
            res.set_content(dashboard_html, "text/html");
        });

        std::cout << "[WebServer] Dashboard HTTP server listening on http://0.0.0.0:" << m_httpPort << std::endl;
        http_server.listen("0.0.0.0", m_httpPort);
    });

    // 2. WebSocket Server Thread (IXWebSocket Audio Streaming)
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
