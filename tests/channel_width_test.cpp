// Deterministic graph/callback coverage. No backend, DSP worker, HTTP listener,
// service restart, or real settings file is used by this executable.
#ifdef NDEBUG
#error "channel_width_test requires assertions enabled"
#endif
#include "dsp_engine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

struct HardwareDeviceTestAccess {
    static void prepare(HardwareDevice& device) {
        assert(device.m_context == nullptr && !device.m_device_initialized);
        device.m_device.pUserData = &device;
    }
    static void capture(HardwareDevice& device, const float* input, unsigned frames) {
        assert(device.m_info.is_capture && !device.m_device_initialized);
        HardwareDevice::data_callback(&device.m_device, nullptr, input, frames);
    }
    static void playback(HardwareDevice& device, float* output, unsigned frames) {
        assert(!device.m_info.is_capture && !device.m_device_initialized);
        HardwareDevice::data_callback(&device.m_device, output, nullptr, frames);
    }
};

struct DeviceManagerTestAccess {
    static void adopt(DeviceManager& manager, const std::shared_ptr<HardwareDevice>& device) {
        assert(!manager.initialized());
        std::lock_guard<std::mutex> lock(manager.m_lock);
        const auto key = device->get_original_id() +
            (device->get_info().is_capture ? "_capture" : "_playback");
        assert(manager.m_active_devices.emplace(key, device).second);
    }
};

struct DspEngineTestAccess {
    static void block(DspEngine& engine) {
        assert(!engine.is_running());
        engine.process_audio();
    }
    static unsigned width(const DspEngine& engine, NodeId node) {
        assert(node >= 0 && static_cast<size_t>(node) < engine.m_nodeWidths.size());
        return engine.m_nodeWidths[node];
    }
};

namespace {
static_assert(CM5_BUFFER_FRAMES == 64, "This test exercises callbacks split into 17 + 47 frames");
static_assert(CM5_MAX_CHANNELS == 254 && MA_MAX_CHANNELS == 254, "Review the explicit hardware bound");
static_assert(CM5_MAX_CLIENT_CHANNELS == 8, "Hardware width must not change the client protocol bound");
constexpr unsigned splitFrames = 17;
constexpr float sampleTolerance = 0.0000001f;

struct Rig {
    DeviceManager devices{false};
    AudioMetrics metrics;
    AudioControls controls;
    ToneControls tone;
    ClientManager clients;
    std::unique_ptr<DspEngine> engine = std::make_unique<DspEngine>(devices, clients, metrics, tone);

