#ifndef DSP_ENGINE_H
#define DSP_ENGINE_H

#include "device_manager.h"
#include "web_server.h"
#include "constants.h"
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>
#include <cmath>
#include <string>
#include <unordered_map>
#include <map>
#include <set>
#include <optional>
#include <functional>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>

class ClientManager;

// Base class for built-in C++ effects. Factories and metadata never run on the audio thread.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void process(float* buffer, size_t frames, unsigned channels) = 0;
};

class PassthroughProcessor : public AudioProcessor {
public:
    void process(float*, size_t, unsigned) override {}
};

class GainProcessor : public AudioProcessor {
public:
    GainProcessor(float gain = 1.0f) : m_gain(gain) {}
    void set_gain(float gain) { m_gain = gain; }
    float get_gain() const { return m_gain; }
    void process(float* buffer, size_t frames, unsigned channels) override {
        const float gain = m_gain.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames * channels; ++i) buffer[i] *= gain;
    }
private:
    std::atomic<float> m_gain;
};

// Basic hard-knee, linear-amplitude compressor with smoothed gain reduction.
class CompressorProcessor : public AudioProcessor {
public:
    CompressorProcessor(float threshold = 0.5f, float ratio = 4.0f, float attack = 0.01f, float release = 0.1f, float sampleRate = 48000.0f)
        : m_threshold(threshold), m_ratio(ratio), m_attack(attack), m_release(release), m_sampleRate(sampleRate) {
        m_attackCoef = std::exp(-1.0f / (m_sampleRate * m_attack));
        m_releaseCoef = std::exp(-1.0f / (m_sampleRate * m_release));
    }

    void set_threshold(float t) { m_threshold = t; }
    void set_ratio(float r) { m_ratio = r; }
    float get_threshold() const { return m_threshold; }
    float get_ratio() const { return m_ratio; }

    void process(float* buffer, size_t frames, unsigned channels) override {
        for (size_t i = 0; i < frames * channels; ++i) {
            float input = buffer[i];
            float absInput = std::abs(input);
            float targetGain = 1.0f;
            if (absInput > m_threshold) {
                targetGain = m_threshold + (absInput - m_threshold) / m_ratio;
                targetGain /= absInput;
            }
            float coef = (targetGain < m_envelope) ? m_attackCoef : m_releaseCoef;
            m_envelope = coef * m_envelope + (1.0f - coef) * targetGain;
            buffer[i] *= m_envelope;
        }
    }

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_ratio;
    float m_attack, m_release, m_sampleRate, m_attackCoef, m_releaseCoef;
    float m_envelope = 1.0f;
};

struct ProcessorChoice {
    std::string label;
    nlohmann::json value;
};

// Supported UI types: number, integer, boolean, string, enum. Units are display-only:
// the factory always receives the units declared here (no implicit dB conversion).
struct ProcessorParameter {
    std::string id;
    std::string name;
    std::string type;
    nlohmann::json default_value;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> step;
    std::string unit;
    std::vector<ProcessorChoice> choices;
};

struct ProcessorDescriptor {
    using Factory = std::function<std::unique_ptr<AudioProcessor>(const nlohmann::json&)>;
    std::string type;
    std::string name;
    std::string description;
    std::vector<ProcessorParameter> parameters;
    Factory factory;
};

// Register C++ descriptors/factories before starting the server. The same definitions
// drive discovery, default filling, validation, settings restoration and construction.
// Registration is startup-only; discovery/create are read-only and safe concurrently.
class ProcessorRegistry {
public:
    static ProcessorRegistry& instance() {
        static ProcessorRegistry registry;
        return registry;
    }

