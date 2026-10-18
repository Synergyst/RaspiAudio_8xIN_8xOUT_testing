#ifndef CM5AUDIO_PLUGIN_PASSTHROUGH_H
#define CM5AUDIO_PLUGIN_PASSTHROUGH_H

#include "audio_processor.h"

class PassthroughProcessor : public AudioProcessor {
public:
    void process(float*, size_t, unsigned) override {}
};

#endif
