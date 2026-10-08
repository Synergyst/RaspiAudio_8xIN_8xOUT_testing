#include "device_manager.h"
#include "constants.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <set>

HardwareDevice::HardwareDevice(const AudioDeviceInfo& info) : m_info(info) {
    ma_uint32 bufferFrames = m_info.sample_rate / 10; 
    if (bufferFrames == 0) bufferFrames = 4800;
    m_input_rb.init(ma_format_f32, m_info.channels, bufferFrames);
    m_output_rb.init(ma_format_f32, m_info.channels, bufferFrames);
}

HardwareDevice::~HardwareDevice() { stop(); }

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
        config.capture.pDeviceID = &m_info.id;
    } else {
        config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = m_info.channels;
        config.playback.pDeviceID = &m_info.id;
    }

    config.sampleRate = m_info.sample_rate;
    config.dataCallback = HardwareDevice::data_callback;
    config.pUserData = this;

    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) return false;

    if (ma_device_init(&context, &config, &m_device) != MA_SUCCESS) {
        ma_context_uninit(&context);
        return false;
    }

    if (ma_device_start(&m_device) != MA_SUCCESS) {
        ma_device_uninit(&m_device);
        ma_context_uninit(&context);
        return false;
    }

    ma_context_uninit(&context);
    return true;
}

void HardwareDevice::data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* device = static_cast<HardwareDevice*>(pDevice->pUserData);
    if (device->m_info.is_capture && pInput) {
        device->m_input_rb.write(pInput, frameCount * device->m_info.channels * sizeof(float));
    } else if (!device->m_info.is_capture && pOutput) {
        size_t bytesNeeded = frameCount * device->m_info.channels * sizeof(float);
        size_t got = device->m_output_rb.read(pOutput, bytesNeeded);
        if (got < bytesNeeded) std::memset((uint8_t*)pOutput + got, 0, bytesNeeded - got);
    }
}

DeviceManager::DeviceManager() {
    if (ma_context_init(NULL, 0, NULL, &m_context) != MA_SUCCESS) {
        std::cerr << "[DeviceManager] Critical: Failed to init global context" << std::endl;
    }
}

DeviceManager::~DeviceManager() { 
    stop_all();
    ma_context_uninit(&m_context);
}

void DeviceManager::enumerate_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_available_devices.clear();

    ma_context_enumerate_devices(&m_context, [](ma_context* pContext, ma_device_type type, const ma_device_info* pDeviceInfo, void* pUserData) -> unsigned int {
        auto* manager = static_cast<DeviceManager*>(pUserData);
        
        static const std::set<std::string> v_list = {
            "null", "lavrate", "samplerate", "speexrate", "jack", 
            "oss", "pulse", "speex", "upmix", "vdownmix"
        };
        if (v_list.count(pDeviceInfo->id.alsa)) return 1;

        AudioDeviceInfo info;
        info.id = pDeviceInfo->id;
        info.name = pDeviceInfo->name;
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
    }, this);
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
                           [&id](const AudioDeviceInfo& info) { return std::string(info.id.alsa) == id; });
    
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