    Rig() {
        clients.set_device_manager(&devices);
        clients.set_dsp_engine(engine.get());
        engine->get_compressor()->set_threshold(1.0f);
        assert(!devices.initialized() && !engine->is_running());
    }
    ~Rig() {
        engine->stop();
        clients.stop_sessions();
        clients.set_dsp_engine(nullptr);
    }
};

struct Endpoint {
    std::shared_ptr<HardwareDevice> device;
    std::string name;
    unsigned width() const { return device->get_info().channels; }
    NodeId node() const { return device->node_id; }
};

Endpoint endpoint(Rig& rig, const std::string& id, unsigned width, bool capture) {
    assert(width > 0 && width <= CM5_MAX_CHANNELS);
    AudioDeviceInfo info{};
    info.name = "Synthetic " + id;
    info.is_capture = capture;
    info.channels = width;
    info.sample_rate = 48000;
    // Deliberately do NOT initialize ma_context or ma_device, even with null.
    auto device = std::make_shared<HardwareDevice>(info, nullptr, id);
    assert(device->get_input_buffer().init(ma_format_f32, width, CM5_BUFFER_FRAMES * 3));
    assert(device->get_output_buffer().init(ma_format_f32, width, CM5_BUFFER_FRAMES * 3));
    device->io_buffer.assign(static_cast<size_t>(CM5_BUFFER_FRAMES) * width, -3.0f);
    HardwareDeviceTestAccess::prepare(*device);
    DeviceManagerTestAccess::adopt(rig.devices, device);
    const auto name = "hardware/" + id + (capture ? "/capture" : "/playback");
    device->node_id = rig.clients.register_node(name);
    assert(device->node_id != NODE_INVALID);
    return {std::move(device), name};
}

AudioRoute route(const Endpoint& source, unsigned sourceChannel,
                 const Endpoint& destination, unsigned destinationChannel, float gain = 1.0f) {
    AudioRoute result;
    result.source_node = source.node();
    result.source_channel = sourceChannel;
    result.destination_node = destination.node();
    result.destination_channel = destinationChannel;
    result.gain = gain;
    return result;
}

uint32_t add(Rig& rig, const AudioRoute& value) {
    uint32_t id = 0;
    std::string error;
    if (!rig.clients.add_route(value, id, error)) {
        std::cerr << "Unexpected route rejection: " << error << '\n';
        std::abort();
    }
    assert(id != 0);
    return id;
}

void reject(Rig& rig, const AudioRoute& value) {
    const auto before = rig.clients.route_snapshot();
    const auto revision = rig.engine->requested_revision();
    uint32_t id = 0;
    std::string error;
    assert(!rig.clients.add_route(value, id, error));
    assert(!error.empty() && id == 0);
    assert(rig.clients.route_snapshot() == before);
    assert(rig.engine->requested_revision() == revision);
}

std::vector<float> signal(unsigned width, unsigned salt) {
    std::vector<float> result(static_cast<size_t>(CM5_BUFFER_FRAMES) * width);
    for (unsigned f = 0; f < CM5_BUFFER_FRAMES; ++f) {
        for (unsigned ch = 0; ch < width; ++ch) {
            // Exactly representable, frame/channel-distinct values. Salt also
            // distinguishes endpoints so a wrong handler cannot pass by chance.
            const float magnitude = static_cast<float>(1 + ch * 257 + f + salt * 97) / 1048576.0f;
            assert(magnitude > 0.0f && magnitude < 0.1f);
            result[static_cast<size_t>(f) * width + ch] = (f + ch + salt) % 2 ? -magnitude : magnitude;
        }
    }
    return result;
}

void capture_split(const Endpoint& input, const std::vector<float>& samples) {
    assert(samples.size() == static_cast<size_t>(CM5_BUFFER_FRAMES) * input.width());
    HardwareDeviceTestAccess::capture(*input.device, samples.data(), splitFrames);
    HardwareDeviceTestAccess::capture(*input.device, samples.data() + splitFrames * input.width(),
                                     CM5_BUFFER_FRAMES - splitFrames);
    assert(ma_pcm_rb_available_read(&input.device->get_input_buffer().rb) == CM5_BUFFER_FRAMES);
}

std::vector<float> playback_split(const Endpoint& output) {
    constexpr size_t guard = 4;
    constexpr float sentinel = -1234.0f;
    const size_t count = static_cast<size_t>(CM5_BUFFER_FRAMES) * output.width();
    std::vector<float> guarded(count + 2 * guard, sentinel);
    float* samples = guarded.data() + guard;
    HardwareDeviceTestAccess::playback(*output.device, samples, splitFrames);
    HardwareDeviceTestAccess::playback(*output.device, samples + splitFrames * output.width(),
                                      CM5_BUFFER_FRAMES - splitFrames);
    for (size_t i = 0; i < guard; ++i) {
        assert(guarded[i] == sentinel);
        assert(guarded[guard + count + i] == sentinel);
    }
    assert(ma_pcm_rb_available_read(&output.device->get_output_buffer().rb) == 0);
    return {samples, samples + count};
}

void near(float actual, float expected, float tolerance = sampleTolerance) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << "Expected " << expected << ", got " << actual << '\n';
        std::abort();
    }
}

void samples_equal(const Endpoint& output, const std::vector<float>& actual,
                   const std::vector<float>& expected) {
    assert(actual.size() == expected.size());
    assert(actual.size() == static_cast<size_t>(CM5_BUFFER_FRAMES) * output.width());
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!std::isfinite(actual[i]) || std::abs(actual[i] - expected[i]) > sampleTolerance) {
            std::cerr << output.name << ": frame " << i / output.width() << ", channel "
                      << i % output.width() << ": expected " << expected[i] << ", got " << actual[i] << '\n';
            std::abort();
        }
    }
}

