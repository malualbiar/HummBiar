#pragma once
#include <vector>

struct PitchResult {
    float frequency;
    float confidence;
    bool voiced;
    float rms;
};

struct BiquadFilter {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void setHighPass(float cutoffHz, float sampleRate, float Q = 0.707f);
    void setLowPass(float cutoffHz, float sampleRate, float Q = 0.707f);
    float processSample(float in);
};

class PitchDetector {
public:
    PitchDetector(float sampleRate, int bufferSize);
    PitchResult process(const float* audioData, int numSamples, float noiseThreshold = 0.0006f, float octaveLock = 0.75f);

private:
    float sampleRate;
    int bufferSize;

    BiquadFilter hpf80;
    BiquadFilter lpf2500;

    float dcBlockX1 = 0.0f;
    float dcBlockY1 = 0.0f;

    std::vector<float> historyBuffer;
    int writeIndex = 0;
    static constexpr int historyLength = 2048;

    // 5-Frame Median Smoothing Ring Buffer
    struct PitchSample {
        float frequency;
        float confidence;
        float rms;
        bool voiced;
    };
    std::vector<PitchSample> medianFilterBuffer;
    int medianFilterIndex = 0;
    static constexpr int medianWindowSize = 5;
};
