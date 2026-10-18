#include "plugins/echo_processor.h"
#include "processor_registry.h"
#include <memory>
#include <cmath>

EchoProcessor::EchoProcessor(float delay_ms, float feedback, float mix, float sampleRate)
    : m_delayMs(delay_ms), m_feedback(feedback), m_mix(mix), m_sampleRate(sampleRate) {
    // Max delay 2 seconds
    m_buffer.resize(static_cast<size_t>(2.0f * sampleRate), 0.0f);
}

void EchoProcessor::process(float* buffer, size_t frames, unsigned channels) {
    const float delayMs = m_delayMs.load(std::memory_order_relaxed);
    const float feedback = m_feedback.load(std::memory_order_relaxed);
    const float mix = m_mix.load(std::memory_order_relaxed);
    
    size_t delaySamples = static_cast<size_t>((delayMs / 1000.0f) * m_sampleRate);
    if (delaySamples >= m_buffer.size()) delaySamples = m_buffer.size() - 1;

    for (size_t f = 0; f < frames; ++f) {
        for (unsigned c = 0; c < channels; ++c) {
            size_t idx = f * channels + c;
            float input = buffer[idx];
            
            // Read from delay line
            long long readPos = static_cast<long long>(m_writePos) - delaySamples;
            if (readPos < 0) readPos += m_buffer.size();
            float delayedSample = m_buffer[readPos];
            
            // Write to delay line (input + feedback)
            m_buffer[m_writePos] = input + (delayedSample * feedback);
            
            // Mix output
            buffer[idx] = (input * (1.0f - mix)) + (delayedSample * mix);
        }
        m_writePos = (m_writePos + 1) % m_buffer.size();
    }
}

void register_echo_processor(ProcessorRegistry& registry) {
    registry.register_processor({"echo", "Echo",
        "Digital delay with feedback and mix.",
        {{"delay_ms", "Delay (ms)", "number", 500.0, 10.0, 2000.0, 1.0, "ms", {}},
         {"feedback", "Feedback", "number", 0.5, 0.0, 0.95, 0.01, "", {}},
         {"mix", "Mix", "number", 0.5, 0.0, 1.0, 0.01, "", {}}},
        [](const nlohmann::json& params) {
            return std::make_unique<EchoProcessor>(
                params.value("delay_ms", 500.0f),
                params.value("feedback", 0.5f),
                params.value("mix", 0.5f)
            );
        }});
}