std::vector<float> reversed(const std::vector<float>& input, unsigned width) {
    assert(input.size() == static_cast<size_t>(CM5_BUFFER_FRAMES) * width);
    std::vector<float> result(input.size());
    for (unsigned f = 0; f < CM5_BUFFER_FRAMES; ++f)
        for (unsigned ch = 0; ch < width; ++ch)
            result[static_cast<size_t>(f) * width + width - 1 - ch] = input[static_cast<size_t>(f) * width + ch];
    return result;
}

void meters(Rig& rig, const Endpoint& endpoint, const std::vector<float>& expected) {
    assert(expected.size() == static_cast<size_t>(CM5_BUFFER_FRAMES) * endpoint.width());
    std::lock_guard<std::mutex> lock(rig.metrics.lock);
    const auto& banks = endpoint.device->get_info().is_capture ? rig.metrics.capture : rig.metrics.playback;
    const auto bank = banks.find(endpoint.name);
    assert(bank != banks.end() && bank->second.size() == endpoint.width());
    for (unsigned ch = 0; ch < endpoint.width(); ++ch) {
        float sum = 0.0f, peak = 0.0f;
        for (unsigned f = 0; f < CM5_BUFFER_FRAMES; ++f) {
            const float sample = expected[static_cast<size_t>(f) * endpoint.width() + ch];
            sum += sample * sample;
            peak = std::max(peak, std::abs(sample));
        }
        const float rms = std::sqrt(sum / CM5_BUFFER_FRAMES);
        const auto& value = bank->second[ch];
        near(value.raw_value.load(), rms);
        near(value.raw_peak.load(), peak);
        near(value.rms_db.load(), rms > 0.00001f ? 20.0f * std::log10(rms) : -60.0f, 0.0001f);
        near(value.peak_db.load(), peak > 0.00001f ? 20.0f * std::log10(peak) : -60.0f, 0.0001f);
        assert(!value.clipped.load());
    }
}

