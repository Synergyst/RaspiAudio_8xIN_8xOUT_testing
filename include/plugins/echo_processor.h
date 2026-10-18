#ifndef CM5AUDIO_PLUGIN_ECHO_H
#define CM5AUDIO_PLUGIN_ECHO_H

#include "audio_processor.h"
#include <atomic>
#include <vector>

class EchoProcessor : public AudioProcessor {
public:
    EchoProcessor(float delay_ms = 500.0f, float feedback = 0.5f, float mix = 0.5f, float sampleRate = 48000.0f);

    void set_delay_ms(float ms) { m_delayMs = ms; }
    void set_feedback(float fb) { m_feedback = fb; }
    void set_mix(float mix) { m_mix = mix; }
    
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_delayMs;
    std::atomic<float> m_feedback;
    std::atomic<float> m_mix;
    float m_sampleRate;
    
    std::vector<float> m_buffer;
    size_t m_writePos = 0;
};

#endif
