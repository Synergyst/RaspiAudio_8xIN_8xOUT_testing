#include "plugins/eq_processor.h"
#include "processor_registry.h"
#include <cmath>
#include <memory>
#include <nlohmann/json.hpp>

void EqProcessor::Band::updateCoeffs(float sampleRate) {
    float f0 = freq.load(std::memory_order_relaxed);
    float g = gain.load(std::memory_order_relaxed);
    float q_val = q.load(std::memory_order_relaxed);
    
    float A = std::pow(10.0f, g / 40.0f);
    float omega = 2.0f * 3.14159265f * f0 / sampleRate;
    float sn = std::sin(omega);
    float cs = std::cos(omega);
    float alpha = sn / (2.0f * q_val);

    b0 = 1.0f + alpha * A;
    b1 = -2.0f * cs;
    b2 = 1.0f - alpha * A;
    a0 = 1.0f + alpha / A;
    a1 = -2.0f * cs;
    a2 = 1.0f - alpha / A;

    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
}

EqProcessor::EqProcessor(float sampleRate) : m_sampleRate(sampleRate) {
    for (int i = 0; i < 24; ++i) {
        Band b;
        // Logarithmic spread from 20Hz to 20kHz
        float f = 20.0f * std::pow(20000.0f / 20.0f, i / 23.0f);
        b.freq = f;
        b.gain = 0.0f;
        b.q = 1.0f;
        b.updateCoeffs(sampleRate);
        m_bands.push_back(b);
    }
}

void EqProcessor::process(float* buffer, size_t frames, unsigned channels) {
    for (auto& band : m_bands) {
        band.updateCoeffs(m_sampleRate);
        float b0 = band.b0, b1 = band.b1, b2 = band.b2;
        float a1 = band.a1, a2 = band.a2;

        for (size_t i = 0; i < frames * channels; ++i) {
            float in = buffer[i];
            float out = b0 * in + band.z1;
            band.z1 = b1 * in - a1 * out + band.z2;
            band.z2 = b2 * in - a2 * out;
            buffer[i] = out;
        }
    }
}

void register_eq_processor(ProcessorRegistry& registry) {
    std::vector<ProcessorParameter> params;
    for (int i = 1; i <= 24; ++i) {
        std::string prefix = "b" + std::to_string(i) + "_";
        
        // Logarithmic default frequency from 20 to 20000
        float freqVal = 20.0f * std::pow(20000.0f / 20.0f, (i-1) / 23.0f);
        
        // User reported that defaults must be integers
        int defaultFreq = static_cast<int>(std::round(freqVal));
        
        params.push_back({prefix + "freq", "Band " + std::to_string(i) + " Freq", "number", (double)defaultFreq, 20.0, 20000.0, 1.0, "Hz", {}});
        params.push_back({prefix + "gain", "Band " + std::to_string(i) + " Gain", "number", 0.0, -24.0, 24.0, 0.1, "dB", {}});
        params.push_back({prefix + "q", "Band " + std::to_string(i) + " Q", "number", 1.0, 0.1, 10.0, 0.1, "", {}});
    }

    registry.register_processor({"eq24", "24-Band EQ",
        "Professional 24-band parametric EQ using Biquad peaking filters.",
        params,
        [](const nlohmann::json& params) {
            auto eq = std::make_unique<EqProcessor>();
            for (int i = 0; i < 24; ++i) {
                std::string prefix = "b" + std::to_string(i+1) + "_";
                if (params.contains(prefix + "freq")) eq->m_bands[i].freq = params[prefix + "freq"];
                if (params.contains(prefix + "gain")) eq->m_bands[i].gain = params[prefix + "gain"];
                if (params.contains(prefix + "q")) eq->m_bands[i].q = params[prefix + "q"];
            }
            return eq;
        }});
}