void width_matrix() {
    Rig rig;
    struct Pair { Endpoint capture, playback; std::vector<float> input; };
    std::vector<Pair> pairs;
    unsigned salt = 0;
    for (unsigned width : {1u, 2u, 3u, 6u, 8u, 12u, 16u, 24u, 32u, 254u}) {
        // Shared IDs for opposite directions and embedded slashes exercise full
        // endpoint identity matching rather than a truncated hardware prefix.
        const auto id = "channel-width/capture/" + std::to_string(width);
        pairs.push_back({endpoint(rig, id, width, true), endpoint(rig, id, width, false), signal(width, ++salt)});
        const auto& pair = pairs.back();
        for (unsigned ch = 0; ch < width; ++ch)
            add(rig, route(pair.capture, ch, pair.playback, width - 1 - ch));
        reject(rig, route(pair.capture, width, pair.playback, 0));
        reject(rig, route(pair.capture, 0, pair.playback, width));
        reject(rig, route(pair.playback, 0, pair.playback, 0));
        reject(rig, route(pair.capture, 0, pair.capture, 0));
    }
    auto invalid = route(pairs.front().capture, CM5_MAX_CHANNELS, pairs.front().playback, 0);
    reject(rig, invalid);
    invalid.source_channel = std::numeric_limits<unsigned>::max();
    reject(rig, invalid);
    invalid = route(pairs.front().capture, 0, pairs.front().playback, CM5_MAX_CHANNELS);
    reject(rig, invalid);
    invalid.source_node = NODE_TONE_GENERATOR;
    invalid.source_channel = 1;
    invalid.destination_channel = 0;
    reject(rig, invalid); // The tone is one channel, not the hardware maximum.
    invalid = route(pairs.front().capture, 0, pairs.front().playback, 0);
    invalid.destination_node = NODE_TONE_GENERATOR;
    reject(rig, invalid);

    for (const auto& pair : pairs) capture_split(pair.capture, pair.input);
    DspEngineTestAccess::block(*rig.engine);
    assert(DspEngineTestAccess::width(*rig.engine, NODE_TONE_GENERATOR) == 1);
    for (const auto& pair : pairs) {
        assert(DspEngineTestAccess::width(*rig.engine, pair.capture.node()) == pair.capture.width());
        assert(DspEngineTestAccess::width(*rig.engine, pair.playback.node()) == pair.playback.width());
        const auto expected = reversed(pair.input, pair.capture.width());
        samples_equal(pair.playback, playback_split(pair.playback), expected);
        meters(rig, pair.capture, pair.input);
        meters(rig, pair.playback, expected);
    }
    {
        std::lock_guard<std::mutex> lock(rig.metrics.lock);
        assert(rig.metrics.capture.size() == pairs.size());
        assert(rig.metrics.playback.size() == pairs.size());
    }

    // Reuse the SAME graph and io_buffer after a nonzero full block. A 17-frame
    // capture read must silence all remaining 47 frames at every width.
    for (auto& pair : pairs) {
        pair.input = signal(pair.capture.width(), ++salt);
        HardwareDeviceTestAccess::capture(*pair.capture.device, pair.input.data(), splitFrames);
        std::fill(pair.input.begin() + splitFrames * pair.capture.width(), pair.input.end(), 0.0f);
    }
    DspEngineTestAccess::block(*rig.engine);
    for (const auto& pair : pairs) {
        const auto expected = reversed(pair.input, pair.capture.width());
        samples_equal(pair.playback, playback_split(pair.playback), expected);
        meters(rig, pair.capture, pair.input);
        meters(rig, pair.playback, expected);
    }

    // An entirely empty next capture block must not replay ANY old channel.
    DspEngineTestAccess::block(*rig.engine);
    for (const auto& pair : pairs) {
        std::vector<float> zero(static_cast<size_t>(CM5_BUFFER_FRAMES) * pair.playback.width(), 0.0f);
        samples_equal(pair.playback, playback_split(pair.playback), zero);
        meters(rig, pair.capture, zero);
        meters(rig, pair.playback, zero);
        // Output underflow must overwrite prefilled destination memory, both
        // when totally empty and when the second 47-frame callback reads short.
        samples_equal(pair.playback, playback_split(pair.playback), zero);
        auto partial = signal(pair.playback.width(), ++salt);
        constexpr unsigned availableFrames = 23;
        const size_t bytes = static_cast<size_t>(availableFrames) * pair.playback.width() * sizeof(float);
        assert(pair.playback.device->get_output_buffer().write(partial.data(), bytes) == bytes);
        std::fill(partial.begin() + availableFrames * pair.playback.width(), partial.end(), 0.0f);
        samples_equal(pair.playback, playback_split(pair.playback), partial);
    }
    assert(!rig.devices.initialized() && !rig.engine->is_running());
}

void same_identity_width_change() {
    Rig rig;
    const auto input = endpoint(rig, "reconnect/source", 12, true);
    auto output = endpoint(rig, "reconnect/sink", 24, false);
    const auto routeId = add(rig, route(input, 11, output, 23));
    const auto originalNode = output.node();
    const auto samples = signal(input.width(), 7);
    capture_split(input, samples);
    DspEngineTestAccess::block(*rig.engine);
    (void)playback_split(output);
    rig.devices.deactivate_device("reconnect/sink");
    output = endpoint(rig, "reconnect/sink", 3, false);
    assert(output.node() == originalNode);
    rig.engine->mark_graph_dirty();
    capture_split(input, samples);
    DspEngineTestAccess::block(*rig.engine);
    samples_equal(output, playback_split(output), std::vector<float>(CM5_BUFFER_FRAMES * 3, 0.0f));
    meters(rig, output, std::vector<float>(CM5_BUFFER_FRAMES * 3, 0.0f));
    assert(rig.clients.get_routes().size() == 1 && rig.clients.get_routes()[0].id == routeId);
    rig.engine->collect_retired_endpoints();
    rig.devices.deactivate_device("reconnect/sink");
    output = endpoint(rig, "reconnect/sink", 24, false);
    rig.engine->mark_graph_dirty();
    capture_split(input, samples);
    DspEngineTestAccess::block(*rig.engine);
    std::vector<float> expected(CM5_BUFFER_FRAMES * 24, 0.0f);
    for (unsigned f = 0; f < CM5_BUFFER_FRAMES; ++f) expected[f * 24 + 23] = samples[f * 12 + 11];
    samples_equal(output, playback_split(output), expected);
    meters(rig, output, expected);
}

