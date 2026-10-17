#include "device_manager.h"
#include "constants.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <set>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

// CONFIGURATION
#define CM5_ENABLE_PROBES 0
#define CM5_ENABLE_HW_INFO_DUMP 1

HardwareDevice::HardwareDevice(const AudioDeviceInfo& info, ma_context* context, const std::string& originalId)
    : m_info(info), m_context(context), m_originalId(originalId) {

    // MATHEMATICAL FIX: Ring buffer is always 3x the processing block size (safe headroom)
    ma_uint32 bufferFrames = CM5_BUFFER_FRAMES * 3;

    // If channels is 0, we use a safe default for the buffers until init() corrects it
    unsigned channels = (m_info.channels > 0) ? m_info.channels : 2;
    m_input_rb.init(ma_format_f32, channels, bufferFrames);
    m_output_rb.init(ma_format_f32, channels, bufferFrames);
    io_buffer.resize(CM5_BUFFER_FRAMES * channels);
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
            // FIX: If channels is 0, let miniaudio use native hardware count
            config.capture.channels = (m_info.channels > 0) ? m_info.channels : 0;
            config.capture.pDeviceID = deviceId;
            config.periodSizeInFrames = CM5_BUFFER_FRAMES;
        } else {
            config = ma_device_config_init(ma_device_type_playback);
            config.playback.format = ma_format_f32;
            // FIX: If channels is 0, let miniaudio use native hardware count
            config.playback.channels = (m_info.channels > 0) ? m_info.channels : 0;
            config.playback.pDeviceID = deviceId;
            config.periodSizeInFrames = CM5_BUFFER_FRAMES;
        }
        config.sampleRate = 48000;
        config.dataCallback = HardwareDevice::data_callback;
        config.pUserData = this;
        if (m_context == nullptr) return false;
        if (ma_device_init(m_context, &config, &m_device) != MA_SUCCESS) return false;
        if (ma_device_start(&m_device) != MA_SUCCESS) { ma_device_uninit(&m_device); return false; }

        // UPDATE: Now that the device is started, we know the ACTUAL channel count
        m_info.channels = m_info.is_capture ? m_device.capture.channels : m_device.playback.channels;

        // Re-size buffers if the native count differs from our initial guess
        unsigned actual = m_info.channels;
        m_input_rb.init(ma_format_f32, actual, (CM5_BUFFER_FRAMES * 3));
        m_output_rb.init(ma_format_f32, actual, (CM5_BUFFER_FRAMES * 3));
        io_buffer.resize(CM5_BUFFER_FRAMES * actual);

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
        device->m_input_rb.write(pInput, frameCount * device->m_info.channels * sizeof(float));
    } else if (!device->m_info.is_capture && pOutput) {
        size_t bytesNeeded = frameCount * device->m_info.channels * sizeof(float);
        size_t got = device->m_output_rb.read(pOutput, bytesNeeded);
        if (got < bytesNeeded) std::memset((uint8_t*)pOutput + got, 0, bytesNeeded - got);
    }
}

ma_backend backends[] = {
    ma_backend_pulseaudio,
    ma_backend_alsa,
};

DeviceManager::DeviceManager() {
    if (ma_context_init(backends, std::size(backends), NULL, &m_context) != MA_SUCCESS) std::cerr << "[DeviceManager] Critical: Failed to init global context" << std::endl;
}

DeviceManager::~DeviceManager() {
    stop_all();
    ma_context_uninit(&m_context);
}

bool contains_ignore_case(const std::string& haystack, const std::string& needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return it != haystack.end();
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

        // FIX: Remove fallback to 2. Use 0 to indicate "Native" if unknown.
        info.channels = maxChannels;
        info.sample_rate = bestSampleRate;

        // Keep the specific overrides for known multi-channel hardware
        if (info.channels < 8) {
            if (contains_ignore_case(info.name, "8x") || contains_ignore_case(info.name, "dac8") || contains_ignore_case(info.name, "hifiberry")) {
                info.channels = 8;
            }
        }

        #if CM5_ENABLE_HW_INFO_DUMP
        std::cout << "[HW PROBE] Name: " << info.name
                  << " | ID: " << pDeviceInfo->id.alsa
                  << " | Type: " << (info.is_capture ? "Capture" : "Playback")
                  << " | NativeFormats: " << pDeviceInfo->nativeDataFormatCount
                  << " | MaxChannels: " << maxChannels
                  << " | FinalChannels: " << info.channels << (info.channels == 0 ? " (Native)" : "") << std::endl;
        #endif

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
