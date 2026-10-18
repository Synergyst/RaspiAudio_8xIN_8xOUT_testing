#ifndef CM5AUDIO_PLUGIN_EQ_H
#define CM5AUDIO_PLUGIN_EQ_H

#include "audio_processor.h"
#include <atomic>
#include <deque>
#include <vector>

class EqProcessor : public AudioProcessor {
public:
    struct Band {
        std::atomic<float> freq;
        std::atomic<float> gain;
        std::atomic<float> q;
        float z1 = 0.0f;
        float z2 = 0.0f;
        float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f, b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;

        void updateCoeffs(float sampleRate);
        
        // Custom copy constructor because atomics are not copyable
        Band(const Band& other) : freq(other.freq.load()), gain(other.gain.load()), q(other.q.load()) {}
        Band() = default;
    };

    EqProcessor(float sampleRate = 48000.0f);

    void process(float* buffer, size_t frames, unsigned channels) override;

    // Public so factory can initialize
    std::deque<Band> m_bands;

private:
    float m_sampleRate;
};

#endif
