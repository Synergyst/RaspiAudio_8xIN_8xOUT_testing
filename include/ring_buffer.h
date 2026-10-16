#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include "miniaudio.h"
#include "constants.h"
#include <mutex>
#include <cstring>
#include <atomic>

struct PcmRingBuffer {
    ma_pcm_rb rb;
    ma_format format = ma_format_f32;
    ma_uint32 channels = 1;
    ma_uint32 total_frames = 0;
    std::atomic<ma_uint32> current_frames{0};
    bool initialized = false;

    bool init(ma_format fmt, ma_uint32 ch, ma_uint32 frames) {
        format = fmt;
        channels = ch;
        total_frames = frames;
        current_frames.store(0);
        if (ma_pcm_rb_init(format, channels, frames, NULL, NULL, &rb) != MA_SUCCESS) {
            return false;
        }
        initialized = true;
        return true;
    }

    void uninit() {
        if (initialized) {
            ma_pcm_rb_uninit(&rb);
            initialized = false;
        }
    }

    size_t write(const void* data, size_t bytes) {
        if (!initialized) return 0;
        ma_uint32 framesToWrite = bytes / (sizeof(float) * channels);
        if (framesToWrite == 0) return 0;
        
        void* pMappedBuffer;
        ma_uint32 actualFrames = framesToWrite;
        if (ma_pcm_rb_acquire_write(&rb, &actualFrames, &pMappedBuffer) != MA_SUCCESS) return 0;
        if (actualFrames == 0) return 0;
        
        std::memcpy(pMappedBuffer, data, actualFrames * sizeof(float) * channels);
        ma_pcm_rb_commit_write(&rb, actualFrames);
        
        current_frames.fetch_add(actualFrames, std::memory_order_relaxed);
        return actualFrames * sizeof(float) * channels;
    }

    size_t read(void* data, size_t bytes) {
        if (!initialized) return 0;
        ma_uint32 framesToRead = bytes / (sizeof(float) * channels);
        if (framesToRead == 0) return 0;
        
        void* pMappedBuffer;
        ma_uint32 actualFrames = framesToRead;
        if (ma_pcm_rb_acquire_read(&rb, &actualFrames, &pMappedBuffer) != MA_SUCCESS) return 0;
        if (actualFrames == 0) return 0;
        
        std::memcpy(data, pMappedBuffer, actualFrames * sizeof(float) * channels);
        ma_pcm_rb_commit_read(&rb, actualFrames);
        
        // Ensure we don't underflow the counter
        ma_uint32 current = current_frames.load(std::memory_order_relaxed);
        while (current >= actualFrames && 
               !current_frames.compare_exchange_weak(current, current - actualFrames, std::memory_order_relaxed));
               
        return actualFrames * sizeof(float) * channels;
    }

    float get_fill_level() {
        if (!initialized || total_frames == 0) return 0.0f;
        return (float)current_frames.load(std::memory_order_relaxed) / total_frames;
    }
};

#endif