    bool register_processor(ProcessorDescriptor descriptor) {
        if (descriptor.type.empty() || descriptor.name.empty() || !descriptor.factory)
            throw std::invalid_argument("Processor registration requires type, name and factory");
        if (m_processors.count(descriptor.type))
            throw std::invalid_argument("Duplicate processor type: " + descriptor.type);
        std::set<std::string> ids;
        for (const auto& p : descriptor.parameters) {
            if (p.id.empty() || !ids.insert(p.id).second)
                throw std::invalid_argument("Empty or duplicate processor parameter: " + p.id);
            if (p.type != "number" && p.type != "integer" && p.type != "boolean" &&
                p.type != "string" && p.type != "enum")
                throw std::invalid_argument("Unsupported parameter type: " + p.type);
            const bool numeric = p.type == "number" || p.type == "integer";
            if ((p.min || p.max || p.step) && !numeric)
                throw std::invalid_argument("Numeric constraints on nonnumeric parameter: " + p.id);
            if ((p.min && !std::isfinite(*p.min)) || (p.max && !std::isfinite(*p.max)) ||
                (p.step && (!std::isfinite(*p.step) || *p.step <= 0)) ||
                (p.min && p.max && *p.min > *p.max))
                throw std::invalid_argument("Invalid numeric constraints: " + p.id);
            if (p.type == "enum") {
                if (p.choices.empty()) throw std::invalid_argument("Empty enum choices: " + p.id);
                for (size_t i = 0; i < p.choices.size(); ++i) {
                    const auto& value = p.choices[i].value;
                    if (value.is_null() || !value.is_primitive() ||
                        (value.is_number() && !std::isfinite(value.get<double>())))
                        throw std::invalid_argument("Enum choices must be finite scalar values: " + p.id);
                    for (size_t j = 0; j < i; ++j)
                        if (p.choices[j].value == value)
                            throw std::invalid_argument("Duplicate enum choice: " + p.id);
                }
            } else if (!p.choices.empty()) {
                throw std::invalid_argument("Choices require enum type: " + p.id);
            }
            validate_value(p, p.default_value);
        }
        const auto type = descriptor.type;
        m_processors.emplace(type, std::move(descriptor));
        return true;
    }

    std::unique_ptr<AudioProcessor> create(const std::string& type, const nlohmann::json& params,
                                           nlohmann::json& normalized) const {
        const auto it = m_processors.find(type);
        if (it == m_processors.end()) throw std::invalid_argument("Unknown processor type: " + type);
        if (!params.is_object()) throw std::invalid_argument("Processor params must be an object");
        const auto& descriptor = it->second;
        normalized = nlohmann::json::object();
        for (const auto& p : descriptor.parameters) {
            const auto found = params.find(p.id);
            const auto& value = found == params.end() ? p.default_value : *found;
            validate_value(p, value);
            normalized[p.id] = value;
        }
        for (auto itParam = params.begin(); itParam != params.end(); ++itParam)
            if (!normalized.contains(itParam.key()))
                throw std::invalid_argument("Unknown parameter for " + type + ": " + itParam.key());
        auto processor = descriptor.factory(normalized);
        if (!processor) throw std::runtime_error("Processor factory returned null: " + type);
        return processor;
    }

    nlohmann::json discovery() const {
        nlohmann::json processors = nlohmann::json::array();
        for (const auto& item : m_processors) {
            const auto& d = item.second;
            nlohmann::json parameters = nlohmann::json::array();
            nlohmann::json defaults = nlohmann::json::object();
            for (const auto& p : d.parameters) {
                nlohmann::json j = {{"id", p.id}, {"name", p.name}, {"type", p.type}, {"default", p.default_value}};
                if (p.min) j["min"] = *p.min;
                if (p.max) j["max"] = *p.max;
                if (p.step) j["step"] = *p.step;
                if (!p.unit.empty()) j["unit"] = p.unit;
                if (!p.choices.empty()) {
                    j["choices"] = nlohmann::json::array();
                    for (const auto& c : p.choices) j["choices"].push_back({{"label", c.label}, {"value", c.value}});
                }
                parameters.push_back(std::move(j));
                defaults[p.id] = p.default_value;
            }
            processors.push_back({{"type", d.type}, {"name", d.name}, {"description", d.description},
                                  {"defaults", defaults}, {"parameters", parameters}});
        }
        return {{"schema_version", 1}, {"max_chain_length", CM5_MAX_PROCESSORS_PER_ROUTE}, {"processors", processors}};
    }

private:
    static void validate_value(const ProcessorParameter& p, const nlohmann::json& value) {
        bool valid = false;
        if (p.type == "number" || p.type == "integer") {
            if (value.is_number()) {
                const double number = value.get<double>();
                valid = std::isfinite(number) && (!p.min || number >= *p.min) &&
                        (!p.max || number <= *p.max) &&
                        (p.type != "integer" || std::trunc(number) == number);
            }
        } else if (p.type == "boolean") valid = value.is_boolean();
        else if (p.type == "string") valid = value.is_string();
        else if (p.type == "enum") {
            for (const auto& c : p.choices) if (c.value == value) valid = true;
        }
        if (!valid) throw std::invalid_argument("Invalid value for parameter '" + p.id + "' (" + p.type + ")");
    }

