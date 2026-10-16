#define MA_DEBUG_OUTPUT
#define MINIAUDIO_IMPLEMENTATION
#include "include/miniaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>

#define SAMPLE_RATE     48000
#define CHANNELS        2
#define RING_BUFFER_SEC 2  // 2 seconds of safety buffer

typedef struct {
    ma_pcm_rb rb;
    bool is_running;
} AudioBridge;

// Capture callback: Pulls data and writes it into the ring buffer
void capture_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    AudioBridge* pBridge = (AudioBridge*)pDevice->pUserData;
    (void)pOutput;

    if (!pInput || frameCount == 0) return;

    void* pWriteHW;
    ma_uint32 framesToWrite = frameCount;
    if (ma_pcm_rb_acquire_write(&pBridge->rb, &framesToWrite, &pWriteHW) == MA_SUCCESS) {
        if (framesToWrite > 0) {
            memcpy(pWriteHW, pInput, framesToWrite * ma_get_bytes_per_frame(pDevice->capture.format, pDevice->capture.channels));
        }
        ma_pcm_rb_commit_write(&pBridge->rb, framesToWrite);
    }
}

// Playback callback: Reads data from the ring buffer and pushes it out
void playback_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    AudioBridge* pBridge = (AudioBridge*)pDevice->pUserData;
    (void)pInput;

    if (!pOutput || frameCount == 0) return;

    memset(pOutput, 0, frameCount * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));

    void* pReadHW;
    ma_uint32 framesToRead = frameCount;
    if (ma_pcm_rb_acquire_read(&pBridge->rb, &framesToRead, &pReadHW) == MA_SUCCESS) {
        if (framesToRead > 0) {
            memcpy(pOutput, pReadHW, framesToRead * ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels));
        }
        ma_pcm_rb_commit_read(&pBridge->rb, framesToRead);
    }
}

static AudioBridge g_bridge;

void int_handler(int dummy) {
    (void)dummy;
    g_bridge.is_running = false;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    ma_context context;
    ma_device_config captureConfig;
    ma_device_config playbackConfig;
    ma_device captureDevice;
    ma_device playbackDevice;
    
    ma_device_info* pPlaybackInfos;
    ma_uint32 playbackCount;
    ma_device_info* pCaptureInfos;
    ma_uint32 captureCount;
    ma_uint32 i;
    
    ma_device_id* pCaptureID = NULL;
    ma_device_id* pPlaybackID = NULL;

    signal(SIGINT, int_handler);
    g_bridge.is_running = true;

    // 1. Initialize ALSA context explicitly forcing ALSA backend on Linux
    ma_context_config contextConfig = ma_context_config_init();
    if (ma_context_init(NULL, 0, &contextConfig, &context) != MA_SUCCESS) {
        printf("Failed to initialize ALSA context.\n");
        return -1;
    }

    // Enumerate devices to locate IDs cleanly
    if (ma_context_get_devices(&context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount) != MA_SUCCESS) {
        printf("Failed to enumerate audio devices.\n");
        ma_context_uninit(&context);
        return -1;
    }

    for (i = 0; i < captureCount; ++i) {
        if (strstr(pCaptureInfos[i].name, "USB Audio") != NULL || strstr(pCaptureInfos[i].name, "USB PnP") != NULL) {
            pCaptureID = &pCaptureInfos[i].id;
        }
    }

    for (i = 0; i < playbackCount; ++i) {
        if (strstr(pPlaybackInfos[i].name, "UAC2_Gadget") != NULL) {
            pPlaybackID = &pPlaybackInfos[i].id;
        }
    }

    if (!pCaptureID || !pPlaybackID) {
        printf("Error: Could not match target capture or playback devices.\n");
        ma_context_uninit(&context);
        return -1;
    }

    // 2. Initialize the Ring Buffer
    ma_uint32 rbCapacityFrames = SAMPLE_RATE * RING_BUFFER_SEC;
    if (ma_pcm_rb_init(ma_format_f32, CHANNELS, rbCapacityFrames, NULL, NULL, &g_bridge.rb) != MA_SUCCESS) {
        printf("Failed to initialize ring buffer.\n");
        ma_context_uninit(&context);
        return -1;
    }

    // 3. Configure Capture Device
    captureConfig = ma_device_config_init(ma_device_type_capture);
    captureConfig.capture.pDeviceID = pCaptureID;
    captureConfig.capture.format    = ma_format_f32;
    captureConfig.capture.channels  = CHANNELS;
    captureConfig.sampleRate        = SAMPLE_RATE;
    captureConfig.dataCallback      = capture_data_callback;
    captureConfig.pUserData         = &g_bridge;

    // 4. Configure Playback Device
    playbackConfig = ma_device_config_init(ma_device_type_playback);
    playbackConfig.playback.pDeviceID = pPlaybackID;
    playbackConfig.playback.format    = ma_format_f32;
    playbackConfig.playback.channels  = CHANNELS;
    playbackConfig.sampleRate         = SAMPLE_RATE;
    playbackConfig.dataCallback       = playback_data_callback;
    playbackConfig.pUserData          = &g_bridge;

    // 5. Initialize & Start Devices via Context
    if (ma_device_init(&context, &captureConfig, &captureDevice) != MA_SUCCESS) {
        printf("Failed to initialize capture device.\n");
        goto cleanup_rb;
    }

    if (ma_device_init(&context, &playbackConfig, &playbackDevice) != MA_SUCCESS) {
        printf("Failed to initialize playback device.\n");
        goto cleanup_capture;
    }

    if (ma_device_start(&captureDevice) != MA_SUCCESS) {
        printf("Failed to start capture device.\n");
        goto cleanup_playback;
    }

    if (ma_device_start(&playbackDevice) != MA_SUCCESS) {
        printf("Failed to start playback device.\n");
        goto cleanup_capture;
    }

    printf("Audio bridge running with debug output: USB Audio -> Ring Buffer -> UAC2. Press Ctrl+C to exit.\n");

    while (g_bridge.is_running) {
        ma_sleep(100);
    }

    printf("\nStopping devices and cleaning up...\n");
    ma_device_uninit(&playbackDevice);
    ma_device_uninit(&captureDevice);
    ma_pcm_rb_uninit(&g_bridge.rb);
    ma_context_uninit(&context);

    return 0;

cleanup_playback:
    ma_device_uninit(&playbackDevice);
cleanup_capture:
    ma_device_uninit(&captureDevice);
cleanup_rb:
    ma_pcm_rb_uninit(&g_bridge.rb);
    ma_context_uninit(&context);
    return -1;
}
