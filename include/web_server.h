#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "miniaudio.h"

namespace ix { class WebSocket; }

constexpr unsigned CM5_MAX_CHANNELS = 8;
constexpr unsigned CM5_MAX_AUDIO_FRAMES = 4096;

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
    std::array<ChannelMeter, CM5_MAX_CHANNELS> capture;
    std::array<ChannelMeter, CM5_MAX_CHANNELS> playback;
};

struct AudioControls {
    std::array<ChannelControl, CM5_MAX_CHANNELS> capture;
    std::array<ChannelControl, CM5_MAX_CHANNELS> playback;
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
        head = tail = count = 0;
        return true;
    }
    void uninit() {
        std::lock_guard<std::mutex> g(lock);
        buffer.clear();
        capacity = head = tail = count = 0;
    }
    size_t write(const void* data, size_t bytes) {
        std::lock_guard<std::mutex> g(lock);
        if (!capacity || !bytes) return 0;
        const size_t n = std::min(bytes, capacity - count);
        const auto* src = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { buffer[head] = src[i]; head = (head + 1) % capacity; }
        count += n;
        return n;
    }
    size_t read(void* data, size_t bytes) {
        std::lock_guard<std::mutex> g(lock);
        if (!capacity || !bytes) return 0;
        const size_t n = std::min(bytes, count);
        auto* dst = static_cast<uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { dst[i] = buffer[tail]; tail = (tail + 1) % capacity; }
        count -= n;
        return n;
    }
};

struct WebClientSession {
    uint32_t id = 0;
    std::string remote_ip;
    std::atomic<unsigned> input_channels{1};   // client -> server
    std::atomic<unsigned> output_channels{2};  // server -> client
    PcmRingBuffer incoming_rb;
    PcmRingBuffer outgoing_rb;
    std::vector<float> input_block;
    std::vector<float> output_block;
    std::vector<float> packet_block;
    std::atomic<bool> active{true};
    std::thread sender_thread;

    void start_sender(const std::shared_ptr<ix::WebSocket>& socket);
    void stop_sender();
    ~WebClientSession();
};

struct AudioRoute {
    uint32_t id = 0;
    std::string source_endpoint;      // hardware/capture or client/<id>/capture
    unsigned source_channel = 0;
    std::string destination_endpoint; // hardware/playback or client/<id>/playback
    unsigned destination_channel = 0;
    float gain = 1.0f;
    bool enabled = true;
};

class ClientManager {
public:
    ClientManager();
    std::shared_ptr<WebClientSession> create_session(uint32_t id, const std::string& remoteIp = "");
    void remove_session(uint32_t id);
    std::vector<std::shared_ptr<WebClientSession>> get_active_sessions();

    std::shared_ptr<const std::vector<AudioRoute>> route_snapshot() const;
    std::vector<AudioRoute> get_routes() const;
    bool add_route(AudioRoute route, uint32_t& assignedId, std::string& error);
    bool remove_route(uint32_t routeId);
    bool update_route(uint32_t routeId, float gain, bool enabled);

private:
    bool endpoint_exists(const std::string& endpoint, bool source) const;
    std::mutex m_lock;
    std::vector<std::shared_ptr<WebClientSession>> m_sessions;
    mutable std::mutex m_route_lock;
    std::shared_ptr<const std::vector<AudioRoute>> m_routes;
    std::atomic<uint32_t> m_next_route_id{1};
};

class WebServer {
public:
    WebServer(AudioMetrics& metrics, AudioControls& controls, ClientManager& clientMgr,
              int httpPort = 8182, int wsPort = 8183);
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

#endif
