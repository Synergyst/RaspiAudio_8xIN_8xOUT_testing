#include "plugins/passthrough.h"
#include "processor_registry.h"
#include <memory>

void register_passthrough_processor(ProcessorRegistry& registry) {
    registry.register_processor({"passthrough", "Passthrough",
        "Passes audio through unchanged.", {},
        [](const nlohmann::json&) { return std::make_unique<PassthroughProcessor>(); }});
}