void unequal_width_fanout_and_sum() {
    Rig rig;
    const auto a = endpoint(rig, "fan/source", 3, true);
    const auto b = endpoint(rig, "fan/source/extra", 12, true);
    const auto small = endpoint(rig, "fan/sink", 6, false);
    const auto wide = endpoint(rig, "fan/sink/extra", 24, false);
    add(rig, route(a, 2, small, 4, 0.5f));
    add(rig, route(a, 2, wide, 23, 0.25f)); // Fan-out to a different stride.
    add(rig, route(b, 11, small, 4, 0.25f)); // Sum into the existing destination.
    add(rig, route(a, 0, wide, 0));
    add(rig, route(b, 8, wide, 0, 0.5f));
    add(rig, route(b, 0, small, 1, 0.75f));
    const auto inputA = signal(a.width(), 31);
    const auto inputB = signal(b.width(), 43);
    capture_split(a, inputA);
    capture_split(b, inputB);
    DspEngineTestAccess::block(*rig.engine);
    std::vector<float> expectedSmall(CM5_BUFFER_FRAMES * small.width(), 0.0f);
    std::vector<float> expectedWide(CM5_BUFFER_FRAMES * wide.width(), 0.0f);
    for (unsigned f = 0; f < CM5_BUFFER_FRAMES; ++f) {
        expectedSmall[f * small.width() + 4] = inputA[f * a.width() + 2] * 0.5f + inputB[f * b.width() + 11] * 0.25f;
        expectedSmall[f * small.width() + 1] = inputB[f * b.width()] * 0.75f;
        expectedWide[f * wide.width() + 23] = inputA[f * a.width() + 2] * 0.25f;
        expectedWide[f * wide.width()] = inputA[f * a.width()] + inputB[f * b.width() + 8] * 0.5f;
    }
    // Compare all channels, including unrouted ones, not only the sums.
    samples_equal(small, playback_split(small), expectedSmall);
    samples_equal(wide, playback_split(wide), expectedWide);
    meters(rig, a, inputA);
    meters(rig, b, inputB);
    meters(rig, small, expectedSmall);
    meters(rig, wide, expectedWide);
}

struct TemporarySettings {
    std::filesystem::path directory, path;
    bool hadPrevious = false;
    std::string previous;
    TemporarySettings() {
        if (const char* value = std::getenv("CM5AUDIO_SETTINGS")) {
            hadPrevious = true;
            previous = value;
        }
        char pattern[] = "/tmp/cm5audio-channel-width-XXXXXX";
        const char* created = mkdtemp(pattern);
        assert(created);
        directory = created;
        path = directory / "settings.json";
        assert(setenv("CM5AUDIO_SETTINGS", path.c_str(), 1) == 0);
    }
    ~TemporarySettings() {
        if (hadPrevious) assert(setenv("CM5AUDIO_SETTINGS", previous.c_str(), 1) == 0);
        else assert(unsetenv("CM5AUDIO_SETTINGS") == 0);
        std::filesystem::remove_all(directory);
    }
};

