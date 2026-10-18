#include "plugins/lush_reverb_processor.h"
#include "processor_registry.h"
#include <memory>
#include <cmath>

LushReverbProcessor::LushReverbProcessor(float decay, float diffusion, float mix, float sampleRate)
    : m_decay(decay), m_diffusion(diffusion), m_mix(mix), m_sampleRate(sampleRate) {

    float combTimes[] = {0.0297f, 0.0371f, 0.0411f, 0.0437f, 0.0471f, 0.0511f, 0.0537f, 0.0571f};
    for (int i = 0; i < 8; ++i) {
        size_t size = static_cast<size_t>(combTimes[i] * sampleRate);
        if (size < 2) size = 2;
        ModulatedComb mc;
        mc.buffer.resize(size, 0.0f);
        mc.lfoInc = (0.1f + (static_cast<float>(i) * 0.05f)) * 2.0f * 3.14159265f / sampleRate;
        m_combs.push_back(mc);
    }

    float apTimes[] = {0.005f, 0.0017f, 0.008f, 0.003f};
    for (float t : apTimes) {
        size_t size = static_cast<size_t>(t * sampleRate);
        if (size < 2) size = 2;
        AllPass ap;
        ap.buffer.resize(size, 0.0f);
        m_allpasses.push_back(ap);
    }
}

void LushReverbProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float decay = m_decay.load(std::memory_order_relaxed);
    const float diff = m_diffusion.load(std::memory_order_relaxed);
    const float mix = m_mix.load(std::memory_order_relaxed);
    
    // Hoist feedback and modulation calculations outside frame/comb loops
    const float combFeedback = 0.6f + (0.3f * decay);
    const float modDepth = 0.5f * diff; // Modulate within safe sample bounds

    for (size_t f = 0; f < frames; ++f) {
        float monoInput = 0.0f;
        for (unsigned c = 0; c < channels; ++c) {
            monoInput += buffer[f * channels + c];
        }
        monoInput /= (channels > 0 ? static_cast<float>(channels) : 1.0f);

        float combOut = 0.0f;
        for (auto& mc : m_combs) {
            mc.feedback = combFeedback;
            combOut += mc.process(monoInput, modDepth);
        }

        float reverbOut = combOut * 0.125f;
        for (auto& ap : m_allpasses) {
            reverbOut = ap.process(reverbOut);
        }

        for (unsigned c = 0; c < channels; ++c) {
            size_t idx = f * channels + c;
            float dry = buffer[idx];
            float wet = reverbOut;
            if (std::isnan(dry) || std::isinf(dry)) dry = 0.0f;
            if (std::isnan(wet) || std::isinf(wet)) wet = 0.0f;
            buffer[idx] = (dry * (1.0f - mix)) + (wet * mix);
        }
    }
}

void register_lush_reverb_processor(ProcessorRegistry& registry) {
    registry.register_processor({
        "lush_reverb", 
        "Lush Reverb",
        "Modulated high-density reverb for smooth, professional spaces.",
        {
            {"decay", "Decay", "number", 0.5, 0.1, 1.0, 0.01, "", {}},
            {"diffusion", "Diffusion", "number", 0.5, 0.0, 1.0, 0.01, "", {}},
            {"mix", "Mix", "number", 0.3, 0.0, 1.0, 0.01, "", {}}
        },
        [](const nlohmann::json& params) {
            return std::make_unique<LushReverbProcessor>(
                params.value("decay", 0.5f),
                params.value("diffusion", 0.5f),
                params.value("mix", 0.3f)
            );
        }
    });
}