    ProcessorRegistry() {
        register_processor({"passthrough", "Passthrough", "Passes audio through unchanged.", {},
            [](const nlohmann::json&) { return std::make_unique<PassthroughProcessor>(); }});
        register_processor({"gain", "Gain", "Linear amplitude multiplier (1 = unity, 0 = silence).",
            {{"gain", "Gain", "number", 1.0, 0.0, 8.0, 0.01, "linear", {}}},
            [](const nlohmann::json& p) { return std::make_unique<GainProcessor>(p.at("gain").get<float>()); }});
        register_processor({"compressor", "Compressor", "Hard-knee compressor. Threshold is linear amplitude, not dBFS.",
            {{"threshold", "Threshold", "number", 0.5, 0.0, 1.0, 0.01, "linear", {}},
             {"ratio", "Ratio", "number", 4.0, 1.0, 100.0, 0.1, ":1", {}},
             {"attack", "Attack", "number", 0.01, 0.0001, 10.0, 0.0001, "s", {}},
             {"release", "Release", "number", 0.1, 0.0001, 10.0, 0.0001, "s", {}}},
            [](const nlohmann::json& p) {
                return std::make_unique<CompressorProcessor>(p.at("threshold").get<float>(), p.at("ratio").get<float>(),
                                                             p.at("attack").get<float>(), p.at("release").get<float>());
            }});
    }

    std::map<std::string, ProcessorDescriptor> m_processors;
};

struct NodeHandler {
    enum class Type { HARDWARE_CAPTURE, HARDWARE_PLAYBACK, CLIENT_CAPTURE, CLIENT_PLAYBACK, TONE, VIRTUAL };
    Type type;
    void* ptr;
    unsigned channels;
    std::string endpointId;
    bool active = false;
};

class DspEngine {
public:
    DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics, ToneControls& tone);
    ~DspEngine();

    void start();
    void stop();
    bool is_running() const { return m_running.load(std::memory_order_acquire); }

    // Publish state before requesting adoption. A revision is acknowledged only
    // after a complete audio block has finished using the adopted state.
    uint64_t mark_graph_dirty();
    uint64_t request_adoption() { return mark_graph_dirty(); }
    uint64_t requested_revision() const { return m_requestedRevision.load(std::memory_order_acquire); }
    uint64_t adopted_revision() const { return m_adoptedRevision.load(std::memory_order_acquire); }
    bool wait_for_revision(uint64_t revision, std::chrono::milliseconds timeout);

    // Control-thread only. Final device/session destruction must never run on
    // the audio worker; stop() also drains all retained endpoint ownership.
    void collect_retired_endpoints();

    GainProcessor* get_gain_processor() { return m_gainProc.get(); }
    CompressorProcessor* get_compressor() { return m_compProc.get(); }
    bool supports_route_dsp(uint32_t routeId) const { return routeId > 0 && routeId < MAX_ROUTES; }

    // Construction, publication and reclamation of processors are control-side.
    // Chain, gain and bypass state are one indivisible audio-side publication.
    void update_route_dsp(uint32_t routeId, std::vector<std::unique_ptr<AudioProcessor>> newChain,
                          float gain, bool active);

