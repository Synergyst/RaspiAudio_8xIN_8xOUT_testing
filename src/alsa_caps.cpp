#include <alsa/asoundlib.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

// Trim whitespace
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

// Read card name from sysfs/proc
std::string get_card_name(int card_num) {
    std::ifstream f("/proc/asound/card" + std::to_string(card_num) + "/id");
    std::string name;
    if (std::getline(f, name)) {
        return trim(name);
    }
    return "Card " + std::to_string(card_num);
}

// Read PCM device info name
std::string get_dev_name(const fs::path& pcm_path) {
    std::ifstream f(pcm_path / "info");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("name:", 0) == 0) {
            return trim(line.substr(5));
        }
    }
    return "-";
}

void query_native_device(int card_num, int dev_num, char stream_type) {
    std::string hw_id = "hw:" + std::to_string(card_num) + "," + std::to_string(dev_num);
    snd_pcm_stream_t stream = (stream_type == 'p') ? SND_PCM_STREAM_PLAYBACK : SND_PCM_STREAM_CAPTURE;
    
    std::string card_name = get_card_name(card_num);
    std::replace(card_name.begin(), card_name.end(), '|', '-');
    
    fs::path pcm_path = fs::path("/proc/asound/card" + std::to_string(card_num)) / ("pcm" + std::to_string(dev_num) + stream_type);
    std::string dev_name = get_dev_name(pcm_path);
    std::replace(dev_name.begin(), dev_name.end(), '|', '-');

    snd_pcm_t *handle = nullptr;
    int err = snd_pcm_open(&handle, hw_id.c_str(), stream, 0);
    if (err < 0) {
        std::cout << "| `" << hw_id << "` | " << card_name << " | " << dev_name 
                  << " | - | - | *Busy or restricted (" << snd_strerror(err) << ")* | - |\n";
        return;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(handle, params);

    // Channels (using correct ALSA functions)
    unsigned int min_ch = 0, max_ch = 0;
    snd_pcm_hw_params_get_channels_min(params, &min_ch);
    snd_pcm_hw_params_get_channels_max(params, &max_ch);
    std::string channels = (min_ch == max_ch) ? std::to_string(min_ch) : "[" + std::to_string(min_ch) + " " + std::to_string(max_ch) + "]";

    // Rates
    unsigned int min_rate = 0, max_rate = 0;
    int dir = 0;
    snd_pcm_hw_params_get_rate_min(params, &min_rate, &dir);
    snd_pcm_hw_params_get_rate_max(params, &max_rate, &dir);
    std::string rates;
    if (min_rate == max_rate) {
        rates = std::to_string(min_rate) + "Hz";
    } else {
        rates = "[" + std::to_string(min_rate) + "Hz " + std::to_string(max_rate) + "Hz]";
    }

    // Formats
    const snd_pcm_format_t test_formats[] = {
        SND_PCM_FORMAT_S8, SND_PCM_FORMAT_U8,
        SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S16_BE,
        SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S24_BE,
        SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S24_3BE,
        SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S32_BE,
        SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_FLOAT_BE
    };
    std::string formats_str = "";
    for (auto fmt : test_formats) {
        if (snd_pcm_hw_params_test_format(handle, params, fmt) == 0) {
            const char* name = snd_pcm_format_name(fmt);
            if (name) {
                if (!formats_str.empty()) formats_str += ", ";
                formats_str += name;
            }
        }
    }
    if (formats_str.empty()) formats_str = "-";

    // Access Modes
    const snd_pcm_access_t test_access[] = {
        SND_PCM_ACCESS_MMAP_INTERLEAVED,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        SND_PCM_ACCESS_RW_NONINTERLEAVED
    };
    std::string access_str = "";
    for (auto acc : test_access) {
        if (snd_pcm_hw_params_test_access(handle, params, acc) == 0) {
            const char* name = snd_pcm_access_name(acc);
            if (name) {
                if (!access_str.empty()) access_str += ", ";
                access_str += name;
            }
        }
    }
    if (access_str.empty()) access_str = "-";

    snd_pcm_close(handle);

    std::cout << "| `" << hw_id << "` | " << card_name << " | " << dev_name 
              << " | " << channels << " | " << rates << " | " << formats_str << " | " << access_str << " |\n";
}

void scan_stream(char stream_type, const std::string& label) {
    std::cout << "### " << label << " Devices\n\n";
    std::cout << "| Device ID | Card Name | Device Name | Channels | Sample Rates | Bit Depths / Formats | Access Modes |\n";
    std::cout << "|---|---|---|---|---|---|---|\n";

    for (auto const& dir_entry : fs::directory_iterator("/proc/asound")) {
        if (!dir_entry.is_directory()) continue;
        std::string filename = dir_entry.path().filename().string();
        if (filename.rfind("card", 0) != 0) continue;

        int card_num = std::stoi(filename.substr(4));

        for (auto const& pcm_entry : fs::directory_iterator(dir_entry.path())) {
            std::string pcm_name = pcm_entry.path().filename().string();
            if (pcm_name.rfind("pcm", 0) != 0 || pcm_name.back() != stream_type) continue;

            int dev_num = std::stoi(pcm_name.substr(3, pcm_name.length() - 4));
            query_native_device(card_num, dev_num, stream_type);
        }
    }
    std::cout << "\n";
}

int main() {
    std::cout << "## System ALSA Hardware Capabilities (Native C++ libasound)\n\n";
    scan_stream('p', "Playback");
    scan_stream('c', "Capture");
    return 0;
}
