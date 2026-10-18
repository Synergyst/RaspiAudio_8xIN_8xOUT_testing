#include "plugins/reverb_processor.h"
#include "processor_registry.h"
#include <memory>
#include <cmath>

ReverbProcessor::ReverbProcessor(float room_size, float damping, float mix, float sampleRate)
    : m_roomSize(room_size), m_damping(damping), m_mix(mix), m_sampleRate(sampleRate) {
    
    // Typical comb filter delay times for a good reverb (scaled by room_size)
    float combTimes[] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
    for (float t : combTimes) {
        size_t size = static_cast<size_t>(t * sampleRate);
        CombFilter cf;
        cf.buffer.resize(size, 0.0f);
        cf.feedback = 0.8f; // Base feedback
        m_combs.push_back(cf);
    }
    
    // All-pass filter delay times
    float apTimes[] = {0.005f, 0.0017f};
    for (float t : apTimes) {
        size_t size = static_cast<size_t>(t * sampleRate);
        AllPassFilter apf;
        apf.buffer.resize(size, 0.0f);
        m_allpasses.push_back(apf);
    }
}

void ReverbProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float roomSize = m_roomSize.load(std::memory_order_relaxed);
    const float damping = m_damping.load(std::memory_order_relaxed);
    const float mix = m_mix.load(std::memory_order_relaxed);

    for (size_t f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < channels; ++c) {
            size_t idx = f * channels + c;
            float input = buffer[idx];
            float combOut = 0.0f;

            // Parallel Comb Filters
            for (auto& cf : m_combs) {
                // Adjust feedback based on room size
                cf.feedback = 0.7f + (0.2f * roomSize);
                combOut += cf.process(input, damping);
            }

            // Series All-Pass Filters
            float reverbOut = combOut * 0.25f;
            for (auto& ap : m_allpasses) {
                reverbOut = ap.process(reverbOut);
            }

            buffer[idx] = (input * (1.0f - mix)) + (reverbOut * mix);
        }
    }
}

void register_reverb_processor(ProcessorRegistry& registry) {
    registry.register_processor({"reverb", "Reverb",
        "Algorithmic Schroeder reverb simulating space.",
        {{"room_size", "Room Size", "number", 0.5, 0.1, 1.0, 0.01, "", {}},
         {"damping", "Damping", "number", 0.5, 0.0, 1.0, 0.01, "", {}},
         {"mix", "Mix", "number", 0.3, 0.0, 1.0, 0.01, "", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<ReverbProcessor>(
                params.value("room_size", 0.5f),
                params.value("damping", 0.5f),
                params.value("mix", 0.3f)
            );
        }});
}
