#include "processor_registry.h"

void register_gain_processor(ProcessorRegistry& registry);
void register_compressor_processor(ProcessorRegistry& registry);
void register_passthrough_processor(ProcessorRegistry& registry);

void register_builtin_processors(ProcessorRegistry& registry) {
    register_passthrough_processor(registry);
    register_gain_processor(registry);
    register_compressor_processor(registry);
}
