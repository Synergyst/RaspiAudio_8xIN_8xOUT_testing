#ifndef CM5AUDIO_PLUGIN_REVERB_H
#define CM5AUDIO_PLUGIN_REVERB_H

#include "audio_processor.h"
#include <atomic>
#include <vector>

class ReverbProcessor : public AudioProcessor {
public:
    ReverbProcessor(float room_size = 0.5f, float damping = 0.5f, float mix = 0.3f, float sampleRate = 48000.0f);

    void set_room_size(float size) { m_roomSize = size; }
    void set_damping(float damping) { m_damping = damping; }
    void set_mix(float mix) { m_mix = mix; }
    
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    struct CombFilter {
        std::vector<float> buffer;
        size_t pos = 0;
        float feedback = 0.0f;
        float lastOut = 0.0f;
        
        float process(float input, float damping) {
            float output = buffer[pos];
            float filtered = input + (output * feedback);
            // Simple one-pole lowpass for damping
            lastOut = output * damping + filtered * (1.0f - damping);
            buffer[pos] = lastOut;
            pos = (pos + 1) % buffer.size();
            return output;
        }
    };

    struct AllPassFilter {
        std::vector<float> buffer;
        size_t pos = 0;
        float gain = 0.5f;

        float process(float input) {
            float bufOut = buffer[pos];
            float output = -gain * input + bufOut;
            buffer[pos] = input + (gain * bufOut);
            pos = (pos + 1) % buffer.size();
            return output;
        }
    };

    std::atomic<float> m_roomSize;
    std::atomic<float> m_damping;
    std::atomic<float> m_mix;
    float m_sampleRate;

    std::vector<CombFilter> m_combs;
    std::vector<AllPassFilter> m_allpasses;
};

#endif
