#include "web_server.h"
#include "../cpp-httplib/httplib.h"
#include <iostream>
#include <sstream>

static httplib::Server g_svr;

WebServer::WebServer(AudioMetrics& metrics, AudioControls& controls, int port) 
    : m_metrics(metrics), m_controls(controls), m_port(port) {}

WebServer::~WebServer() {
    stop();
}

static const char* DASHBOARD_HTML = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>CM5 Audio — 8x8 Live DSP Control</title>
    <style>
        body { background: #0f111a; color: #eceff4; font-family: 'Segoe UI', monospace; margin: 20px; }
        h1 { text-align: center; color: #88c0d0; margin-bottom: 20px; }
        .top-bar { display: flex; justify-content: space-between; align-items: center; max-width: 1520px; margin: 0 auto 15px auto; }
        .clear-btn { background: #bf616a; color: #fff; border: none; padding: 8px 16px; border-radius: 4px; font-weight: bold; cursor: pointer; }
        .clear-btn:hover { background: #d08770; }
        
        .dashboard { display: flex; flex-wrap: wrap; gap: 20px; justify-content: center; }
        .panel { background: #1a1c23; padding: 20px; border-radius: 10px; border: 1px solid #2e3440; flex: 1; min-width: 480px; max-width: 750px; }
        h2 { color: #81a1c1; border-bottom: 1px solid #3b4252; padding-bottom: 8px; margin-top: 0; }
        
        .meter-grid { display: grid; grid-template-columns: repeat(8, 1fr); gap: 8px; }
        .meter-col { display: flex; flex-direction: column; align-items: center; background: #121318; padding: 8px 4px; border-radius: 6px; border: 1px solid #2e3440; }
        .meter-label { font-size: 11px; font-weight: bold; color: #88c0d0; margin-bottom: 4px; }
        
        /* Clip LED */
        .clip-led { width: 12px; height: 12px; border-radius: 50%; background: #2e1a1e; border: 1px solid #4c282e; margin-bottom: 6px; cursor: pointer; transition: background 0.1s; }
        .clip-led.active { background: #ff0033; border-color: #ff6688; box-shadow: 0 0 8px #ff0033; }

        /* VU Bar with Peak Hold Marker */
        .bar-container { position: relative; width: 16px; height: 120px; background: #2e3440; border-radius: 3px; overflow: hidden; display: flex; align-items: flex-end; }
        .bar-fill { width: 100%; background: linear-gradient(to top, #a3be8c 0%, #a3be8c 60%, #ebcb8b 80%, #bf616a 100%); transition: height 0.05s linear; }
        .peak-hold-marker { position: absolute; left: 0; width: 100%; height: 2px; background: #ffffff; box-shadow: 0 0 3px #ffffff; transition: bottom 0.05s linear; }
        
        .db-readout { font-size: 9px; color: #e5e9f0; margin: 2px 0 0 0; text-align: center; font-family: monospace; }
        .peak-readout { font-size: 8px; color: #ebcb8b; margin-bottom: 4px; text-align: center; font-family: monospace; }

        /* Per-Channel Graph Canvas */
        .ch-graph { width: 100%; height: 45px; background: #0a0b0e; border-radius: 3px; border: 1px solid #2e3440; margin: 4px 0; }

        /* Controls */
        .gain-slider { width: 100%; height: 65px; writing-mode: bt-lr; -webkit-appearance: slider-vertical; margin: 4px 0; }
        .mute-btn { width: 100%; font-size: 9px; font-weight: bold; padding: 3px 0; background: #3b4252; color: #d8dee9; border: none; border-radius: 3px; cursor: pointer; }
        .mute-btn.active { background: #bf616a; color: #fff; }
        .gain-val { font-size: 9px; color: #ebcb8b; margin-top: 2px; }
    </style>
</head>
<body>
    <div class="top-bar">
        <h1>CM5 Audio Engine — DSP Metering & Peak Latching</h1>
        <button class="clear-btn" onclick="clearAllClips()">RESET ALL CLIPS</button>
    </div>

    <div class="dashboard">
        <div class="panel">
            <h2>Capture (ADC8x Channels 1–8)</h2>
            <div class="meter-grid" id="cap-meters"></div>
        </div>
        <div class="panel">
            <h2>Playback (DAC8x Channels 1–8)</h2>
            <div class="meter-grid" id="pb-meters"></div>
        </div>
    </div>

<script>
const HISTORY_LEN = 60;
const historyCap = Array.from({length: 8}, () => new Array(HISTORY_LEN).fill(-60));
const historyPb  = Array.from({length: 8}, () => new Array(HISTORY_LEN).fill(-60));

function initMeters(containerId, type) {
    const grid = document.getElementById(containerId);
    grid.innerHTML = '';
    for (let c = 0; c < 8; c++) {
        grid.innerHTML += `
            <div class="meter-col">
                <div class="meter-label">CH${c+1}</div>
                <div class="clip-led" id="${type}-clip-${c}" title="Click to clear clip" onclick="resetClip('${type}', ${c})"></div>
                <div class="bar-container">
                    <div class="bar-fill" id="${type}-bar-${c}" style="height: 0%;"></div>
                    <div class="peak-hold-marker" id="${type}-pk-${c}" style="bottom: 0%;"></div>
                </div>
                <div class="db-readout" id="${type}-db-${c}">-60.0</div>
                <div class="peak-readout" id="${type}-pkdb-${c}">P: -60.0</div>
                <canvas class="ch-graph" id="${type}-cv-${c}"></canvas>
                <input type="range" class="gain-slider" id="${type}-gain-${c}" min="0" max="2" step="0.02" value="1.0" 
                       oninput="sendControl('${type}', ${c})">
                <div class="gain-val" id="${type}-gval-${c}">1.00x</div>
                <button class="mute-btn" id="${type}-mute-${c}" onclick="toggleMute('${type}', ${c})">MUTE</button>
            </div>`;
    }
}

initMeters('cap-meters', 'capture');
initMeters('pb-meters', 'playback');

const muteStates = { capture: new Array(8).fill(false), playback: new Array(8).fill(false) };

function toggleMute(type, ch) {
    muteStates[type][ch] = !muteStates[type][ch];
    const btn = document.getElementById(`${type}-mute-${ch}`);
    if (muteStates[type][ch]) btn.classList.add('active');
    else btn.classList.remove('active');
    sendControl(type, ch);
}

function sendControl(type, ch) {
    const gain = document.getElementById(`${type}-gain-${ch}`).value;
    const mute = muteStates[type][ch] ? 1 : 0;
    document.getElementById(`${type}-gval-${ch}`).innerText = parseFloat(gain).toFixed(2) + 'x';
    fetch(`/api/control?type=${type}&ch=${ch}&gain=${gain}&mute=${mute}`, { method: 'POST' });
}

function resetClip(type, ch) {
    fetch(`/api/reset_clips?type=${type}&ch=${ch}`, { method: 'POST' });
}

function clearAllClips() {
    fetch(`/api/reset_clips?all=1`, { method: 'POST' });
}

function dbToPct(db) {
    if (db <= -60) return 0;
    if (db >= 0) return 100;
    return ((db + 60) / 60) * 100;
}

function drawChannelSparkline(canvasId, historyData, strokeColor) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width = canvas.clientWidth;
    const h = canvas.height = canvas.clientHeight;

    ctx.clearRect(0, 0, w, h);
    ctx.strokeStyle = '#2e3440';
    ctx.lineWidth = 1;
    [-12, -36].forEach(db => {
        let y = h - (dbToPct(db) / 100 * h);
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    });

    ctx.strokeStyle = strokeColor;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let x = 0; x < HISTORY_LEN; x++) {
        let db = historyData[x];
        let px = (x / (HISTORY_LEN - 1)) * w;
        let py = h - (dbToPct(db) / 100 * h);
        if (x === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
    }
    ctx.stroke();
}

async function fetchMeters() {
    try {
        const res = await fetch('/api/meters');
        const data = await res.json();

        data.capture.forEach((ch, i) => {
            document.getElementById(`capture-bar-${i}`).style.height = dbToPct(ch.rms_db) + '%';
            document.getElementById(`capture-pk-${i}`).style.bottom = dbToPct(ch.peak_hold_db) + '%';
            document.getElementById(`capture-db-${i}`).innerText = ch.rms_db.toFixed(1);
            document.getElementById(`capture-pkdb-${i}`).innerText = 'P:' + ch.peak_hold_db.toFixed(1);
            
            const clipLed = document.getElementById(`capture-clip-${i}`);
            if (ch.clipped) clipLed.classList.add('active');
            else clipLed.classList.remove('active');

            historyCap[i].shift(); historyCap[i].push(ch.rms_db);
            drawChannelSparkline(`capture-cv-${i}`, historyCap[i], '#88c0d0');
        });

        data.playback.forEach((ch, i) => {
            document.getElementById(`playback-bar-${i}`).style.height = dbToPct(ch.rms_db) + '%';
            document.getElementById(`playback-pk-${i}`).style.bottom = dbToPct(ch.peak_hold_db) + '%';
            document.getElementById(`playback-db-${i}`).innerText = ch.rms_db.toFixed(1);
            document.getElementById(`playback-pkdb-${i}`).innerText = 'P:' + ch.peak_hold_db.toFixed(1);

            const clipLed = document.getElementById(`playback-clip-${i}`);
            if (ch.clipped) clipLed.classList.add('active');
            else clipLed.classList.remove('active');

            historyPb[i].shift(); historyPb[i].push(ch.rms_db);
            drawChannelSparkline(`playback-cv-${i}`, historyPb[i], '#a3be8c');
        });
    } catch (e) {}
}

setInterval(fetchMeters, 50);
</script>
</body>
</html>
)rawhtml";

bool WebServer::start() {
    if (m_running) return true;

    g_svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(DASHBOARD_HTML, "text/html");
    });

    g_svr.Post("/api/control", [this](const httplib::Request& req, httplib::Response& res) {
        std::string type = req.get_param_value("type");
        int ch = req.has_param("ch") ? std::stoi(req.get_param_value("ch")) : -1;

        if (ch >= 0 && ch < 8) {
            auto& target = (type == "playback") ? m_controls.playback[ch] : m_controls.capture[ch];
            if (req.has_param("gain")) {
                target.gain.store(std::stof(req.get_param_value("gain")), std::memory_order_relaxed);
            }
            if (req.has_param("mute")) {
                target.mute.store(req.get_param_value("mute") == "1", std::memory_order_relaxed);
            }
            res.set_content("{\"status\":\"ok\"}", "application/json");
        } else {
            res.status = 400;
            res.set_content("{\"status\":\"error\"}", "application/json");
        }
    });

    // Reset Clip LEDs API Endpoint
    g_svr.Post("/api/reset_clips", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("all")) {
            for (int i = 0; i < 8; ++i) {
                m_metrics.capture[i].clipped.store(false, std::memory_order_relaxed);
                m_metrics.capture[i].peak_hold_db.store(-60.0f, std::memory_order_relaxed);
                m_metrics.playback[i].clipped.store(false, std::memory_order_relaxed);
                m_metrics.playback[i].peak_hold_db.store(-60.0f, std::memory_order_relaxed);
            }
        } else {
            std::string type = req.get_param_value("type");
            int ch = req.has_param("ch") ? std::stoi(req.get_param_value("ch")) : -1;
            if (ch >= 0 && ch < 8) {
                auto& target = (type == "playback") ? m_metrics.playback[ch] : m_metrics.capture[ch];
                target.clipped.store(false, std::memory_order_relaxed);
                target.peak_hold_db.store(-60.0f, std::memory_order_relaxed);
            }
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // Meters API JSON
    g_svr.Get("/api/meters", [this](const httplib::Request&, httplib::Response& res) {
        std::stringstream ss;
        ss << "{\"capture\":[";
        for (int i = 0; i < 8; ++i) {
            ss << "{\"ch\":" << i
               << ",\"rms_db\":" << m_metrics.capture[i].rms_db.load(std::memory_order_relaxed)
               << ",\"peak_db\":" << m_metrics.capture[i].peak_db.load(std::memory_order_relaxed)
               << ",\"peak_hold_db\":" << m_metrics.capture[i].peak_hold_db.load(std::memory_order_relaxed)
               << ",\"clipped\":" << (m_metrics.capture[i].clipped.load(std::memory_order_relaxed) ? "true" : "false") << "}"
               << (i < 7 ? "," : "");
        }
        ss << "],\"playback\":[";
        for (int i = 0; i < 8; ++i) {
            ss << "{\"ch\":" << i
               << ",\"rms_db\":" << m_metrics.playback[i].rms_db.load(std::memory_order_relaxed)
               << ",\"peak_db\":" << m_metrics.playback[i].peak_db.load(std::memory_order_relaxed)
               << ",\"peak_hold_db\":" << m_metrics.playback[i].peak_hold_db.load(std::memory_order_relaxed)
               << ",\"clipped\":" << (m_metrics.playback[i].clipped.load(std::memory_order_relaxed) ? "true" : "false") << "}"
               << (i < 7 ? "," : "");
        }
        ss << "]}";

        res.set_content(ss.str(), "application/json");
    });

    m_running = true;
    m_serverThread = std::thread([this]() {
        std::cout << "[HTTP] Server listening on http://0.0.0.0:" << m_port << std::endl;
        g_svr.listen("0.0.0.0", m_port);
        m_running = false;
    });

    return true;
}

void WebServer::stop() {
    if (m_running) {
        g_svr.stop();
        if (m_serverThread.joinable()) {
            m_serverThread.join();
        }
        m_running = false;
    }
}
