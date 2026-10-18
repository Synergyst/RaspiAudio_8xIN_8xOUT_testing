#include "miniaudio.h"
#include "web_server.h"
#include "device_manager.h"
#include "dsp_engine.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <memory>

std::atomic<bool> g_running(true);
AudioMetrics g_metrics;
AudioControls g_controls;
ToneControls g_tone;
ClientManager g_clientMgr;
DeviceManager g_deviceMgr;
std::unique_ptr<DspEngine> g_dspEngine;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) g_running = false;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    bool plainText = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--plain-text") plainText = true;
    }

    std::cout << "Starting CM5 Audio Network Patchbay..." << std::endl;

    // Enumerate hardware before restoring its activation state.
    g_deviceMgr.enumerate_devices();
    std::cout << "Detected " << g_deviceMgr.get_available_devices().size() << " devices." << std::endl;
    g_clientMgr.set_device_manager(&g_deviceMgr);

    // Construct and link DSP before loading settings, but start processing only after
    // restoration. Otherwise saved route chains are skipped and graph changes never
    // notify the engine through ClientManager::mark_graph_dirty().
    const auto discovery = ProcessorRegistry::instance().discovery();
    std::cout << "[DSP] Registered processors: " << discovery["processors"].size() << std::endl;
    g_dspEngine = std::make_unique<DspEngine>(g_deviceMgr, g_clientMgr, g_metrics, g_tone);
    g_clientMgr.set_dsp_engine(g_dspEngine.get());
    g_clientMgr.load_settings(g_controls, g_tone);
    g_dspEngine->start();

    WebServer webServer(g_metrics, g_controls, g_tone, g_clientMgr, g_deviceMgr, *g_dspEngine, 8182, 8183, plainText);
    if (!webServer.start()) return -1;

    std::cout << "Operational: http://192.168.168.172:8182/" << std::endl;
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    g_dspEngine->stop();
    webServer.stop();
    return 0;
}
