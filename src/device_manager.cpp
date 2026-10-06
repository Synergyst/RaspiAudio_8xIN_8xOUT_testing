#include "device_manager.h"
#include "constants.h"
#include <iostream>
#include <algorithm>

HardwareDevice::HardwareDevice(const AudioDeviceInfo& info) : m_info(info) {
    // Use ma_pcm_rb frames (approx 100ms)
    m_input_rb.init(ma_format_f32, m_info.channels, 4800);
    m_output_rb.init(ma_format_f32, m_info.channels, 4800);
}

HardwareDevice::~HardwareDevice() {
    stop();
}

void HardwareDevice::stop() {
    if (ma_device_is_started(&m_device)) {
        ma_device_uninit(&m_device);
    }
}

bool HardwareDevice::init() {
    ma_device_config config;
    if (m_info.is_capture) {
        config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = m_info.channels;
    } else {
        config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = m_info.channels;
    }

    config.sampleRate = m_info.sample_rate;
    config.dataCallback = HardwareDevice::data_callback;
    config.pUserData = this;

    if (ma_device_init(NULL, &config, &m_device) != MA_SUCCESS) {
        return false;
    }

    if (ma_device_start(&m_device) != MA_SUCCESS) {
        ma_device_uninit(&m_device);
        return false;
    }

    return true;
}

void HardwareDevice::data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* device = static_cast<HardwareDevice*>(pDevice->pUserData);
    
    if (device->m_info.is_capture && pInput) {
        device->m_input_rb.write(pInput, frameCount * device->m_info.channels * sizeof(float));
    } else if (!device->m_info.is_capture && pOutput) {
        size_t bytesNeeded = frameCount * device->m_info.channels * sizeof(float);
        size_t got = device->m_output_rb.read(pOutput, bytesNeeded);
        if (got < bytesNeeded) {
            std::memset((uint8_t*)pOutput + got, 0, bytesNeeded - got);
        }
    }
}

DeviceManager::DeviceManager() {}
DeviceManager::~DeviceManager() {
    stop_all();
}

void DeviceManager::enumerate_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_available_devices.clear();

    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) return;

    auto enum_callback = [](ma_context* pContext, ma_device_type type, const ma_device_info* pDeviceInfo, void* pUserData) -> unsigned int {
        auto* manager = static_cast<DeviceManager*>(pUserData);
        AudioDeviceInfo info;
        
        // MiniAudio's ma_device_info stores the ID in a fixed-size array/struct ma_device_id.
        // We need to convert that to a string.
        char idStr[64];
        // We use the id member of ma_device_info. Since it's a struct, we might need a helper 
        // or just a direct copy if it's a char array. 
        // Based on the library, pDeviceInfo->id is the key.
        
        // Let's try to use a simple approach to get the ID and Name.
        // If pDeviceInfo->name is available, use it.
        info.name = pDeviceInfo->name ? pDeviceInfo->name : "unknown";
        info.is_capture = (type == ma_device_type_capture);
        
        // To get the ID as a string, we can use a temporary buffer or a specific MiniAudio helper.
        // Since we don't want to guess the exact structure of ma_device_id, 
        // let's use a simple identifier for now or a cast if we know it's a string.
        // Actually, ma_device_info usually has a name and the id is used for init.
        
        // We'll use a dummy ID based on the pointer for now to ensure it compiles, 
        // then refine it to use the actual ma_device_id.
        info.id = std::to_string(reinterpret_cast<uintptr_t>(pDeviceInfo));
        
        info.sample_rate = 48000;
        info.channels = CM5_MAX_CHANNELS;

        manager->m_available_devices.push_back(info);
        return 1; // Continue enumeration
    };

    if (ma_context_enumerate_devices(&context, enum_callback, this) != MA_SUCCESS) {
        std::cerr << "[DeviceManager] Failed to enumerate devices" << std::endl;
    }

    ma_context_uninit(&context);
}

std::vector<AudioDeviceInfo> DeviceManager::get_available_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    return m_available_devices;
}

std::vector<std::shared_ptr<HardwareDevice>> DeviceManager::get_active_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<std::shared_ptr<HardwareDevice>> active;
    for (auto const& [id, dev] : m_active_devices) active.push_back(dev);
    return active;
}

bool DeviceManager::activate_device(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_active_devices.count(id)) return true;
    auto it = std::find_if(m_available_devices.begin(), m_available_devices.end(),
                           [&id](const AudioDeviceInfo& info) { return info.id == id; });
    if (it == m_available_devices.end()) return false;
    auto dev = std::make_shared<HardwareDevice>(*it);
    if (dev->init()) {
        m_active_devices[id] = dev;
        return true;
    }
    return false;
}

void DeviceManager::deactivate_device(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_lock);
    m_active_devices.erase(id);
}

void DeviceManager::stop_all() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_active_devices.clear();
}
