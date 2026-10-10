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

    // --- CRITICAL INITIALIZATION SEQUENCE ---
    
    // 1. Enumerate the hardware first so the system knows what devices exist
    g_deviceMgr.enumerate_devices();
    std::cout << "Detected " << g_deviceMgr.get_available_devices().size() << " devices." << std::endl;

    // 2. Link the ClientManager to the DeviceManager
    // This must happen BEFORE loading settings so that load_settings can call activate_device()
    g_clientMgr.set_device_manager(&g_deviceMgr);

    // 3. Load settings from disk
    // This now restores saved routes AND automatically activates previously enabled hardware
    g_clientMgr.load_settings(g_controls, g_tone);

    // 4. Initialize and start the DSP Engine
    g_dspEngine = std::make_unique<DspEngine>(g_deviceMgr, g_clientMgr, g_metrics);
    g_dspEngine->start();

    // 5. Start the Web Server
    WebServer webServer(g_metrics, g_controls, g_tone, g_clientMgr, g_deviceMgr, *g_dspEngine, 8182, 8183, plainText);
    if (!webServer.start()) return -1;

    std::cout << "Operational: http://192.168.168.172:8182/" << std::endl;
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    g_dspEngine->stop();
    webServer.stop();
    return 0;
}
