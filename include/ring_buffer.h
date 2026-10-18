#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include "miniaudio.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Single producer / single consumer. The owner must quiesce BOTH endpoints before
// init(), uninit(), or destruction. In particular, a live session must not re-init
// its buffers in response to another hello. No locks or allocations in read/write.
struct PcmRingBuffer {
    ma_pcm_rb rb{};
    ma_format format = ma_format_f32;
    ma_uint32 channels = 1;
    ma_uint32 total_frames = 0;
    bool initialized = false;

    PcmRingBuffer() = default;
    ~PcmRingBuffer() { uninit(); }
    PcmRingBuffer(const PcmRingBuffer&) = delete;
    PcmRingBuffer& operator=(const PcmRingBuffer&) = delete;
    PcmRingBuffer(PcmRingBuffer&&) = delete;
    PcmRingBuffer& operator=(PcmRingBuffer&&) = delete;

    bool init(ma_format fmt, ma_uint32 ch, ma_uint32 frames) {
        uninit();
        // Validate before asking miniaudio for bytes/frame: its format lookup
        // assumes a valid enum. Its encoded offsets also reserve the top bit.
        if (fmt < ma_format_u8 || fmt > ma_format_f32 || ch == 0 ||
            ch > MA_MAX_CHANNELS || frames == 0) return false;
        const ma_uint32 bytesPerFrame = ma_get_bytes_per_frame(fmt, ch);
        const ma_uint64 maxBufferBytes = 0x7FFFFFFFULL - (MA_SIMD_ALIGNMENT - 1);
        if (static_cast<ma_uint64>(frames) * bytesPerFrame > maxBufferBytes) return false;
        if (ma_pcm_rb_init(fmt, ch, frames, nullptr, nullptr, &rb) != MA_SUCCESS) return false;
        format = fmt;
        channels = ch;
        total_frames = frames;
        initialized = true;
        return true;
    }

    void uninit() {
        if (initialized) {
            ma_pcm_rb_uninit(&rb);
            initialized = false;
        }
        rb = {};
        total_frames = 0;
    }

    size_t write(const void* data, size_t bytes) {
        if (!initialized || data == nullptr) return 0;
        const size_t bytesPerFrame = ma_get_bytes_per_frame(format, channels);
        // Bound work to the space available on entry, even if the consumer is
        // advancing concurrently. Cast only AFTER limiting the size_t request.
        ma_uint32 remaining = static_cast<ma_uint32>(std::min<size_t>(
            bytes / bytesPerFrame, ma_pcm_rb_available_write(&rb)));
        size_t written = 0;
        const auto* source = static_cast<const unsigned char*>(data);
        while (remaining > 0) {
            void* mapped = nullptr;
            ma_uint32 actual = remaining;
            if (ma_pcm_rb_acquire_write(&rb, &actual, &mapped) != MA_SUCCESS || actual == 0) break;
            const size_t chunkBytes = static_cast<size_t>(actual) * bytesPerFrame;
            std::memcpy(mapped, source + written, chunkBytes);
            if (ma_pcm_rb_commit_write(&rb, actual) != MA_SUCCESS) break;
            written += chunkBytes;
            remaining -= actual;
        }
        return written;
    }

    size_t read(void* data, size_t bytes) {
        if (!initialized || data == nullptr) return 0;
        const size_t bytesPerFrame = ma_get_bytes_per_frame(format, channels);
        ma_uint32 remaining = static_cast<ma_uint32>(std::min<size_t>(
            bytes / bytesPerFrame, ma_pcm_rb_available_read(&rb)));
        size_t readBytes = 0;
        auto* destination = static_cast<unsigned char*>(data);
        while (remaining > 0) {
            void* mapped = nullptr;
            ma_uint32 actual = remaining;
            if (ma_pcm_rb_acquire_read(&rb, &actual, &mapped) != MA_SUCCESS || actual == 0) break;
            const size_t chunkBytes = static_cast<size_t>(actual) * bytesPerFrame;
            std::memcpy(destination + readBytes, mapped, chunkBytes);
            if (ma_pcm_rb_commit_read(&rb, actual) != MA_SUCCESS) break;
            readBytes += chunkBytes;
            remaining -= actual;
        }
        return readBytes;
    }

    float get_fill_level() {
        if (!initialized || total_frames == 0) return 0.0f;
        // Use miniaudio's atomic cursor accounting rather than a second counter
        // that can lag publication and underflow when the consumer catches up.
        const ma_uint32 filled = std::min(ma_pcm_rb_available_read(&rb), total_frames);
        return static_cast<float>(filled) / total_frames;
    }
};

#endif