private:
    friend struct DspEngineTestAccess;

    void processing_loop();
    void process_audio();
    void rebuild_graph();
    void update_meters(float* buffer, unsigned channels, const std::string& endpointId, bool isCapture);

    DeviceManager& m_deviceManager;
    ClientManager& m_clientManager;
    AudioMetrics& m_metrics;
    ToneControls& m_tone;
    std::thread m_workerThread;
    std::atomic<bool> m_running{false};

    static constexpr size_t MAX_NODES = 256;
    static constexpr size_t MAX_ROUTES = 1024;

    std::vector<float> m_nodeBufferPool;
    std::vector<float> m_feedbackPool;
    std::vector<float> m_nextFeedbackPool;
    std::vector<NodeId> m_executionOrder;
    int m_nodePosition[MAX_NODES];
    NodeHandler m_nodeHandlers[MAX_NODES];
    std::atomic<bool> m_graphDirty{true};
    std::atomic<uint64_t> m_requestedRevision{1};
    std::atomic<uint64_t> m_adoptedRevision{0};
    uint64_t m_builtRevision = 0; // Audio-thread only (or after join).
    std::mutex m_revisionWaitLock;
    std::condition_variable m_revisionChanged;

    struct EndpointOwners {
        std::vector<std::shared_ptr<HardwareDevice>> hardware;
        std::vector<std::shared_ptr<WebClientSession>> sessions;
        bool empty() const { return hardware.empty() && sessions.empty(); }
    };
    EndpointOwners m_endpointOwners;
    std::mutex m_retiredEndpointsLock;
    std::vector<EndpointOwners> m_retiredEndpointOwners;

    // Installed only by the friend test accessor while the worker is stopped.
    // Runs after taking graph snapshots, before publishing the rebuilt graph.
    std::function<void()> m_rebuildGraphTestHook;

    std::shared_ptr<const std::vector<AudioRoute>> m_currentRoutes;
    std::vector<const AudioRoute*> m_adjList[MAX_NODES];

    struct RouteDsp {
        struct Chain {
            std::unique_ptr<AudioProcessor> processors[CM5_MAX_PROCESSORS_PER_ROUTE];
            int length = 0;
            float gain = 1.0f;
            bool active = false;
        };
        // Three slots guarantee a writable slot even while the audio reader holds an
        // older publication. Only the control thread creates/destroys processors.
        Chain chains[3];
        std::atomic<int> current_idx{0};
        std::atomic<int> reader_idx{-1};
        std::mutex writer_lock;
    };
    RouteDsp m_routeDsp[MAX_ROUTES];
    std::vector<float> m_routeScratchBuffer;

    std::unique_ptr<GainProcessor> m_gainProc;
    std::unique_ptr<CompressorProcessor> m_compProc;

    ma_uint32 m_engineSampleRate = 48000;
    double m_tonePhase = 0.0;

    inline float* get_buffer(NodeId id) {
        if (id < 0 || static_cast<size_t>(id) >= MAX_NODES) return nullptr;
        return &m_nodeBufferPool[static_cast<size_t>(id) * CM5_BUFFER_FRAMES * CM5_INTERNAL_WIDTH];
    }
    inline float* get_feedback_buffer(NodeId id) {
        if (id < 0 || static_cast<size_t>(id) >= MAX_NODES) return nullptr;
        return &m_feedbackPool[static_cast<size_t>(id) * CM5_BUFFER_FRAMES * CM5_INTERNAL_WIDTH];
    }
    inline float* get_next_feedback_buffer(NodeId id) {
        if (id < 0 || static_cast<size_t>(id) >= MAX_NODES) return nullptr;
        return &m_nextFeedbackPool[static_cast<size_t>(id) * CM5_BUFFER_FRAMES * CM5_INTERNAL_WIDTH];
    }
};

#endif
