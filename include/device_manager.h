#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include "miniaudio.h"
#pragma GCC diagnostic pop

#include "constants.h"
#include "ring_buffer.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

// Forward declaration of NodeId from web_server.h
using NodeId = int32_t;

// A native capability is a JOINT tuple, not four independent maxima. Unknown
// format / zero channels / zero sample_rate retain miniaudio's wildcard meaning.
struct NativeStreamFormat {
    ma_format format = ma_format_unknown;
    unsigned channels = 0;
    unsigned sample_rate = 0;
    unsigned flags = 0;
};

struct AudioDeviceInfo {
    ma_device_id id{};
    std::string name;
    bool is_capture = false;
    unsigned channels = 0;
    unsigned sample_rate = 0;
    // Stable within the selected backend. ALSA and Pulse keep their raw IDs;
    // JACK uses "jack:0". The null test backend retains the legacy empty ID.
    std::string stable_id;
    std::string backend;
    std::vector<NativeStreamFormat> native_formats; // Only populated by an explicit query.
    unsigned requested_channels = 0; // Zero = backend-native/auto, NOT a maximum.
    // channels/sample_rate above describe the opened f32/48 kHz CLIENT stream.
    // Enumeration leaves them unknown (zero). Internal means backend stream,
    // not a promise about physical ports behind a server or an ALSA plugin.
    unsigned internal_channels = 0;
    unsigned internal_sample_rate = 0;
    ma_format internal_format = ma_format_unknown;
    std::vector<unsigned> client_channel_map;
    std::vector<unsigned> internal_channel_map;
};

class HardwareDevice {
public:
    HardwareDevice(const AudioDeviceInfo& info, ma_context* context, const std::string& originalId);
    ~HardwareDevice();
    HardwareDevice(const HardwareDevice&) = delete;
    HardwareDevice& operator=(const HardwareDevice&) = delete;

    // Lifecycle calls are serialized by the owner, never by the audio callback.
    bool init();
    void stop();

    const AudioDeviceInfo& get_info() const { return m_info; }
    const std::string& last_error() const { return m_last_error; }
    PcmRingBuffer& get_input_buffer() { return m_input_rb; }
    PcmRingBuffer& get_output_buffer() { return m_output_rb; }
    const std::string& get_original_id() const { return m_originalId; }

    // Node ID tracking for zero-allocation routing
    NodeId node_id = -1;

    // Prepared before device start; never resized while callbacks/DSP use it.
    std::vector<float> io_buffer;

private:
    friend struct HardwareDeviceTestAccess;
    static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    AudioDeviceInfo m_info;
    ma_context* m_context;
    ma_device m_device{};
    bool m_device_initialized = false;
    PcmRingBuffer m_input_rb;
    PcmRingBuffer m_output_rb;
    std::string m_originalId;
    unsigned m_requested_channels = 0;
    std::string m_last_error;
};

class DeviceManager {
public:
    explicit DeviceManager(bool initializeBackend = true);
    ~DeviceManager();

    bool initialized() const { return m_context_initialized; }
    bool is_initialized() const { return initialized(); }
    void enumerate_devices();
    std::vector<AudioDeviceInfo> get_available_devices();
    std::vector<std::shared_ptr<HardwareDevice>> get_active_devices();
    // Queries ONLY this enumerated endpoint; may temporarily open it to probe.
    // An empty result is not a maximum or evidence that no channels exist.
    std::vector<NativeStreamFormat> get_device_capabilities(
        const std::string& id, bool capture, std::string* error = nullptr);
    // Existing one-argument callers retain auto selection. A nonzero request
    // must match the backend stream width; no channel up/downmix is accepted.
    // An active endpoint is never reconfigured: deactivate before changing width.
    bool activate_device(const std::string& id, unsigned captureChannels = 0,
                         unsigned playbackChannels = 0, std::string* error = nullptr);
    void deactivate_device(const std::string& id);
    void stop_all();

private:
    friend struct DeviceManagerTestAccess;
    std::mutex m_lock;
    ma_context m_context{};
    bool m_context_initialized = false;
    // Invalidates in-flight activations when a removal/stop races device init.
    std::uint64_t m_activation_generation = 0;
    std::vector<AudioDeviceInfo> m_available_devices;
    // Key is "ID_capture" or "ID_playback" to support duplex devices.
    std::unordered_map<std::string, std::shared_ptr<HardwareDevice>> m_active_devices;
};

#endif
