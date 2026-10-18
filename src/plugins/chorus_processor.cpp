#include "plugins/chorus_processor.h"
#include "processor_registry.h"
#include <memory>
#include <cmath>

ChorusProcessor::ChorusProcessor(float rate_hz, float depth, float mix, float sampleRate)
    : m_rateHz(rate_hz), m_depth(depth), m_mix(mix), m_sampleRate(sampleRate) {
    // Max delay for chorus is usually small, 50ms is plenty
    m_buffer.resize(static_cast<size_t>(0.05f * sampleRate), 0.0f);
}

void ChorusProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float rate = m_rateHz.load(std::memory_order_relaxed);
    const float depth = m_depth.load(std::memory_order_relaxed);
    const float mix = m_mix.load(std::memory_order_relaxed);
    
    const float lfoInc = (2.0f * M_PI * rate) / m_sampleRate;
    const float baseDelay = 0.020f * m_sampleRate; // 20ms base delay
    const float modAmount = 0.005f * m_sampleRate; // 5ms modulation range

    for (size_t f = 0; f < frames; ++f) {
        // Calculate current LFO offset
        float offset = (std::sin(m_lfoPhase) + 1.0f) * 0.5f * depth * modAmount;
        float readPos = static_cast<float>(m_writePos) - (baseDelay + offset);
        
        while (readPos < 0) readPos += m_buffer.size();
        
        // Linear Interpolation
        size_t i0 = static_cast<size_t>(readPos);
        size_t i1 = (i0 + 1) % m_buffer.size();
        float frac = readPos - i0;
        float delayedSample = m_buffer[i0] * (1.0f - frac) + m_buffer[i1] * frac;

        for (unsigned c = 0; c < channels; ++c) {
            size_t idx = f * channels + c;
            float input = buffer[idx];
            
            m_buffer[m_writePos] = input; // Simplified for mono buffer
            buffer[idx] = (input * (1.0f - mix)) + (delayedSample * mix);
        }
        
        m_writePos = (m_writePos + 1) % m_buffer.size();
        m_lfoPhase += lfoInc;
        if (m_lfoPhase > 2.0f * M_PI) m_lfoPhase -= 2.0f * M_PI;
    }
}

void register_chorus_processor(ProcessorRegistry& registry) {
    registry.register_processor({"chorus", "Chorus",
        "LFO-modulated delay for shimmering unison effect.",
        {{"rate_hz", "Rate (Hz)", "number", 1.5, 0.1, 5.0, 0.1, "Hz", {}},
         {"depth", "Depth", "number", 0.3, 0.0, 1.0, 0.01, "", {}},
         {"mix", "Mix", "number", 0.5, 0.0, 1.0, 0.01, "", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<ChorusProcessor>(
                params.value("rate_hz", 1.5f),
                params.value("depth", 0.3f),
                params.value("mix", 0.5f)
            );
        }});
}
