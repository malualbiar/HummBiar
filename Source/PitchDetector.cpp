#include "PitchDetector.h"
#include <cmath>
#include <algorithm>
#include <vector>

void BiquadFilter::setHighPass(float cutoffHz, float sampleRate, float Q) {
    if (sampleRate <= 0.0f) sampleRate = 44100.0f;
    float w0 = 2.0f * 3.14159265f * (cutoffHz / sampleRate);
    float cosw0 = std::cos(w0);
    float sinw0 = std::sin(w0);
    float alpha = sinw0 / (2.0f * Q);
    
    float a0 = 1.0f + alpha;
    b0 = ((1.0f + cosw0) * 0.5f) / a0;
    b1 = (-(1.0f + cosw0)) / a0;
    b2 = ((1.0f + cosw0) * 0.5f) / a0;
    a1 = (-2.0f * cosw0) / a0;
    a2 = (1.0f - alpha) / a0;
    z1 = 0.0f; z2 = 0.0f;
}

void BiquadFilter::setLowPass(float cutoffHz, float sampleRate, float Q) {
    if (sampleRate <= 0.0f) sampleRate = 44100.0f;
    float w0 = 2.0f * 3.14159265f * (cutoffHz / sampleRate);
    float cosw0 = std::cos(w0);
    float sinw0 = std::sin(w0);
    float alpha = sinw0 / (2.0f * Q);
    
    float a0 = 1.0f + alpha;
    b0 = ((1.0f - cosw0) * 0.5f) / a0;
    b1 = (1.0f - cosw0) / a0;
    b2 = ((1.0f - cosw0) * 0.5f) / a0;
    a1 = (-2.0f * cosw0) / a0;
    a2 = (1.0f - alpha) / a0;
    z1 = 0.0f; z2 = 0.0f;
}

float BiquadFilter::processSample(float in) {
    float out = b0 * in + z1;
    z1 = b1 * in - a1 * out + z2;
    z2 = b2 * in - a2 * out;
    return out;
}

PitchDetector::PitchDetector(float sampleRate, int bufferSize)
    : sampleRate(sampleRate), bufferSize(bufferSize), historyBuffer(historyLength, 0.0f), writeIndex(0),
      medianFilterBuffer(medianWindowSize, { 0.0f, 0.0f, 0.0f, false }), medianFilterIndex(0) {
    hpf80.setHighPass(80.0f, sampleRate, 0.707f);
    lpf2500.setLowPass(2500.0f, sampleRate, 0.707f);
}

