#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <thread>
#include <atomic>
#include <array>

struct ChannelMeter {
    std::atomic<float> rms_db{-60.0f};
    std::atomic<float> peak_db{-60.0f};
    std::atomic<float> peak_hold_db{-60.0f};
    std::atomic<bool> clipped{false};
};

struct ChannelControl {
    std::atomic<float> gain{1.0f};  // Linear gain multiplier (1.0 = 0dB unity)
    std::atomic<bool> mute{false};
};

struct AudioMetrics {
    std::array<ChannelMeter, 8> capture;
    std::array<ChannelMeter, 8> playback;
};

struct AudioControls {
    std::array<ChannelControl, 8> capture;
    std::array<ChannelControl, 8> playback;
};

class WebServer {
public:
    WebServer(AudioMetrics& metrics, AudioControls& controls, int port = 8182);
    ~WebServer();

    bool start();
    void stop();

private:
    AudioMetrics& m_metrics;
    AudioControls& m_controls;
    int m_port;
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;
};

#endif // WEB_SERVER_H
