#ifndef CM5AUDIO_PLUGIN_GAIN_H
#define CM5AUDIO_PLUGIN_GAIN_H

#include "audio_processor.h"
#include <atomic>

class GainProcessor : public AudioProcessor {
public:
    explicit GainProcessor(float gain = 1.0f) : m_gain(gain) {}
    void set_gain(float gain) { m_gain = gain; }
    float get_gain() const { return m_gain; }
    void process(float* buffer, size_t frames, unsigned channels) override;

private:
    std::atomic<float> m_gain;
};

#endif
