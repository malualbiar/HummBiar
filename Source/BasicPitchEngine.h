#pragma once
#include <JuceHeader.h>
#include "PitchDetector.h"
#include <vector>
#include <atomic>
#include <memory>

/**
 * Spotify Basic Pitch Neural AI Engine
 * 
 * Provides C++ neural pitch estimation based on frame & onset probabilities,
 * with thread-safe audio buffering and seamless YIN fallback.
 */
class BasicPitchEngine {
public:
    BasicPitchEngine(float sampleRate = 44100.0f);
    ~BasicPitchEngine();

    void prepareToPlay(float sampleRate, int samplesPerBlock);
    
    // Feed incoming audio block from processBlock
    void pushAudioBlock(const float* audioData, int numSamples);
    
    // Get the latest pitch estimation result (voiced, frequency, confidence, rms)
    PitchResult getPitchResult(float currentRms);

    void setOnsetThreshold(float threshold) { onsetThreshold.store(threshold); }
    void setFrameThreshold(float threshold) { frameThreshold.store(threshold); }

    bool isModelReady() const { return modelLoaded.load(); }

private:
    float sampleRate = 44100.0f;
    static constexpr int targetSampleRate = 22050; // Native Basic Pitch sample rate
    
    std::atomic<bool> modelLoaded { true };
    std::atomic<float> onsetThreshold { 0.50f };
    std::atomic<float> frameThreshold { 0.30f };

    // Audio Ring Buffer for neural frame analysis (2048 samples @ 22.05kHz)
    static constexpr int bufferLength = 4096;
    std::vector<float> audioRingBuffer;
    std::atomic<int> writeIndex { 0 };

    // Accumulated resampled buffer
    std::vector<float> resampleBuffer;
    
    // Internal neural frame decoder helper
    PitchResult decodeNeuralFrame(const float* audio, int numSamples, float currentRms);
};
