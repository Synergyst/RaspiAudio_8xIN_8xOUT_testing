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
ToneControls g_tone;
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

static void update_endpoint_metrics(std::array<ChannelMeter, CM5_MAX_CHANNELS>& metrics,
                                    const std::vector<float>& block, unsigned frames, unsigned channels) {
    channels = std::clamp(channels, 1u, CM5_MAX_CHANNELS);
    for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
        float sum = 0.0f;
        float peak = 0.0f;
        for (unsigned i = 0; i < frames; ++i) {
            const float value = c < channels ? block[i * CM5_MAX_CHANNELS + c] : 0.0f;
            sum += value * value;
            peak = std::max(peak, std::abs(value));
            if (i + 1 == frames) metrics[c].raw_value.store(value, std::memory_order_relaxed);
        }
        metrics[c].raw_peak.store(peak, std::memory_order_relaxed);
        metrics[c].rms_db.store(lin_to_db(std::sqrt(sum / std::max(1u, frames))), std::memory_order_relaxed);
        metrics[c].peak_db.store(lin_to_db(peak), std::memory_order_relaxed);
        const float old_hold = metrics[c].peak_hold_db.load(std::memory_order_relaxed);
        metrics[c].peak_hold_db.store(std::max(lin_to_db(peak), std::max(-60.0f, old_hold - 0.15f)), std::memory_order_relaxed);
        if (peak >= 0.999f) metrics[c].clipped.store(true, std::memory_order_relaxed);
    }
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
    float cap_raw_peak[CM5_MAX_CHANNELS] = {};
    float pb_sum[CM5_MAX_CHANNELS] = {};
    float pb_peak[CM5_MAX_CHANNELS] = {};
    float pb_raw_peak[CM5_MAX_CHANNELS] = {};
    thread_local std::array<float, CM5_MAX_AUDIO_FRAMES * CM5_MAX_CHANNELS> tone_block{};
    const bool tone_enabled = g_tone.enabled.load(std::memory_order_relaxed);
    const float tone_frequency = std::clamp(g_tone.frequency_hz.load(std::memory_order_relaxed), 1.0f, 20000.0f);
    const float tone_amplitude = std::clamp(g_tone.amplitude.load(std::memory_order_relaxed), 0.0f, 1.0f);
    thread_local double tone_phase = 0.0;
    std::fill(tone_block.begin(), tone_block.begin() + frames * CM5_MAX_CHANNELS, 0.0f);
    if (tone_enabled) {
        const double phase_step = 2.0 * 3.14159265358979323846 * tone_frequency / 48000.0;
        for (unsigned i = 0; i < frames; ++i) {
            const float value = tone_amplitude * static_cast<float>(std::sin(tone_phase));
            for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) tone_block[i * CM5_MAX_CHANNELS + c] = value;
            tone_phase += phase_step;
            if (tone_phase >= 2.0 * 3.14159265358979323846) tone_phase = std::fmod(tone_phase, 2.0 * 3.14159265358979323846);
        }
    }

    for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
        capture_gain[c] = g_controls.capture[c].mute.load(std::memory_order_relaxed) ? 0.0f :
            g_controls.capture[c].gain.load(std::memory_order_relaxed);
        playback_gain[c] = g_controls.playback[c].mute.load(std::memory_order_relaxed) ? 0.0f :
            g_controls.playback[c].gain.load(std::memory_order_relaxed);
    }

    for (unsigned i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
            const float raw_value = in[i * CM5_MAX_CHANNELS + c];
            g_metrics.capture[c].raw_value.store(raw_value, std::memory_order_relaxed);
            cap_raw_peak[c] = std::max(cap_raw_peak[c], std::abs(raw_value));
            const float value = raw_value * capture_gain[c];
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
        for (unsigned i = 0; i < frames; ++i) {
            for (unsigned c = 0; c < channels; ++c) {
                const size_t index = i * channels + c;
                session->input_block[i * CM5_MAX_CHANNELS + c] = index < samples ? session->packet_block[index] : 0.0f;
            }
        }
        update_endpoint_metrics(session->input_metrics, session->input_block, frames, channels);
    }
    for (const auto& session : sessions)
        std::fill(session->output_block.begin(), session->output_block.begin() + frames * CM5_MAX_CHANNELS, 0.0f);

    const auto routes = g_clientMgr.route_snapshot();
    for (const auto& route : *routes) {
        if (!route.enabled || route.source_channel >= CM5_MAX_CHANNELS || route.destination_channel >= CM5_MAX_CHANNELS) continue;
        uint32_t source_id = 0, destination_id = 0;
        const bool source_hw = route.source_endpoint == "hardware/capture";
        const bool source_tone = route.source_endpoint == "tone/generator";
        const bool destination_hw = route.destination_endpoint == "hardware/playback";
        if (!source_hw && !source_tone && !parse_client_endpoint(route.source_endpoint, true, source_id)) continue;
        if (!destination_hw && !parse_client_endpoint(route.destination_endpoint, false, destination_id)) continue;
        auto source_session = source_hw || source_tone ? std::shared_ptr<WebClientSession>() : find_client(sessions, source_id);
        auto destination_session = destination_hw ? std::shared_ptr<WebClientSession>() : find_client(sessions, destination_id);
        if ((!source_hw && !source_tone && !source_session) || (!destination_hw && !destination_session)) continue;
        const unsigned source_channels = source_hw || source_tone ? CM5_MAX_CHANNELS :
            std::clamp(source_session->input_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
        const unsigned destination_channels = destination_hw ? CM5_MAX_CHANNELS :
            std::clamp(destination_session->output_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
        if (route.source_channel >= source_channels || route.destination_channel >= destination_channels) continue;
        for (unsigned i = 0; i < frames; ++i) {
            const float value = source_hw ? local_capture[i * CM5_MAX_CHANNELS + route.source_channel] :
                (source_tone ? tone_block[i * CM5_MAX_CHANNELS + route.source_channel] :
                 source_session->input_block[i * CM5_MAX_CHANNELS + route.source_channel]);
            if (destination_hw) out[i * CM5_MAX_CHANNELS + route.destination_channel] += value * route.gain;
            else destination_session->output_block[i * CM5_MAX_CHANNELS + route.destination_channel] += value * route.gain;
        }
    }

    for (unsigned i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
            out[i * CM5_MAX_CHANNELS + c] *= playback_gain[c];
            const float raw_value = out[i * CM5_MAX_CHANNELS + c];
            g_metrics.playback[c].raw_value.store(raw_value, std::memory_order_relaxed);
            pb_raw_peak[c] = std::max(pb_raw_peak[c], std::abs(raw_value));
            const float magnitude = std::abs(raw_value);
            pb_sum[c] += raw_value * raw_value;
            pb_peak[c] = std::max(pb_peak[c], magnitude);
            if (magnitude >= 0.999f) g_metrics.playback[c].clipped.store(true, std::memory_order_relaxed);
        }
    }

    for (const auto& session : sessions) {
        const unsigned channels = std::clamp(session->output_channels.load(std::memory_order_relaxed), 1u, CM5_MAX_CHANNELS);
        for (unsigned i = 0; i < frames; ++i)
            for (unsigned c = 0; c < channels; ++c)
                session->packet_block[i * channels + c] = session->output_block[i * CM5_MAX_CHANNELS + c];
        update_endpoint_metrics(session->output_metrics, session->output_block, frames, channels);
        session->outgoing_rb.write(session->packet_block.data(), frames * channels * sizeof(float));
    }

    for (unsigned c = 0; c < CM5_MAX_CHANNELS; ++c) {
        g_metrics.capture[c].raw_peak.store(cap_raw_peak[c], std::memory_order_relaxed);
        g_metrics.playback[c].raw_peak.store(pb_raw_peak[c], std::memory_order_relaxed);
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

    WebServer webServer(g_metrics, g_controls, g_tone, g_clientMgr, 8182);
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
