#include "miniaudio.h"
#include "web_server.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

std::atomic<bool> g_running(true);
AudioMetrics g_metrics;
AudioControls g_controls;
ClientManager g_clientMgr;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) g_running = false;
}

static float lin_to_db(float lin) {
    if (lin <= 0.00001f) return -60.0f;
    return std::clamp(20.0f * std::log10(lin), -60.0f, 0.0f);
}

static std::shared_ptr<WebClientSession> find_client(
    const std::vector<std::shared_ptr<WebClientSession>>& sessions, uint32_t id) {
    for (const auto& session : sessions) if (session->id == id) return session;
    return {};
}

static bool parse_client_endpoint(const std::string& endpoint, bool source, uint32_t& id) {
    const std::string prefix = "client/";
    const std::string suffix = source ? "/capture" : "/playback";
    if (endpoint.size() <= prefix.size() + suffix.size() || endpoint.compare(0, prefix.size(), prefix) != 0 ||
        endpoint.compare(endpoint.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    try {
        id = static_cast<uint32_t>(std::stoul(endpoint.substr(prefix.size(), endpoint.size() - prefix.size() - suffix.size())));
        return id != 0;
    } catch (...) { return false; }
}

void data_callback(ma_device*, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    if (!pOutput || !pInput || frameCount == 0) return;
    auto* in = static_cast<const float*>(pInput);
    auto* out = static_cast<float*>(pOutput);
    const unsigned frames = std::min<unsigned>(frameCount, CM5_MAX_AUDIO_FRAMES);
    thread_local std::array<float, CM5_MAX_AUDIO_FRAMES * CM5_MAX_CHANNELS> local_capture{};
    auto sessions = g_clientMgr.get_active_sessions();

    std::fill(local_capture.begin(), local_capture.begin() + frames * CM5_MAX_CHANNELS, 0.0f);
    std::memset(out, 0, frameCount * CM5_MAX_CHANNELS * sizeof(float));

    float capture_gain[CM5_MAX_CHANNELS];
    float playback_gain[CM5_MAX_CHANNELS];
    float cap_sum[CM5_MAX_CHANNELS] = {};
    float cap_peak[CM5_MAX_CHANNELS] = {};
    float pb_sum[CM5_MAX_CHANNELS] = {};
    float pb_peak[CM5_MAX_CHANNELS] = {};

    for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
        capture_gain[c] = g_controls.capture[c].mute.load(std::memory_order_relaxed) ? 0.0f :
            g_controls.capture[c].gain.load(std::memory_order_relaxed);
        playback_gain[c] = g_controls.playback[c].mute.load(std::memory_order_relaxed) ? 0.0f :
            g_controls.playback[c].gain.load(std::memory_order_relaxed);
    }

    for (unsigned i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
            const float value = in[i * CM5_MAX_CHANNELS + c] * capture_gain[c];
            local_capture[i * CM5_MAX_CHANNELS + c] = value;
            const float magnitude = std::abs(value);
            cap_sum[c] += value * value;
            cap_peak[c] = std::max(cap_peak[c], magnitude);
            if (magnitude >= 0.999f) g_metrics.capture[c].clipped.store(true, std::memory_order_relaxed);
        }
    }

    // Pull each client's source stream once per block. Client input channels are negotiated.
    for (const auto& session : sessions) {
        const unsigned channels = std::clamp(session->input_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
        std::fill(session->input_block.begin(), session->input_block.begin() + frames * CM5_MAX_CHANNELS, 0.0f);
        const size_t bytes = frames * channels * sizeof(float);
        const size_t got = session->incoming_rb.read(session->packet_block.data(), bytes);
        const size_t samples = got / sizeof(float);
        std::copy(session->packet_block.begin(), session->packet_block.begin() + samples, session->input_block.begin());
    }
    for (const auto& session : sessions)
        std::fill(session->output_block.begin(), session->output_block.begin() + frames * CM5_MAX_CHANNELS, 0.0f);

    const auto routes = g_clientMgr.route_snapshot();
    for (const auto& route : *routes) {
        if (!route.enabled || route.source_channel >= CM5_MAX_CHANNELS || route.destination_channel >= CM5_MAX_CHANNELS) continue;
        uint32_t source_id = 0, destination_id = 0;
        const bool source_hw = route.source_endpoint == "hardware/capture";
        const bool destination_hw = route.destination_endpoint == "hardware/playback";
        if (!source_hw && !parse_client_endpoint(route.source_endpoint, true, source_id)) continue;
        if (!destination_hw && !parse_client_endpoint(route.destination_endpoint, false, destination_id)) continue;
        auto source_session = source_hw ? std::shared_ptr<WebClientSession>() : find_client(sessions, source_id);
        auto destination_session = destination_hw ? std::shared_ptr<WebClientSession>() : find_client(sessions, destination_id);
        if ((!source_hw && !source_session) || (!destination_hw && !destination_session)) continue;
        const unsigned source_channels = source_hw ? CM5_MAX_CHANNELS :
            std::clamp(source_session->input_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
        const unsigned destination_channels = destination_hw ? CM5_MAX_CHANNELS :
            std::clamp(destination_session->output_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
        if (route.source_channel >= source_channels || route.destination_channel >= destination_channels) continue;
        for (unsigned i = 0; i < frames; ++i) {
            const float value = source_hw ? local_capture[i * CM5_MAX_CHANNELS + route.source_channel] :
                source_session->input_block[i * CM5_MAX_CHANNELS + route.source_channel];
            if (destination_hw) out[i * CM5_MAX_CHANNELS + route.destination_channel] += value * route.gain;
            else destination_session->output_block[i * CM5_MAX_CHANNELS + route.destination_channel] += value * route.gain;
        }
    }

    for (unsigned i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
            out[i * CM5_MAX_CHANNELS + c] *= playback_gain[c];
            const float magnitude = std::abs(out[i * CM5_MAX_CHANNELS + c]);
            pb_sum[c] += out[i * CM5_MAX_CHANNELS + c] * out[i * CM5_MAX_CHANNELS + c];
            pb_peak[c] = std::max(pb_peak[c], magnitude);
            if (magnitude >= 0.999f) g_metrics.playback[c].clipped.store(true, std::memory_order_relaxed);
        }
    }

    for (const auto& session : sessions) {
        const unsigned channels = std::clamp(session->output_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
        for (unsigned i = 0; i < frames; ++i)
            for (unsigned c = 0; c < channels; ++c)
                session->packet_block[i * channels + c] = session->output_block[i * CM5_MAX_CHANNELS + c];
        session->outgoing_rb.write(session->packet_block.data(), frames * channels * sizeof(float));
    }

    for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
        const float rms = std::sqrt(cap_sum[c] / frames);
        const float peak_db = lin_to_db(cap_peak[c]);
        g_metrics.capture[c].rms_db.store(lin_to_db(rms), std::memory_order_relaxed);
        g_metrics.capture[c].peak_db.store(peak_db, std::memory_order_relaxed);
        const float old_cap = g_metrics.capture[c].peak_hold_db.load(std::memory_order_relaxed);
        g_metrics.capture[c].peak_hold_db.store(std::max(peak_db, std::max(-60.0f, old_cap - 0.15f)), std::memory_order_relaxed);

        const float out_rms = std::sqrt(pb_sum[c] / frames);
        const float out_peak_db = lin_to_db(pb_peak[c]);
        g_metrics.playback[c].rms_db.store(lin_to_db(out_rms), std::memory_order_relaxed);
        g_metrics.playback[c].peak_db.store(out_peak_db, std::memory_order_relaxed);
        const float old_pb = g_metrics.playback[c].peak_hold_db.load(std::memory_order_relaxed);
        g_metrics.playback[c].peak_hold_db.store(std::max(out_peak_db, std::max(-60.0f, old_pb - 0.15f)), std::memory_order_relaxed);
    }
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::cout << "Starting CM5 Audio Network Patchbay..." << std::endl;

    WebServer webServer(g_metrics, g_controls, g_clientMgr, 8182);
    if (!webServer.start()) return -1;

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format = ma_format_f32;
    config.capture.channels = CM5_MAX_CHANNELS;
    config.playback.format = ma_format_f32;
    config.playback.channels = CM5_MAX_CHANNELS;
    config.sampleRate = 48000;
    config.dataCallback = data_callback;

    ma_device device;
    if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
        std::cerr << "Failed to initialize audio device!" << std::endl;
        webServer.stop();
        return -1;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
        std::cerr << "Failed to start audio device!" << std::endl;
        ma_device_uninit(&device);
        webServer.stop();
        return -1;
    }

    std::cout << "Operational: https://192.168.168.172:8182/" << std::endl;
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ma_device_uninit(&device);
    webServer.stop();
    return 0;
}
