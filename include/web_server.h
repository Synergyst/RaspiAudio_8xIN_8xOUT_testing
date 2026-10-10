#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "ring_buffer.h"
#include "constants.h"

namespace ix { class WebSocket; }

struct ChannelMeter {
    std::atomic<float> raw_value{0.0f};
    std::atomic<float> raw_peak{0.0f};
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
    std::unordered_map<std::string, std::array<ChannelMeter, CM5_MAX_CHANNELS>> capture;
    std::unordered_map<std::string, std::array<ChannelMeter, CM5_MAX_CHANNELS>> playback;
};

struct AudioControls {
    std::unordered_map<std::string, std::array<ChannelControl, CM5_MAX_CHANNELS>> capture;
    std::unordered_map<std::string, std::array<ChannelControl, CM5_MAX_CHANNELS>> playback;
};

struct ToneControls {
    std::atomic<bool> enabled{false};
    std::atomic<float> frequency_hz{440.0f};
    std::atomic<float> amplitude{0.2f};
};

struct WebClientSession {
    uint32_t id = 0;
    std::string remote_ip;
    std::shared_ptr<const std::string> client_key;
    std::shared_ptr<const std::string> client_name;
    std::atomic<unsigned> input_channels{1};
    std::atomic<unsigned> output_channels{2};
    PcmRingBuffer incoming_rb;
    PcmRingBuffer outgoing_rb;
    std::vector<float> input_block;
    std::vector<float> output_block;
    std::array<ChannelMeter, CM5_MAX_CHANNELS> input_metrics;
    std::array<ChannelMeter, CM5_MAX_CHANNELS> output_metrics;
    std::vector<float> packet_block;
    std::atomic<bool> active{true};
    std::thread sender_thread;

    std::string get_client_key() const;
    std::string get_client_name() const;
    void set_identity(const std::string& key, const std::string& name);
    void start_sender(const std::shared_ptr<ix::WebSocket>& socket);
    void stop_sender();
    ~WebClientSession();
};

struct AudioRoute {
    uint32_t id = 0;
    std::string source_endpoint;
    unsigned source_channel = 0;
    std::string destination_endpoint;
    unsigned destination_channel = 0;
    float gain = 1.0f;
    bool enabled = true;
};

class DeviceManager;
class DspEngine;

class ClientManager {
public:
    ClientManager();
    void set_device_manager(DeviceManager* dm) { m_deviceManager = dm; }
    std::shared_ptr<WebClientSession> create_session(uint32_t id, const std::string& remoteIp = "");
    void remove_session(uint32_t id);
    std::vector<std::shared_ptr<WebClientSession>> get_active_sessions();
    bool claim_identity(const std::shared_ptr<WebClientSession>& session,
                       const std::string& key, const std::string& name);
    bool load_settings(AudioControls& controls, ToneControls& tone);
    bool save_settings(const AudioControls& controls, const ToneControls& tone) const;
    std::unordered_map<std::string, std::string> known_client_names() const;

    std::shared_ptr<const std::vector<AudioRoute>> route_snapshot() const;
    std::vector<AudioRoute> get_routes() const;
    bool add_route(AudioRoute route, uint32_t& assignedId, std::string& error);
    bool remove_route(uint32_t routeId);
    bool update_route(uint32_t routeId, float gain, bool enabled);
    void update_activated_device(const std::string& id, bool active);

private:
    bool endpoint_exists(const std::string& endpoint, bool source) const;
    mutable std::mutex m_lock;
    std::vector<std::shared_ptr<WebClientSession>> m_sessions;
    mutable std::mutex m_route_lock;
    std::shared_ptr<const std::vector<AudioRoute>> m_routes;
    std::unordered_map<std::string, std::string> m_known_client_names;
    std::vector<std::string> m_activatedDevices;
    std::atomic<uint32_t> m_next_route_id{1};
    DeviceManager* m_deviceManager = nullptr;
};

class WebServer {
public:
    WebServer(AudioMetrics& metrics, AudioControls& controls, ToneControls& tone,
              ClientManager& clientMgr, DeviceManager& deviceMgr, DspEngine& dspEngine, 
              int httpPort = 8182, int wsPort = 8183, bool plainText = false);
    ~WebServer();
    bool start();
    void stop();

private:
    AudioMetrics& m_metrics;
    AudioControls& m_controls;
    ToneControls& m_tone;
    ClientManager& m_clientMgr;
    DeviceManager& m_deviceMgr;
    DspEngine& m_dspEngine;
    int m_httpPort;
    int m_wsPort;
    bool m_plainText;
    std::atomic<bool> m_running{false};
    std::thread m_httpThread;
    std::thread m_wsThread;
};

#endif
