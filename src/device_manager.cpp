#include "device_manager.h"
#include "constants.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <set>
#include <atomic>
#include <thread>
#include <chrono>

// CONFIGURATION
#define CM5_RB_SIZE_MS 120           // Total ring buffer size in milliseconds
#define CM5_ENABLE_PROBES 0         // Set to 1 to enable console signal tracing

#if CM5_ENABLE_PROBES
std::atomic<float> g_callbackPeak{0.0f};
std::atomic<bool> g_monitorRunning{false};
std::atomic<uint32_t> g_callbackCount{0};
#endif

HardwareDevice::HardwareDevice(const AudioDeviceInfo& info, ma_context* context, const std::string& originalId) 
    : m_info(info), m_context(context), m_originalId(originalId) {
    
    ma_uint32 bufferFrames = (m_info.sample_rate * CM5_RB_SIZE_MS) / 1000;
    
    m_input_rb.init(ma_format_f32, m_info.channels, bufferFrames);
    m_output_rb.init(ma_format_f32, m_info.channels, bufferFrames);
    
    io_buffer.resize(CM5_BUFFER_FRAMES * m_info.channels);
}

HardwareDevice::~HardwareDevice() { stop(); }

void HardwareDevice::stop() {
    if (ma_device_is_started(&m_device)) ma_device_uninit(&m_device);
}

bool HardwareDevice::init() {
    auto attempt_init = [&](const ma_device_id* deviceId) -> bool {
        ma_device_config config;
        if (m_info.is_capture) {
            config = ma_device_config_init(ma_device_type_capture);
            config.capture.format = ma_format_f32;
            config.capture.channels = m_info.channels;
            config.capture.pDeviceID = deviceId;
        } else {
            config = ma_device_config_init(ma_device_type_playback);
            config.playback.format = ma_format_f32;
            config.playback.channels = m_info.channels;
            config.playback.pDeviceID = deviceId;
        }
        config.sampleRate = 48000; 
        config.dataCallback = HardwareDevice::data_callback;
        config.pUserData = this;
        if (m_context == nullptr) return false;
        if (ma_device_init(m_context, &config, &m_device) != MA_SUCCESS) return false;
        if (ma_device_start(&m_device) != MA_SUCCESS) { ma_device_uninit(&m_device); return false; }
        return true;
    };
    if (attempt_init(&m_info.id)) return true;
    std::string plughwStr = m_originalId;
    if (plughwStr.find("hw:") == 0) plughwStr = "plughw" + plughwStr.substr(2);
    else if (!plughwStr.empty() && plughwStr[0] == ':') plughwStr = "plughw" + plughwStr;
    else plughwStr = "plughw:" + plughwStr;
    ma_device_id fallbackId;
    std::memset(&fallbackId, 0, sizeof(ma_device_id));
    std::strncpy(fallbackId.alsa, plughwStr.c_str(), sizeof(fallbackId.alsa) - 1);
    if (attempt_init(&fallbackId)) return true;
    return false;
}

void HardwareDevice::data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* device = static_cast<HardwareDevice*>(pDevice->pUserData);
    if (device->m_info.is_capture && pInput) {
        size_t written = device->m_input_rb.write(pInput, frameCount * device->m_info.channels * sizeof(float));
        #if CM5_ENABLE_PROBES
        if (g_callbackCount.fetch_add(1) % 100 == 0) {
            std::cout << "[PROBE A] Capture Callback " << device->m_originalId << " wrote " << written << " bytes" << std::endl;
        }
        #endif
    } else if (!device->m_info.is_capture && pOutput) {
        size_t bytesNeeded = frameCount * device->m_info.channels * sizeof(float);
        size_t got = device->m_output_rb.read(pOutput, bytesNeeded);
        
        #if CM5_ENABLE_PROBES
        if (got > 0) {
            float* samples = static_cast<float*>(pOutput);
            size_t numSamples = got / sizeof(float);
            float localPeak = 0.0f;
            for (size_t i = 0; i < numSamples; ++i) {
                float absS = std::abs(samples[i]);
                if (absS > localPeak) localPeak = absS;
            }
            float current = g_callbackPeak.load();
            if (localPeak > current) g_callbackPeak.store(localPeak);
        }
        if (g_callbackCount.fetch_add(1) % 100 == 0) {
            std::cout << "[PROBE D] Playback Callback " << device->m_originalId << " read " << got << "/" << bytesNeeded << " bytes" << std::endl;
        }
        #endif
        if (got < bytesNeeded) std::memset((uint8_t*)pOutput + got, 0, bytesNeeded - got);
    }
}

