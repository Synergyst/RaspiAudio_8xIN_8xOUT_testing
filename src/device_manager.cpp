#include "device_manager.h"
#include "constants.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <iterator>
#include <set>
#include <vector>

// CONFIGURATION
#define CM5_ENABLE_HW_INFO_DUMP 0

HardwareDevice::HardwareDevice(const AudioDeviceInfo& info, ma_context* context, const std::string& originalId)
    : m_info(info), m_context(context), m_originalId(originalId) {
    // Channel negotiation and all buffer allocation happen in init(), while the
    // initialized device is still stopped. No guessed-channel buffers are needed.
}

HardwareDevice::~HardwareDevice() { stop(); }

void HardwareDevice::stop() {
    if (!m_device_initialized) return;

    // The vendored uninit path wakes synchronous backend data loops after
    // setting the terminal state. Avoid a preliminary stop wait, which can hang
    // when backend and miniaudio running states disagree.
    // A successfully initialized device owns resources even if start failed or
    // the backend has already stopped. Never use is_started as an ownership flag.
    ma_device_uninit(&m_device);
    m_device_initialized = false;
}

bool HardwareDevice::init() {
    if (m_context == nullptr) return false;
    if (m_device_initialized) {
        // Repeated init must not resize buffers underneath an existing callback.
        return ma_device_is_started(&m_device) || ma_device_start(&m_device) == MA_SUCCESS;
    }

    const unsigned requestedChannels = m_info.channels;
    auto attempt_init = [&](const ma_device_id* deviceId) -> bool {
        ma_device_config config = ma_device_config_init(
            m_info.is_capture ? ma_device_type_capture : ma_device_type_playback);
        if (m_info.is_capture) {
            config.capture.format = ma_format_f32;
            config.capture.channels = requestedChannels; // Zero requests native channels.
            config.capture.pDeviceID = deviceId;
        } else {
            config.playback.format = ma_format_f32;
            config.playback.channels = requestedChannels;
            config.playback.pDeviceID = deviceId;
        }
        config.periodSizeInFrames = CM5_BUFFER_FRAMES;
        config.sampleRate = 48000;
        config.dataCallback = HardwareDevice::data_callback;
        config.pUserData = this;
        if (ma_device_init(m_context, &config, &m_device) != MA_SUCCESS) return false;
        m_device_initialized = true;

        // ma_device_init has already negotiated client channels/sample rate, but
        // no audio callback is running until ma_device_start (which may callback
        // before it returns). Prepare EVERYTHING the callback/DSP can access now.
        const unsigned actualChannels = m_info.is_capture ? m_device.capture.channels : m_device.playback.channels;
        if (actualChannels == 0 || actualChannels > MA_MAX_CHANNELS ||
            !m_input_rb.init(ma_format_f32, actualChannels, CM5_BUFFER_FRAMES * 3) ||
            !m_output_rb.init(ma_format_f32, actualChannels, CM5_BUFFER_FRAMES * 3)) {
            stop();
            return false;
        }
        try {
            io_buffer.assign(static_cast<size_t>(CM5_BUFFER_FRAMES) * actualChannels, 0.0f);
        } catch (const std::exception&) {
            stop();
            return false;
        }
        m_info.channels = actualChannels;
        m_info.sample_rate = m_device.sampleRate;
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            stop();
            return false;
        }
        return true;
    };

    if (attempt_init(&m_info.id)) return true;
    // ALSA IDs cannot be used as fallback IDs on JACK, PulseAudio, or null.
    if (m_context->backend != ma_backend_alsa) return false;
    std::string plughwStr = m_originalId;
    if (plughwStr.find("plughw:") != 0) {
        if (plughwStr.find("hw:") == 0) plughwStr = "plughw" + plughwStr.substr(2);
        else if (!plughwStr.empty() && plughwStr[0] == ':') plughwStr = "plughw" + plughwStr;
        else plughwStr = "plughw:" + plughwStr;
    }
    ma_device_id fallbackId{};
    std::strncpy(fallbackId.alsa, plughwStr.c_str(), sizeof(fallbackId.alsa) - 1);
    return attempt_init(&fallbackId);
}

void HardwareDevice::data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    auto* device = static_cast<HardwareDevice*>(pDevice->pUserData);
    const size_t bytesNeeded = static_cast<size_t>(frameCount) * device->m_info.channels * sizeof(float);
    if (device->m_info.is_capture && pInput) {
        device->m_input_rb.write(pInput, bytesNeeded);
    } else if (!device->m_info.is_capture && pOutput) {
        const size_t got = device->m_output_rb.read(pOutput, bytesNeeded);
        if (got < bytesNeeded) std::memset(static_cast<unsigned char*>(pOutput) + got, 0, bytesNeeded - got);
    }
}

