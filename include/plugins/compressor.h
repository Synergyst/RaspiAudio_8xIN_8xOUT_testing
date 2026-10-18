#ifndef CM5AUDIO_PLUGIN_COMPRESSOR_H
#define CM5AUDIO_PLUGIN_COMPRESSOR_H

#include "audio_processor.h"
#include <atomic>

// Basic hard-knee, linear-amplitude compressor with smoothed gain reduction.
class CompressorProcessor : public AudioProcessor {
public:
    CompressorProcessor(float threshold = 0.5f, float ratio = 4.0f,
                        float attack = 0.01f, float release = 0.1f,
                        float sampleRate = 48000.0f);

    void set_threshold(float threshold) { m_threshold = threshold; }
    void set_ratio(float ratio) { m_ratio = ratio; }
    float get_threshold() const { return m_threshold; }
    float get_ratio() const { return m_ratio; }
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_ratio;
    float m_attack;
    float m_release;
    float m_sampleRate;
    float m_attackCoef;
    float m_releaseCoef;
    float m_envelope = 1.0f;
};

#endif