DeviceManager::DeviceManager() {
    if (ma_context_init(NULL, 0, NULL, &m_context) != MA_SUCCESS) std::cerr << "[DeviceManager] Critical: Failed to init global context" << std::endl;
    #if CM5_ENABLE_PROBES
    g_monitorRunning = true;
    std::thread([]() {
        while (g_monitorRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            float peak = g_callbackPeak.exchange(0.0f);
            std::cout << "[DEBUG] HW Callback Peak: " << peak << (peak > 0 ? " <--- AUDIO FLOWING" : " <--- SILENCE") << std::endl;
        }
    }).detach();
    #endif
}

DeviceManager::~DeviceManager() { 
    #if CM5_ENABLE_PROBES
    g_monitorRunning = false;
    #endif
    stop_all();
    ma_context_uninit(&m_context);
}

void DeviceManager::enumerate_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_available_devices.clear();
    ma_context_enumerate_devices(&m_context, [](ma_context* pContext, ma_device_type type, const ma_device_info* pDeviceInfo, void* pUserData) -> unsigned int {
        auto* manager = static_cast<DeviceManager*>(pUserData);
        static const std::set<std::string> v_list = {"null", "lavrate", "samplerate", "speexrate", "jack", "oss", "pulse", "speex", "upmix", "vdownmix"};
        if (v_list.count(pDeviceInfo->id.alsa)) return 1;
        AudioDeviceInfo info;
        info.id = pDeviceInfo->id;
        info.name = pDeviceInfo->name;
        info.is_capture = (type == ma_device_type_capture);
        unsigned maxChannels = 0;
        ma_uint32 bestSampleRate = 48000;
        for (unsigned i = 0; i < pDeviceInfo->nativeDataFormatCount; ++i) {
            if (pDeviceInfo->nativeDataFormats[i].channels > maxChannels) {
                maxChannels = pDeviceInfo->nativeDataFormats[i].channels;
                bestSampleRate = pDeviceInfo->nativeDataFormats[i].sampleRate;
            }
        }
        info.channels = (maxChannels > 0) ? maxChannels : 2;
        info.sample_rate = bestSampleRate;
        if (info.channels < 8 && (info.name.find("8x") != std::string::npos || info.name.find("DAC8") != std::string::npos)) info.channels = 8;
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
    for (auto const& [key, dev] : m_active_devices) active.push_back(dev);
    return active;
}

bool DeviceManager::activate_device(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_lock);
    bool anyActivated = false;
    for (const auto& baseInfo : m_available_devices) {
        if (std::string(baseInfo.id.alsa) == id) {
            std::string uniqueKey = id + (baseInfo.is_capture ? "_capture" : "_playback");
            if (m_active_devices.count(uniqueKey)) continue;
            auto dev = std::make_shared<HardwareDevice>(baseInfo, &m_context, id);
            if (dev->init()) {
                m_active_devices[uniqueKey] = dev;
                anyActivated = true;
            }
        }
    }
    return anyActivated;
}

void DeviceManager::deactivate_device(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto it = m_active_devices.begin(); it != m_active_devices.end(); ) {
        if (it->first.find(id) == 0) it = m_active_devices.erase(it);
        else ++it;
    }
}

void DeviceManager::stop_all() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_active_devices.clear();
}
