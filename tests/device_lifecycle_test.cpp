// Isolated lifecycle coverage: explicitly select null, never probe host audio.
#ifdef NDEBUG
#error "device_lifecycle_test requires assertions enabled"
#endif
#include "device_manager.h"

#include <cassert>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

struct HardwareDeviceTestAccess {
    static ma_device& native(HardwareDevice& device) { return device.m_device; }
    static bool initialized(const HardwareDevice& device) { return device.m_device_initialized; }

    // Model successful native initialization followed by no start (e.g. an early
    // failure). No callback may run here: callback buffers are not prepared.
    static void initialize_without_start(HardwareDevice& device) {
        assert(!device.m_device_initialized);
        auto config = ma_device_config_init(device.m_info.is_capture
            ? ma_device_type_capture : ma_device_type_playback);
        config.capture.format = config.playback.format = ma_format_f32;
        config.capture.channels = config.playback.channels = device.m_info.channels;
        config.capture.pDeviceID = config.playback.pDeviceID = &device.m_info.id;
        config.sampleRate = 48000;
        config.periodSizeInFrames = CM5_BUFFER_FRAMES;
        config.dataCallback = HardwareDevice::data_callback;
        config.pUserData = &device;
        assert(ma_device_init(device.m_context, &config, &device.m_device) == MA_SUCCESS);
        device.m_device_initialized = true;
        assert(ma_device_get_state(&device.m_device) == ma_device_state_stopped);
    }
};

struct DeviceManagerTestAccess {
    static ma_context& context(DeviceManager& manager) { return manager.m_context; }
    static void initialize_null(DeviceManager& manager, void* userData) {
        assert(!manager.m_context_initialized);
        const ma_backend backend = ma_backend_null;
        auto config = ma_context_config_init();
        config.pUserData = userData;
        assert(ma_context_init(&backend, 1, &config, &manager.m_context) == MA_SUCCESS);
        manager.m_context_initialized = true;
        assert(manager.m_context.backend == ma_backend_null);
    }
    static void available(DeviceManager& manager, std::vector<AudioDeviceInfo> devices) {
        manager.m_available_devices = std::move(devices);
    }
    static void adopt(DeviceManager& manager, const std::shared_ptr<HardwareDevice>& device) {
        const auto key = device->get_original_id() +
            (device->get_info().is_capture ? "_capture" : "_playback");
        assert(manager.m_active_devices.emplace(key, device).second);
    }
};

namespace {
using namespace std::chrono_literals;

// Forward to the REAL null backend; count actual native resource destruction,
// including destructor-only paths where the HardwareDevice no longer exists.
struct BackendProbe {
    decltype(ma_backend_callbacks{}.onDeviceUninit) deviceUninit = nullptr;
    decltype(ma_backend_callbacks{}.onContextUninit) contextUninit = nullptr;
    unsigned deviceUninits = 0;
    unsigned contextUninits = 0;