void high_channel_persistence() {
    TemporarySettings settings;
    json saved;
    {
        Rig rig;
        const auto input = endpoint(rig, "persist/capture/nested", 254, true);
        const auto wide = endpoint(rig, "persist/playback/nested", 254, false);
        const auto narrow = endpoint(rig, "persist/playback", 32, false);
        add(rig, route(input, 253, wide, 253, 0.5f));
        add(rig, route(input, 31, wide, 17, 0.25f));
        add(rig, route(input, 17, narrow, 31, 0.75f));
        assert(rig.clients.save_settings(rig.controls, rig.tone));
        std::ifstream file(settings.path);
        file >> saved;
        assert(saved["activated_devices"].empty()); // Loading cannot open hardware.
        assert(saved["routes"].size() == 3);
        assert(saved["routes"][0]["source_channel"] == 253);
        assert(saved["routes"][0]["destination_channel"] == 253);
        assert(saved["routes"][1]["destination_channel"] == 17);
        assert(saved["routes"][2]["destination_channel"] == 31);
    }
    {
        Rig rig;
        // Reverse registration order: persisted endpoint strings, not process-
        // local numeric node IDs, must reconnect the high-channel routes.
        const auto narrow = endpoint(rig, "persist/playback", 32, false);
        const auto wide = endpoint(rig, "persist/playback/nested", 254, false);
        const auto input = endpoint(rig, "persist/capture/nested", 254, true);
        assert(rig.clients.load_settings(rig.controls, rig.tone));
        const auto restored = rig.clients.get_routes();
        assert(restored.size() == 3);
        for (size_t i = 0; i < restored.size(); ++i) {
            assert(restored[i].id == saved["routes"][i]["id"].get<uint32_t>());
            assert(rig.clients.get_endpoint_from_id(restored[i].source_node) == saved["routes"][i]["source"]);
            assert(rig.clients.get_endpoint_from_id(restored[i].destination_node) == saved["routes"][i]["destination"]);
            assert(restored[i].source_channel == saved["routes"][i]["source_channel"].get<unsigned>());
            assert(restored[i].destination_channel == saved["routes"][i]["destination_channel"].get<unsigned>());
            near(restored[i].gain, saved["routes"][i]["gain"].get<float>());
            assert(restored[i].enabled);
        }
        const auto samples = signal(input.width(), 59);
        capture_split(input, samples);
        DspEngineTestAccess::block(*rig.engine);
        std::vector<float> expectedWide(CM5_BUFFER_FRAMES * wide.width(), 0.0f);
        std::vector<float> expectedNarrow(CM5_BUFFER_FRAMES * narrow.width(), 0.0f);
        for (unsigned f = 0; f < CM5_BUFFER_FRAMES; ++f) {
            expectedWide[f * wide.width() + 253] = samples[f * input.width() + 253] * 0.5f;
            expectedWide[f * wide.width() + 17] = samples[f * input.width() + 31] * 0.25f;
            expectedNarrow[f * narrow.width() + 31] = samples[f * input.width() + 17] * 0.75f;
        }
        samples_equal(wide, playback_split(wide), expectedWide);
        samples_equal(narrow, playback_split(narrow), expectedNarrow);
        meters(rig, input, samples);
        meters(rig, wide, expectedWide);
        meters(rig, narrow, expectedNarrow);
        assert(rig.clients.save_settings(rig.controls, rig.tone));
        std::ifstream file(settings.path);
        const auto savedAgain = json::parse(file);
        assert(savedAgain["routes"] == saved["routes"]);
        assert(!rig.devices.initialized() && !rig.engine->is_running());
    }
}

void explicit_ring_width_limit() {
    PcmRingBuffer ring;
    assert(ring.init(ma_format_f32, 254, CM5_BUFFER_FRAMES));
    assert(ring.initialized && ring.channels == 254);
    float sample = 0.01f;
    for (unsigned invalid : {0u, 255u, 256u, std::numeric_limits<unsigned>::max()}) {
        assert(!ring.init(ma_format_f32, invalid, CM5_BUFFER_FRAMES));
        assert(!ring.initialized && ring.total_frames == 0);
        assert(ring.read(&sample, sizeof(sample)) == 0);
        assert(ring.write(&sample, sizeof(sample)) == 0);
    }
}
} // namespace

int main() {
    explicit_ring_width_limit();
    width_matrix();
    unequal_width_fanout_and_sum();
    same_identity_width_change();
    high_channel_persistence();
    std::cout << "PASS: widths 1,2,3,6,8,12,16,24,32,254; split hardware callbacks; reverse routes; "
                 "meters; channel/direction rejection; unequal-width fan-out/sum; short reads; "
                 "underflow; ring width limit; high-channel persistence (no audio backend or service)\n";
}
