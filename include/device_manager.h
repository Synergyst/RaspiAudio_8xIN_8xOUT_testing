#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include "miniaudio.h"
#pragma GCC diagnostic pop

#include "constants.h"
#include "ring_buffer.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

// Forward declaration of NodeId from web_server.h
using NodeId = int32_t;

struct AudioDeviceInfo {
    ma_device_id id;
    std::string name;
    bool is_capture;
    unsigned channels;
    unsigned sample_rate;
};

class HardwareDevice {
public:
    HardwareDevice(const AudioDeviceInfo& info, ma_context* context, const std::string& originalId);
    ~HardwareDevice();

    bool init();
    void stop();
    
    const AudioDeviceInfo& get_info() const { return m_info; }
    PcmRingBuffer& get_input_buffer() { return m_input_rb; }
    PcmRingBuffer& get_output_buffer() { return m_output_rb; }
    const std::string& get_original_id() const { return m_originalId; }

    // Node ID tracking for zero-allocation routing
    NodeId node_id = -1;

    // Pre-allocated buffers for zero-allocation I/O
    std::vector<float> io_buffer;

private:
    static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    AudioDeviceInfo m_info;
    ma_context* m_context;
    ma_device m_device;
    PcmRingBuffer m_input_rb;
    PcmRingBuffer m_output_rb;
    std::string m_originalId;
};

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    void enumerate_devices();
    std::vector<AudioDeviceInfo> get_available_devices();
    std::vector<std::shared_ptr<HardwareDevice>> get_active_devices();
    bool activate_device(const std::string& id);
    void deactivate_device(const std::string& id);
    void stop_all();

private:
    std::mutex m_lock;
    ma_context m_context;
    std::vector<AudioDeviceInfo> m_available_devices;
    // Key is now "ID_capture" or "ID_playback" to support duplex devices
    std::unordered_map<std::string, std::shared_ptr<HardwareDevice>> m_active_devices;
};

#endif
