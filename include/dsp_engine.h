#ifndef DSP_ENGINE_H
#define DSP_ENGINE_H

#include "device_manager.h"
#include "web_server.h"
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>
#include <cmath>

class ClientManager; // Forward declaration

// Base class for audio effects
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void process(float* buffer, size_t frames, unsigned channels) = 0;
};

// Simple Gain Processor
class GainProcessor : public AudioProcessor {
public:
    GainProcessor(float gain = 1.0f) : m_gain(gain) {}
    void set_gain(float gain) { m_gain = gain; }
    float get_gain() const { return m_gain; }
    void process(float* buffer, size_t frames, unsigned channels) override {
        for (size_t i = 0; i < frames * channels; ++i) {
            buffer[i] *= m_gain;
        }
    }
private:
    std::atomic<float> m_gain;
};

// Basic Soft-Knee Compressor
class CompressorProcessor : public AudioProcessor {
public:
    CompressorProcessor(float threshold = 0.5f, float ratio = 4.0f, float attack = 0.01f, float release = 0.1f, float sampleRate = 48000.0f) 
        : m_threshold(threshold), m_ratio(ratio), m_attack(attack), m_release(release), m_sampleRate(sampleRate) {
        m_attackCoef = std::exp(-1.0f / (m_sampleRate * m_attack));
        m_releaseCoef = std::exp(-1.0f / (m_sampleRate * m_release));
    }

    void set_threshold(float t) { m_threshold = t; }
    void set_ratio(float r) { m_ratio = r; }
    float get_threshold() const { return m_threshold; }
    float get_ratio() const { return m_ratio; }

    void process(float* buffer, size_t frames, unsigned channels) override {
        for (size_t i = 0; i < frames * channels; ++i) {
            float input = buffer[i];
            float absInput = std::abs(input);
            
            // Calculate target gain based on threshold and ratio
            float targetGain = 1.0f;
            if (absInput > m_threshold) {
                targetGain = m_threshold + (absInput - m_threshold) / m_ratio;
                targetGain /= absInput;
            }

            // Smooth the gain change (Attack/Release)
            float coef = (targetGain < m_envelope) ? m_attackCoef : m_releaseCoef;
            m_envelope = coef * m_envelope + (1.0f - coef) * targetGain;
            
            buffer[i] *= m_envelope;
        }
    }

private:
    std::atomic<float> m_threshold;
    std::atomic<float> m_ratio;
    float m_attack;
    float m_release;
    float m_sampleRate;
    float m_attackCoef;
    float m_releaseCoef;
    float m_envelope = 1.0f;
};

class DspEngine {
public:
    DspEngine(DeviceManager& deviceManager, ClientManager& clientManager, AudioMetrics& metrics);
    ~DspEngine();

    void start();
    void stop();

    // Accessors for the processor chain
    GainProcessor* get_gain_processor() { return m_gainProc.get(); }
    CompressorProcessor* get_compressor() { return m_compProc.get(); }

private:
    void processing_loop();
    void process_audio();
    void update_meters(const std::vector<float>& buffer, unsigned channels, bool isCapture);

    DeviceManager& m_deviceManager;
    ClientManager& m_clientManager;
    AudioMetrics& m_metrics;
    std::thread m_workerThread;
    std::atomic<bool> m_running{false};
    
    std::vector<float> m_mixBuffer;
    
    // Processor chain
    std::unique_ptr<GainProcessor> m_gainProc;
    std::unique_ptr<CompressorProcessor> m_compProc;
    
    ma_uint32 m_engineSampleRate = 48000;
    ma_uint32 m_engineChannels = 2;
};

#endif
