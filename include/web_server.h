#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <thread>
#include <atomic>
#include <array>
#include <vector>
#include <memory>
#include <mutex>
#include <cstring>
#include <algorithm>
#include "miniaudio.h"

struct ChannelMeter {
    std::atomic<float> rms_db{-60.0f};
    std::atomic<float> peak_db{-60.0f};
    std::atomic<float> peak_hold_db{-60.0f};
    std::atomic<bool> clipped{false};
};

struct ChannelControl {
    std::atomic<float> gain{1.0f};
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

struct PcmRingBuffer {
    std::vector<uint8_t> buffer;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;
    size_t capacity = 0;
    mutable std::mutex lock;

    bool init(size_t capacityBytes) {
        std::lock_guard<std::mutex> g(lock);
        buffer.resize(capacityBytes);
        capacity = capacityBytes;
        head = 0;
        tail = 0;
        count = 0;
        return true;
    }

    void uninit() {
        std::lock_guard<std::mutex> g(lock);
        buffer.clear();
        capacity = 0;
        head = 0;
        tail = 0;
        count = 0;
    }

    size_t write(const void* data, size_t bytes) {
        std::lock_guard<std::mutex> g(lock);
        if (capacity == 0 || bytes == 0) return 0;
        size_t bytesToWrite = std::min(bytes, capacity - count);
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytesToWrite; ++i) {
            buffer[head] = src[i];
            head = (head + 1) % capacity;
        }
        count += bytesToWrite;
        return bytesToWrite;
    }

    size_t read(void* data, size_t bytes) {
        std::lock_guard<std::mutex> g(lock);
        if (capacity == 0 || bytes == 0) return 0;
        size_t bytesToRead = std::min(bytes, count);
        uint8_t* dst = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < bytesToRead; ++i) {
            dst[i] = buffer[tail];
            tail = (tail + 1) % capacity;
        }
        count -= bytesToRead;
        return bytesToRead;
    }

    size_t available_read() const {
        std::lock_guard<std::mutex> g(lock);
        return count;
    }
};

struct WebClientSession {
    uint32_t id;
    PcmRingBuffer incoming_rb;
    PcmRingBuffer outgoing_rb;
    std::atomic<bool> active{true};
};

class ClientManager {
public:
    std::shared_ptr<WebClientSession> create_session(uint32_t id);
    void remove_session(uint32_t id);
    std::vector<std::shared_ptr<WebClientSession>> get_active_sessions();

private:
    std::mutex m_lock;
    std::vector<std::shared_ptr<WebClientSession>> m_sessions;
};

class WebServer {
public:
    WebServer(AudioMetrics& metrics, AudioControls& controls, ClientManager& clientMgr, int httpPort = 8182, int wsPort = 8183);
    ~WebServer();

    bool start();
    void stop();

private:
    AudioMetrics& m_metrics;
    AudioControls& m_controls;
    ClientManager& m_clientMgr;
    int m_httpPort;
    int m_wsPort;
    std::atomic<bool> m_running{false};
    std::thread m_httpThread;
    std::thread m_wsThread;
};

#endif // WEB_SERVER_H
