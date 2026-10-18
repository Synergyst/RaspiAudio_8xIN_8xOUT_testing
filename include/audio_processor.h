#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <cstddef>

// Native processors execute on the audio thread. Implementations must not
// allocate, lock, perform I/O, or throw from process().
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void process(float* buffer, size_t frames, unsigned channels) = 0;
};

#endif
