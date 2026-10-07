#include "device_manager.h"
#include "constants.h"
#include <iostream>
#include <algorithm>
#include <cstring>

HardwareDevice::HardwareDevice(const AudioDeviceInfo& info) : m_info(info) {
    // Use a reasonable buffer size based on sample rate (approx 100ms)
    ma_uint32 bufferFrames = m_info.sample_rate / 10; 
    if (bufferFrames == 0) bufferFrames = 4800;

    m_input_rb.init(ma_format_f32, m_info.channels, bufferFrames);
    m_output_rb.init(ma_format_f32, m_info.channels, bufferFrames);
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

    if (ma_device_init(m_info.id.c_str(), &config, &m_device) != MA_SUCCESS) {
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

    // List of virtual/plugin backends to ignore.
    // We make this static so the non-capturing lambda can access it.
    static const std::vector<std::string> virtual_backends = {
        "null", "lavrate", "samplerate", "speexrate", "jack", 
        "oss", "pulse", "speex", "upmix", "vdownmix"
    };

    // MUST be a non-capturing lambda to be convertible to a function pointer.
    auto enum_callback = [](ma_context* pContext, ma_device_type type, const ma_device_info* pDeviceInfo, void* pUserData) -> unsigned int {
        auto* manager = static_cast<DeviceManager*>(pUserData);
        
        // Extract the ALSA ID
        std::string idStr = pDeviceInfo->id.alsa;
        
        // Filter out virtual devices using the static list
        // We need to access the static list here.
        // Since it's not captured, we can't use 'virtual_backends' directly if it were local.
        // But since I'll define it as a static constant, it works.
        // However, to be absolutely safe with lambdas and static vectors, 
        // I'll just use a local static inside the lambda or a helper.
        
        // Actually, let's just use a simple check for the most common virtual IDs 
        // or use a static const array for maximum compatibility.
        
        static const char* v_list[] = { "null", "lavrate", "samplerate", "speexrate", "jack", "oss", "pulse", "speex", "upmix", "vdownmix" };
        for (const char* v : v_list) {
            if (idStr == v) return 1;
        }

        AudioDeviceInfo info;
        // pDeviceInfo->name is a char array, it is never NULL.
        info.name = pDeviceInfo->name;
        info.id = idStr;
        info.is_capture = (type == ma_device_type_capture);
        
        if (pDeviceInfo->nativeDataFormatCount > 0) {
            info.sample_rate = pDeviceInfo->nativeDataFormats[0].sampleRate;
            info.channels = pDeviceInfo->nativeDataFormats[0].channels;
        } else {
            info.sample_rate = 48000;
            info.channels = 2;
        }

        manager->m_available_devices.push_back(info);
        return 1; 
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

bool DeviceManager::activate_device(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (m_active_devices.count(name)) return true;
    auto it = std::find_if(m_available_devices.begin(), m_available_devices.end(),
                           [&name](const AudioDeviceInfo& info) { return info.name == name; });
    if (it == m_available_devices.end()) return false;
    auto dev = std::make_shared<HardwareDevice>(*it);
    if (dev->init()) {
        m_active_devices[name] = dev;
        return true;
    }
    return false;
}

void DeviceManager::deactivate_device(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_lock);
    m_active_devices.erase(name);
}

void DeviceManager::stop_all() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_active_devices.clear();
}
