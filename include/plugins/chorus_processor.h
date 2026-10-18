#ifndef CM5AUDIO_PLUGIN_CHORUS_H
#define CM5AUDIO_PLUGIN_CHORUS_H

#include "audio_processor.h"
#include <atomic>
#include <vector>

class ChorusProcessor : public AudioProcessor {
public:
    ChorusProcessor(float rate_hz = 1.5f, float depth = 0.3f, float mix = 0.5f, float sampleRate = 48000.0f);

    void set_rate_hz(float rate) { m_rateHz = rate; }
    void set_depth(float depth) { m_depth = depth; }
    void set_mix(float mix) { m_mix = mix; }
    
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_rateHz;
    std::atomic<float> m_depth;
    std::atomic<float> m_mix;
    float m_sampleRate;
    
    std::vector<float> m_buffer;
    size_t m_writePos = 0;
    float m_lfoPhase = 0.0f;
};

#endif