    void attach(DeviceManager& manager) {
        auto& context = DeviceManagerTestAccess::context(manager);
        assert(context.pUserData == this);
        deviceUninit = context.callbacks.onDeviceUninit;
        contextUninit = context.callbacks.onContextUninit;
        assert(deviceUninit && contextUninit);
        context.callbacks.onDeviceUninit = [](ma_device* device) -> ma_result {
            auto& probe = *static_cast<BackendProbe*>(device->pContext->pUserData);
            assert(probe.contextUninits == 0); // Context must still be alive.
            const auto result = probe.deviceUninit(device);
            ++probe.deviceUninits;
            return result;
        };
        context.callbacks.onContextUninit = [](ma_context* context) -> ma_result {
            auto& probe = *static_cast<BackendProbe*>(context->pUserData);
            const auto result = probe.contextUninit(context);
            ++probe.contextUninits;
            return result;
        };
    }
};

AudioDeviceInfo null_info(bool capture, unsigned channels = 2) {
    assert(channels > 0 && channels <= 8);
    AudioDeviceInfo info{}; // nullbackend == 0 is the only valid null device ID.
    info.name = capture ? "Synthetic null capture" : "Synthetic null playback";
    info.is_capture = capture;
    info.channels = channels;
    info.sample_rate = 48000;
    assert(info.id.nullbackend == 0);
    return info;
}

void prepare(DeviceManager& manager, BackendProbe& probe) {
    DeviceManagerTestAccess::initialize_null(manager, &probe);
    probe.attach(manager);
    DeviceManagerTestAccess::available(manager, {null_info(false), null_info(true, 8)});
    assert(manager.initialized());
    assert(manager.get_available_devices().size() == 2);
}

template<class Predicate>
void eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

void assert_running(HardwareDevice& device) {
    assert(HardwareDeviceTestAccess::initialized(device));
    assert(ma_device_is_started(&HardwareDeviceTestAccess::native(device)));
    assert(device.io_buffer.size() == CM5_BUFFER_FRAMES * device.get_info().channels);
    assert(device.get_info().sample_rate == 48000);
    assert(device.get_input_buffer().initialized);
    assert(device.get_output_buffer().initialized);
}

void exercise_callbacks(HardwareDevice& device) {
    assert_running(device);
    if (device.get_info().is_capture) {
        auto& ring = device.get_input_buffer();
        eventually([&] { return ma_pcm_rb_available_read(&ring.rb) > 0; });
        std::vector<float> samples(device.get_info().channels, 1.0f);
        assert(ring.read(samples.data(), samples.size() * sizeof(float)) ==
               samples.size() * sizeof(float));
        for (float sample : samples) assert(sample == 0.0f);
    } else {
        auto& ring = device.get_output_buffer();
        std::vector<float> samples(CM5_BUFFER_FRAMES * device.get_info().channels, 0.25f);
        assert(ring.write(samples.data(), samples.size() * sizeof(float)) ==
               samples.size() * sizeof(float));
        eventually([&] { return ma_pcm_rb_available_read(&ring.rb) == 0; });
    }
}

void never_initialized() {
    DeviceManager manager(false);
    assert(!manager.initialized());
    manager.enumerate_devices();
    assert(manager.get_available_devices().empty());
    assert(!manager.activate_device("hw:1"));
    manager.deactivate_device("hw:1");
    manager.stop_all();
    assert(manager.get_active_devices().empty());
    HardwareDevice device(null_info(false), nullptr, "never-initialized");
    assert(!device.init());
    device.stop();
    device.stop();
    assert(!HardwareDeviceTestAccess::initialized(device));
    // Both destructors must be safe without ever initializing a native object.
}

void native_teardown() {
    BackendProbe probe;
    {
        DeviceManager manager(false);
        prepare(manager, probe);
        for (bool capture : {false, true}) {
            // Initialized but NEVER started: destruction must still uninitialize.
            const auto before = probe.deviceUninits;
            {
                HardwareDevice stopped(null_info(capture),
                    &DeviceManagerTestAccess::context(manager), "native-only");
                HardwareDeviceTestAccess::initialize_without_start(stopped);
                assert(HardwareDeviceTestAccess::initialized(stopped));
                assert(!ma_device_is_started(&HardwareDeviceTestAccess::native(stopped)));
            }
            assert(probe.deviceUninits == before + 1);

            HardwareDevice device(null_info(capture),
                &DeviceManagerTestAccess::context(manager), "started");
            assert(device.init());
            exercise_callbacks(device);
            const auto* storage = device.io_buffer.data();
            assert(device.init()); // Idempotent; never resize live callback storage.
            assert(device.io_buffer.data() == storage);

            // Simulate an independently stopped backend, leaving native ownership
            // intact. stop() must not confuse !is_started with !initialized.
            auto& native = HardwareDeviceTestAccess::native(device);
            assert(ma_device_stop(&native) == MA_SUCCESS);
            assert(ma_device_get_state(&native) == ma_device_state_stopped);
            assert(HardwareDeviceTestAccess::initialized(device));
            assert(ma_device_get_context(&native) != nullptr);
            const auto beforeStop = probe.deviceUninits;
            device.stop();
            assert(!HardwareDeviceTestAccess::initialized(device));
            assert(ma_device_get_state(&native) == ma_device_state_uninitialized);
            assert(ma_device_get_context(&native) == nullptr);
            assert(probe.deviceUninits == beforeStop + 1);
            device.stop();
            assert(probe.deviceUninits == beforeStop + 1);

            assert(device.init()); // Fresh resources after complete teardown.
            exercise_callbacks(device);
            // Leave it started: the destructor must join and uninitialize it.
        }
        assert(probe.deviceUninits == 6);
        assert(probe.contextUninits == 0);
    }
    assert(probe.contextUninits == 1);
}

void uninit_wakes_blocked_data_loop() {
    BackendProbe probe;
    DeviceManager manager(false);
    prepare(manager, probe);
    struct Loop {
        std::mutex lock;
        std::condition_variable changed;
        bool entered = false, wake = false;
        std::atomic<unsigned> wakeCalls{0};
    } loop;
    auto& context = DeviceManagerTestAccess::context(manager);
    // Real null device resources, but a controlled synchronous data loop that
    // can return ONLY after onDeviceDataLoopWakeup. This reproduces the missing
    // backend-wakeup join hang without involving ALSA or physical hardware.
    context.callbacks.onDeviceDataLoop = [](ma_device* native) -> ma_result {
        auto& loop = *static_cast<Loop*>(native->pUserData);
        std::unique_lock<std::mutex> lock(loop.lock);
        loop.entered = true;
        loop.changed.notify_all();
        loop.changed.wait(lock, [&] { return loop.wake; });
        return MA_SUCCESS;
    };
    context.callbacks.onDeviceDataLoopWakeup = [](ma_device* native) -> ma_result {
        auto& loop = *static_cast<Loop*>(native->pUserData);
        // The patch must wake AFTER setting terminal state, not resume a block.
        assert(ma_device_get_state(native) == ma_device_state_uninitialized);
        ++loop.wakeCalls;
        { std::lock_guard<std::mutex> lock(loop.lock); loop.wake = true; }
        loop.changed.notify_all();
        return MA_SUCCESS;
    };
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 48000;
    config.periodSizeInFrames = CM5_BUFFER_FRAMES;
    config.pUserData = &loop;
    ma_device native{};
    assert(ma_device_init(&context, &config, &native) == MA_SUCCESS);
    assert(ma_device_start(&native) == MA_SUCCESS);
    {
        std::unique_lock<std::mutex> lock(loop.lock);
        assert(loop.changed.wait_for(lock, 2s, [&] { return loop.entered; }));
    }
    ma_device_uninit(&native); // Old vendored implementation waits forever here.
    assert(loop.wakeCalls == 1);
    assert(ma_device_get_state(&native) == ma_device_state_uninitialized);
}

void activation_and_exact_ids() {
    BackendProbe probe;
    DeviceManager manager(false);
    prepare(manager, probe);
    // The manager currently compares id.alsa for every backend. For a valid null
    // ID this string is empty. Exercise real public activation using that ID.
    assert(manager.activate_device(""));
    auto active = manager.get_active_devices();
    assert(active.size() == 2);
    assert(manager.activate_device(""));
    assert(manager.get_active_devices().size() == 2);
    for (const auto& device : manager.get_active_devices()) {
        assert(device == active[0] || device == active[1]);
        exercise_callbacks(*device);
    }
    manager.deactivate_device("");
    assert(manager.get_active_devices().empty());
    for (const auto& device : active) exercise_callbacks(*device);
    active.clear(); // External owners MUST be released before manager destruction.
    assert(probe.deviceUninits == 2);

    // ma_device_id is a union: writing "hw:1" into .alsa would INVALIDATE
    // .nullbackend. Seed actual null devices with logical labels instead; this
    // tests exact active-ID/idempotence logic without inventing invalid null IDs.
    std::vector<std::shared_ptr<HardwareDevice>> held;
    for (const std::string id : {"hw:1", "hw:10"}) {
        for (bool capture : {false, true}) {
            auto device = std::make_shared<HardwareDevice>(null_info(capture),
                &DeviceManagerTestAccess::context(manager), id);
            assert(device->init());
            DeviceManagerTestAccess::adopt(manager, device);
            held.push_back(std::move(device));
        }
    }
    assert(manager.activate_device("hw:1"));
    assert(manager.activate_device("hw:10"));
    assert(!manager.activate_device("hw:"));
    assert(manager.get_active_devices().size() == 4);
    for (const auto& device : manager.get_active_devices()) {
        bool same = false;
        for (const auto& owner : held) same = same || owner == device;
        assert(same); // Idempotence preserves identity, not just device count.
    }
    manager.deactivate_device("hw:1");
    manager.deactivate_device("hw:1");
    assert(!manager.activate_device("hw:1"));
    assert(manager.get_active_devices().size() == 2);
    for (const auto& device : manager.get_active_devices()) {
        assert(device->get_original_id() == "hw:10");
        assert_running(*device);
    }
    for (const auto& device : held) exercise_callbacks(*device);
    assert(probe.deviceUninits == 2); // Retired hw:1 still has external owners.
    std::weak_ptr<HardwareDevice> retired = held[0];
    held[0].reset();
    assert(retired.expired());
    held[1].reset();
    assert(probe.deviceUninits == 4);
    assert(manager.initialized());
    manager.stop_all();
    assert(manager.get_active_devices().empty());
    held.clear();
    assert(probe.deviceUninits == 6);
}

void stop_all_retains_external_owners() {
    BackendProbe probe;
    {
        DeviceManager manager(false);
        prepare(manager, probe);
        assert(manager.activate_device(""));
        auto owners = manager.get_active_devices();
        assert(owners.size() == 2);
        std::weak_ptr<HardwareDevice> first = owners[0], second = owners[1];
        manager.stop_all();
        manager.stop_all();
        assert(manager.get_active_devices().empty());
        assert(probe.deviceUninits == 0);
        assert(probe.contextUninits == 0);
        assert(manager.initialized());
        for (const auto& owner : owners) exercise_callbacks(*owner);
        owners[0].reset();
        assert(first.expired());
        assert(!second.expired());
        assert(probe.deviceUninits == 1);
        exercise_callbacks(*owners[1]);
        owners.clear();
        assert(second.expired());
        assert(probe.deviceUninits == 2);
        assert(probe.contextUninits == 0);
        // Manager/context can be reused after all old owners have gone away.
        assert(manager.activate_device(""));
        // No external owners: manager destruction must stop devices FIRST.
    }
    assert(probe.deviceUninits == 4);
    assert(probe.contextUninits == 1);
}
} // namespace

int main() {
    never_initialized();
    native_teardown();
    uninit_wakes_blocked_data_loop();
    activation_and_exact_ids();
    stop_all_retains_external_owners();
    std::cout << "device lifecycle tests passed (null backend only)\n";
}
