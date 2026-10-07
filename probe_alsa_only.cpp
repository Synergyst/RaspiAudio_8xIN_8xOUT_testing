#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <iostream>
#include <vector>
#include <string>
#include <set>

std::string device_id_to_string(const ma_device_id* id) {
    return std::string(id->alsa);
}

int main() {
    ma_context context;
    if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
        std::cerr << "Failed to initialize context" << std::endl;
        return 1;
    }

    // List of known virtual/plugin backends to ignore
    std::set<std::string> virtual_backends = {
        "null", "lavrate", "samplerate", "speexrate", "jack", 
        "oss", "pulse", "speex", "upmix", "vdownmix"
    };

    std::cout << "Scanning for ACTUAL ALSA Hardware..." << std::endl;
    std::cout << "----------------------------------------------------------------------" << std::endl;
    std::cout << "Name | ID | Type" << std::endl;
    std::cout << "----------------------------------------------------------------------" << std::endl;

    ma_context_enumerate_devices(&context, [](ma_context* pContext, ma_device_type type, const ma_device_info* pDeviceInfo, void* pUserData) -> unsigned int {
        auto* virtuals = static_cast<std::set<std::string>*>(pUserData);
        
        std::string id = device_id_to_string(&pDeviceInfo->id);
        
        // FILTER: If the ID is in our virtual list, skip it.
        if (virtuals->count(id)) {
            return 1; // Continue to next device
        }

        std::string typeStr = (type == ma_device_type_capture) ? "Input" : "Output";
        std::string name = pDeviceInfo->name ? pDeviceInfo->name : "Unknown";

        std::cout << name << " | " << id << " | " << typeStr << std::endl;
        
        return 1; 
    }, &virtual_backends);

    ma_context_uninit(&context);
    return 0;
}