DeviceManager::DeviceManager(bool initializeBackend) {
    // Tests can exercise the manager without loading ANY backend or enumerating
    // physical devices. This is not a request to initialize miniaudio's null backend.
    if (!initializeBackend) return;

    // Priority order: JACK (Pro) -> PulseAudio (Desktop) -> ALSA (Hardware Native)
    ma_backend backends[] = {
        ma_backend_jack,
        ma_backend_pulseaudio,
        ma_backend_alsa,
    };

    std::cout << "[DeviceManager] Initializing backend search..." << std::endl;
    std::cout << "[DeviceManager] Enabled backends (priority order):" << std::endl;
    for (size_t i = 0; i < std::size(backends); ++i) {
        std::cout << "  " << i + 1 << ". " << ma_get_backend_name(backends[i]) << std::endl;
    }

    ma_context_config config = ma_context_config_init();
    if (ma_context_init(backends, std::size(backends), &config, &m_context) != MA_SUCCESS) {
        std::cerr << "[DeviceManager] CRITICAL ERROR: Failed to initialize any available audio backend!" << std::endl;
    } else {
        m_context_initialized = true;
        std::cout << "[DeviceManager] SUCCESS: Context initialized." << std::endl;
        std::cout << "[DeviceManager] Active Backend: " << ma_get_backend_name(m_context.backend) << std::endl;
    }
}

DeviceManager::~DeviceManager() {
    // The owner must stop graph/session users and release their device references
    // before destroying this manager: its context must outlive every device.
    stop_all();
    if (m_context_initialized) {
        ma_context_uninit(&m_context);
        m_context_initialized = false;
    }
}

static bool contains_ignore_case(const std::string& haystack, const std::string& needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](unsigned char ch1, unsigned char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
    );
    return it != haystack.end();
}

void DeviceManager::enumerate_devices() {
    std::lock_guard<std::mutex> lock(m_lock);
    m_available_devices.clear();
    if (!m_context_initialized || m_context.backend == ma_backend_null) return;
    ma_context_enumerate_devices(&m_context, [](ma_context* pContext, ma_device_type type, const ma_device_info* pDeviceInfo, void* pUserData) -> unsigned int {
        auto* manager = static_cast<DeviceManager*>(pUserData);
        static const std::set<std::string> v_list = {"null", "lavrate", "samplerate", "speexrate", "jack", "oss", "pulse", "speex", "upmix", "vdownmix"};
        if (pContext->backend == ma_backend_alsa && v_list.count(pDeviceInfo->id.alsa)) return 1;
        AudioDeviceInfo info{};
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

        // Zero indicates native channels if unknown.
        info.channels = maxChannels;
        info.sample_rate = bestSampleRate;

        // Keep the specific overrides for known multi-channel hardware.
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
    active.reserve(m_active_devices.size());
    for (auto const& entry : m_active_devices) active.push_back(entry.second);
    return active;
}

bool DeviceManager::activate_device(const std::string& id) {
    std::vector<AudioDeviceInfo> candidates;
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        if (!m_context_initialized) return false;
        generation = m_activation_generation;
        bool alreadyActive = false;
        for (const auto& entry : m_active_devices) {
            if (entry.second->get_original_id() == id) alreadyActive = true;
        }
        for (const auto& baseInfo : m_available_devices) {
            if (std::string(baseInfo.id.alsa) != id) continue;
            const std::string key = id + (baseInfo.is_capture ? "_capture" : "_playback");
            if (m_active_devices.count(key) == 0) candidates.push_back(baseInfo);
        }
        if (candidates.empty()) return alreadyActive;
    }

    // Initialize/start outside the manager lock. Even failed initialization can
    // need to stop/join a worker. Keep owners outside all lock scopes so failed,
    // cancelled, and duplicate devices are also destroyed without the lock held.
    std::vector<std::shared_ptr<HardwareDevice>> ready;
    for (const auto& info : candidates) {
        auto device = std::make_shared<HardwareDevice>(info, &m_context, id);
        if (device->init()) ready.push_back(std::move(device));
    }

    bool anyActive = false;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        // A removal/stop during initialization wins; don't resurrect a device.
        if (generation == m_activation_generation) {
            for (const auto& device : ready) {
                const std::string key = id + (device->get_info().is_capture ? "_capture" : "_playback");
                m_active_devices.emplace(key, device);
            }
        }
        for (const auto& entry : m_active_devices) {
            if (entry.second->get_original_id() == id) anyActive = true;
        }
    }
    return anyActive;
}

void DeviceManager::deactivate_device(const std::string& id) {
    std::vector<std::shared_ptr<HardwareDevice>> retired;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        retired.reserve(m_active_devices.size());
        ++m_activation_generation;
        for (auto it = m_active_devices.begin(); it != m_active_devices.end();) {
            if (it->second->get_original_id() == id) {
                retired.push_back(std::move(it->second));
                it = m_active_devices.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Last-owner destruction/worker joins happen here, not under m_lock. Graph
    // snapshots may deliberately keep an endpoint alive until they are retired.
}

void DeviceManager::stop_all() {
    std::unordered_map<std::string, std::shared_ptr<HardwareDevice>> retired;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        ++m_activation_generation;
        retired.swap(m_active_devices);
    }
}