PitchResult PitchDetector::process(const float* audioData, int numSamples, float noiseThreshold, float octaveLock) {
    PitchResult result { 0.0f, 0.0f, false, 0.0f };
    if (numSamples <= 0 || audioData == nullptr) return result;
    
    // 1. DC Blocker + Soft Limiter + Bandpass Pre-Filter (Removes breath wind & stops clipping)
    std::vector<float> filteredAudio(numSamples, 0.0f);
    float rms = 0.0f;
    const float r = 0.995f; // DC blocker pole
    
    for (int i = 0; i < numSamples; ++i) {
        float in = audioData[i];
        
        // Single-pole DC blocker (filters sub-audible wind puffs)
        float dcOut = in - dcBlockX1 + r * dcBlockY1;
        dcBlockX1 = in;
        dcBlockY1 = dcOut;
        
        // Soft Limiter (prevents harsh square-wave clipping on loud headphone mics)
        float softSample = std::tanh(dcOut);
        
        // Biquad Bandpass (80Hz HPF + 2.5kHz LPF)
        float sample = hpf80.processSample(softSample);
        sample = lpf2500.processSample(sample);
        
        filteredAudio[i] = sample;
        rms += sample * sample;
    }
    rms = std::sqrt(rms / numSamples);
    result.rms = rms;

    // Noise gate - uses dynamic calibrated noise threshold (rejects room & fan noise)
    if (rms < noiseThreshold || std::isnan(rms) || std::isinf(rms)) {
        result.voiced = false;
        medianFilterBuffer[medianFilterIndex] = { 0.0f, 0.0f, rms, false };
        medianFilterIndex = (medianFilterIndex + 1) % medianWindowSize;
        return result;
    }
    result.voiced = true;

    // Write pre-filtered audio to the ring buffer
    for (int i = 0; i < numSamples; ++i) {
        historyBuffer[writeIndex] = filteredAudio[i];
        writeIndex = (writeIndex + 1) % historyLength;
    }

    // Read the last historyLength samples from history buffer into a contiguous block for YIN analysis
    std::vector<float> analysisBuffer(historyLength, 0.0f);
    int readIndex = writeIndex;
    for (int i = 0; i < historyLength; ++i) {
        analysisBuffer[i] = historyBuffer[readIndex];
        readIndex = (readIndex + 1) % historyLength;
    }

    // Run YIN on the analysisBuffer
    int halfBuffer = historyLength / 2; // 1024
    std::vector<float> yinBuffer(halfBuffer, 0.0f);

    // 1. Difference function
    for (int tau = 0; tau < halfBuffer; ++tau) {
        for (int i = 0; i < halfBuffer; ++i) {
            float delta = analysisBuffer[i] - analysisBuffer[i + tau];
            yinBuffer[tau] += delta * delta;
        }
    }

    // 2. Cumulative mean normalized difference
    yinBuffer[0] = 1.0f;
    float runningSum = 0.0f;
    for (int tau = 1; tau < halfBuffer; ++tau) {
        runningSum += yinBuffer[tau];
        if (runningSum > 0.00001f) {
            yinBuffer[tau] *= static_cast<float>(tau) / runningSum;
        } else {
            yinBuffer[tau] = 1.0f;
        }
    }

    // Vocal Search Range: 55Hz (approx A1) to 1100Hz (approx C6)
    int minTau = std::max(2, static_cast<int>(sampleRate / 1100.0f));
    int maxTau = std::min(halfBuffer - 2, static_cast<int>(sampleRate / 55.0f));

    // 3. Absolute threshold search within vocal range
    int tauEstimate = -1;
    float threshold = 0.15f;
    for (int tau = minTau; tau <= maxTau; ++tau) {
        if (yinBuffer[tau] < threshold) {
            while (tau + 1 <= maxTau && yinBuffer[tau + 1] < yinBuffer[tau]) {
                tau++;
            }
            tauEstimate = tau;
            break;
        }
    }

    // Fallback: Find lowest valley within vocal range ONLY IF highly periodic
    // (A threshold of 0.22 firmly rejects unvoiced plosive consonants like 'T', 'K', 'S', 'P')
    if (tauEstimate == -1) {
        float minVal = 1000.0f;
        int minTauIdx = -1;
        for (int tau = minTau; tau <= maxTau; ++tau) {
            if (yinBuffer[tau] < minVal) {
                minVal = yinBuffer[tau];
                minTauIdx = tau;
            }
        }
        if (minTauIdx != -1 && minVal < 0.22f) {
            tauEstimate = minTauIdx;
        }
    }

    // Subharmonic Octave Validation (McLeod / YIN 2*tau Check scaled by octaveLock)
    // Prevents nasal vocal tract resonances from picking the 2nd harmonic (octave above)
    if (tauEstimate != -1 && octaveLock > 0.05f) {
        int doubleTau = tauEstimate * 2;
        if (doubleTau <= maxTau) {
            int searchRadius = 4;
            int subMinTau = -1;
            float subMinVal = 1000.0f;
            for (int t = std::max(minTau, doubleTau - searchRadius); t <= std::min(maxTau, doubleTau + searchRadius); ++t) {
                if (yinBuffer[t] < subMinVal) {
                    subMinVal = yinBuffer[t];
                    subMinTau = t;
                }
            }
            float subharmonicMultiplier = 1.0f + (octaveLock * 0.55f);
            float subharmonicMaxDip = 0.25f + (octaveLock * 0.18f);
            if (subMinTau != -1 && subMinVal < subharmonicMaxDip && subMinVal <= (yinBuffer[tauEstimate] * subharmonicMultiplier)) {
                tauEstimate = subMinTau;
            }
        }
    }

    // 4. Parabolic interpolation & frequency calculation
    if (tauEstimate >= minTau && tauEstimate <= maxTau) {
        float s0 = yinBuffer[tauEstimate - 1];
        float s1 = yinBuffer[tauEstimate];
        float s2 = yinBuffer[tauEstimate + 1];
        
        float denominator = 2.0f * (2.0f * s1 - s2 - s0);
        float betterTau = static_cast<float>(tauEstimate);
        if (std::abs(denominator) > 0.00001f) {
            betterTau = static_cast<float>(tauEstimate) + (s2 - s0) / denominator;
        }
        
        if (std::isnan(betterTau) || std::isinf(betterTau) || betterTau <= 0.0f) {
            result.voiced = false;
        } else {
            result.frequency = sampleRate / betterTau;
            if (std::isnan(result.frequency) || std::isinf(result.frequency) || result.frequency < 55.0f || result.frequency > 1100.0f) {
                result.voiced = false;
                result.frequency = 0.0f;
            } else {
                result.confidence = std::max(0.0f, 1.0f - s1); 
            }
        }
    } else {
        result.voiced = false;
    }

    // 5. Dubler-style 5-Frame Median Filter & Harmonic Stabilizer
    medianFilterBuffer[medianFilterIndex] = { result.frequency, result.confidence, result.rms, result.voiced };
    medianFilterIndex = (medianFilterIndex + 1) % medianWindowSize;

    std::vector<float> voicedFreqs;
    float maxConfidence = 0.0f;
    float avgRms = 0.0f;
    int voicedCount = 0;

    for (const auto& sample : medianFilterBuffer) {
        avgRms += sample.rms;
        if (sample.voiced && sample.frequency > 0.0f) {
            voicedFreqs.push_back(sample.frequency);
            if (sample.confidence > maxConfidence) maxConfidence = sample.confidence;
            voicedCount++;
        }
    }
    avgRms /= static_cast<float>(medianWindowSize);

    // If at least 3 out of 5 frames are voiced, output the robust median
    if (voicedCount >= 3 && !voicedFreqs.empty()) {
        std::sort(voicedFreqs.begin(), voicedFreqs.end());
        float medianFreq = voicedFreqs[voicedFreqs.size() / 2];

        result.frequency = medianFreq;
        result.voiced = true;
        result.confidence = maxConfidence;
        result.rms = avgRms;
    } else if (voicedCount < 2) {
        result.voiced = false;
        result.frequency = 0.0f;
    }

    return result;
}
