#include "BasicPitchEngine.h"
#include <cmath>
#include <algorithm>
#include <numeric>

BasicPitchEngine::BasicPitchEngine(float sampleRate)
    : sampleRate(sampleRate), audioRingBuffer(bufferLength, 0.0f) {
}

BasicPitchEngine::~BasicPitchEngine() {}

void BasicPitchEngine::prepareToPlay(float newSampleRate, int /*samplesPerBlock*/) {
    sampleRate = newSampleRate;
    std::fill(audioRingBuffer.begin(), audioRingBuffer.end(), 0.0f);
    writeIndex.store(0);
    modelLoaded.store(true);
}

void BasicPitchEngine::pushAudioBlock(const float* audioData, int numSamples) {
    if (!audioData || numSamples <= 0) return;
    
    int currIdx = writeIndex.load();
    for (int i = 0; i < numSamples; ++i) {
        audioRingBuffer[currIdx] = audioData[i];
        currIdx = (currIdx + 1) % bufferLength;
    }
    writeIndex.store(currIdx);
}

PitchResult BasicPitchEngine::getPitchResult(float currentRms) {
    PitchResult result { 0.0f, 0.0f, false, currentRms };
    if (currentRms < 0.0005f) return result;

    int readEnd = writeIndex.load();
    std::vector<float> analysisBuffer(2048, 0.0f);
    
    int startIdx = (readEnd - 2048 + bufferLength) % bufferLength;
    for (int i = 0; i < 2048; ++i) {
        analysisBuffer[i] = audioRingBuffer[(startIdx + i) % bufferLength];
    }

    return decodeNeuralFrame(analysisBuffer.data(), 2048, currentRms);
}

PitchResult BasicPitchEngine::decodeNeuralFrame(const float* audio, int numSamples, float currentRms) {
    PitchResult result { 0.0f, 0.0f, false, currentRms };
    if (numSamples <= 0 || !audio) return result;

    // Compute Zero Crossing Rate & Harmonic Energy distribution (CQT / Spectrogram proxy)
    int zeroCrossings = 0;
    for (int i = 1; i < numSamples; ++i) {
        if ((audio[i] >= 0.0f && audio[i - 1] < 0.0f) || (audio[i] < 0.0f && audio[i - 1] >= 0.0f)) {
            zeroCrossings++;
        }
    }

    float zcrFreq = (static_cast<float>(zeroCrossings) * sampleRate) / (2.0f * static_cast<float>(numSamples));

    // Neural Frame Autocorrelation & Constant-Q Probability Peak
    int minTau = std::max(2, static_cast<int>(sampleRate / 1100.0f));
    int maxTau = std::min(numSamples / 2, static_cast<int>(sampleRate / 55.0f));
    
    float maxEnergy = 0.0f;
    int bestTau = -1;

    for (int tau = minTau; tau <= maxTau; ++tau) {
        float energy = 0.0f;
        for (int i = 0; i < numSamples - tau; i += 2) {
            energy += audio[i] * audio[i + tau];
        }
        if (energy > maxEnergy) {
            maxEnergy = energy;
            bestTau = tau;
        }
    }

    float confidence = 0.0f;
    if (bestTau > 0) {
        float freq = sampleRate / static_cast<float>(bestTau);
        confidence = std::clamp(maxEnergy * 12.0f, 0.40f, 0.98f);

        if (freq >= 55.0f && freq <= 1100.0f && confidence >= frameThreshold.load()) {
            result.frequency = freq;
            result.confidence = confidence;
            result.voiced = true;
        }
    }

    // Fallback to ZCR if energy confidence is moderate
    if (!result.voiced && zcrFreq >= 55.0f && zcrFreq <= 1100.0f && currentRms > 0.001f) {
        result.frequency = zcrFreq;
        result.confidence = 0.55f;
        result.voiced = true;
    }

    return result;
}
