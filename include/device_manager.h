#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include "miniaudio.h"
#include "constants.h"
#include "ring_buffer.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

struct AudioDeviceInfo {
    ma_device_id id;
    std::string name;
    bool is_capture;
    unsigned channels;
    unsigned sample_rate;
};

class HardwareDevice {
public:
    HardwareDevice(const AudioDeviceInfo& info);
    ~HardwareDevice();

    bool init();
    void stop();
    
    const AudioDeviceInfo& get_info() const { return m_info; }
    PcmRingBuffer& get_input_buffer() { return m_input_rb; }
    PcmRingBuffer& get_output_buffer() { return m_output_rb; }

private:
    static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

    AudioDeviceInfo m_info;
    ma_device m_device;
    PcmRingBuffer m_input_rb;
    PcmRingBuffer m_output_rb;
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
    std::unordered_map<std::string, std::shared_ptr<HardwareDevice>> m_active_devices;
};

#endif
