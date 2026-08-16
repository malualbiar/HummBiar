#pragma once
#include <JuceHeader.h>
#include "PitchDetector.h"
#include "NoteTracker.h"
#include "InstrumentPresets.h"
#include "BasicPitchEngine.h"

struct SimpleOscillator {
    double phase = 0.0;
    double phase2 = 0.0;  // 2nd oscillator (detuned / harmonic)
    double phase3 = 0.0;  // 3rd oscillator
    double phaseDelta = 0.0;
    float currentVolume = 0.0f;
    float targetVolume = 0.0f;
    double currentFreq = 0.0;
    double targetFreq = 0.0;
    int activeNote = -1;

    // Envelope for instruments with decay (Pluck, Bell, Piano)
    float envLevel = 0.0f;
    float envDecay = 0.9995f;
    bool envelopeActive = false;

    // Slow attack accumulator (Strings)
    float attackLevel = 0.0f;
    
    enum class WaveType {
        Sine = 0,
        Triangle,
        Saw,
        Square,
        SoftPiano,
        Flute,
        Pluck,
        Strings,
        Bell,
        Bass,
        Muted
    };
    WaveType waveType = WaveType::Sine;

    void setNote(int note, float velocity) {
        if (note == -1 || std::isnan(velocity) || std::isinf(velocity)) {
            targetVolume = 0.0f;
            activeNote = -1;
            envelopeActive = false;
            attackLevel = 0.0f;
        } else {
            bool newNote = (note != activeNote);
            activeNote = note;
            double freq = juce::MidiMessage::getMidiNoteInHertz(note);
            if (!std::isnan(freq) && !std::isinf(freq) && freq > 0.0) {
                targetFreq = freq;
                targetVolume = velocity * 0.15f;
                if (newNote) {
                    // Retrigger envelope on new note
                    envLevel = 1.0f;
                    envelopeActive = true;
                    attackLevel = 0.0f;
                    // For pluck/bell, reset phase for crisp attack
                    if (waveType == WaveType::Pluck || waveType == WaveType::Bell || waveType == WaveType::SoftPiano) {
                        phase = 0.0; phase2 = 0.0; phase3 = 0.0;
                    }
                }
            } else {
                targetVolume = 0.0f;
                activeNote = -1;
            }
        }
    }

    float getNextSample(double sampleRate) {
        if (sampleRate <= 0.0 || std::isnan(sampleRate) || std::isinf(sampleRate)) return 0.0f;
        if (std::isnan(targetFreq) || std::isinf(targetFreq) || targetFreq <= 0.0) {
            targetVolume = 0.0f; targetFreq = 0.0;
        }
        if (currentFreq == 0.0 || std::isnan(currentFreq) || std::isinf(currentFreq)) {
            currentFreq = targetFreq;
        } else {
            currentFreq += (targetFreq - currentFreq) * 0.08;
        }
        if (currentFreq > 0.0 && !std::isnan(currentFreq) && !std::isinf(currentFreq)) {
            phaseDelta = (currentFreq * 2.0 * juce::MathConstants<double>::pi) / sampleRate;
        } else {
            phaseDelta = 0.0;
        }
        if (std::isnan(phase) || std::isinf(phase)) phase = 0.0;

        const double twoPi = 2.0 * juce::MathConstants<double>::pi;
        phase  += phaseDelta;           if (phase  >= twoPi) phase  = std::fmod(phase,  twoPi);
        phase2 += phaseDelta * 2.0;     if (phase2 >= twoPi) phase2 = std::fmod(phase2, twoPi);
        phase3 += phaseDelta * 0.5;     if (phase3 >= twoPi) phase3 = std::fmod(phase3, twoPi);

        currentVolume += (targetVolume - currentVolume) * 0.015f;

        if (waveType == WaveType::Muted) return 0.0f;
        if (std::isnan(currentVolume) || std::isinf(currentVolume) || currentVolume < 0.0001f) {
            currentVolume = 0.0f; return 0.0f;
        }

        float sample = 0.0f;

        switch (waveType) {
            case WaveType::Sine:
                sample = std::sin(phase);
                break;

            case WaveType::Triangle:
                sample = (phase < juce::MathConstants<double>::pi)
                    ? -1.0f + (2.0f * (float)phase / juce::MathConstants<double>::pi)
                    :  3.0f - (2.0f * (float)phase / juce::MathConstants<double>::pi);
                break;

            case WaveType::Saw:
                sample = -1.0f + 2.0f * (float)(phase / twoPi);
                break;

            case WaveType::Square:
                sample = (phase < juce::MathConstants<double>::pi) ? 1.0f : -1.0f;
                break;

            case WaveType::SoftPiano: {
                // Fundamental + octave harmonic + 3rd harmonic, decay envelope
                float f1 = std::sin(phase);
                float f2 = 0.45f * std::sin(phase2);              // octave
                float f3 = 0.15f * std::sin(phase * 3.0);         // 3rd harmonic
                envLevel *= 0.9996f;
                sample = (f1 + f2 + f3) * envLevel * 0.6f;
                break;
            }

            case WaveType::Flute: {
                // Sine + subtle 2nd/3rd harmonics + gentle breath noise
                float f1 = std::sin(phase);
                float f2 = 0.18f * std::sin(phase2);
                float f3 = 0.08f * std::sin(phase * 3.0);
                // Gentle vibrato (slow sine modulation)
                float vibrato = 1.0f + 0.004f * std::sin(phase * 0.02);
                sample = (f1 * vibrato + f2 + f3) * 0.75f;
                break;
            }

            case WaveType::Pluck: {
                // Saw with fast exponential decay — Karplus-Strong inspired
                float saw = -1.0f + 2.0f * (float)(phase / twoPi);
                envLevel *= 0.9988f;  // fast decay
                sample = saw * envLevel * 0.7f;
                break;
            }

            case WaveType::Strings: {
                // Two detuned saws with slow attack swell
                double detunedDelta = phaseDelta * 1.007;
                phase2 += (detunedDelta - phaseDelta); // only add the detuning delta portion
                float saw1 = -1.0f + 2.0f * (float)(phase  / twoPi);
                float saw2 = -1.0f + 2.0f * (float)(std::fmod(phase2, twoPi) / twoPi);
                attackLevel += (1.0f - attackLevel) * 0.0006f; // slow attack swell ~300ms
                sample = (saw1 * 0.5f + saw2 * 0.5f) * attackLevel * 0.55f;
                break;
            }

            case WaveType::Bell: {
                // Inharmonic partials: fundamental + 2.76x + 5.4x + decay
                float f1 = std::sin(phase);
                float f2 = 0.50f * std::sin(phase * 2.756);
                float f3 = 0.25f * std::sin(phase * 5.404);
                float f4 = 0.12f * std::sin(phase * 8.933);
                envLevel *= 0.9990f;  // medium decay
                sample = (f1 + f2 + f3 + f4) * envLevel * 0.50f;
                break;
            }

            case WaveType::Bass: {
                // Sine fundamental + sub octave (phase3) + soft clip warmth
                float fund = std::sin(phase);
                float sub  = 0.35f * std::sin(phase3); // one octave below
                float raw  = fund + sub;
                // Soft clip (tanh-like) for warmth
                sample = raw / (1.0f + std::abs(raw) * 0.5f);
                sample *= 0.65f;
                break;
            }

            default:
                sample = std::sin(phase);
                break;
        }

        float outputSample = sample * currentVolume;
        if (std::isnan(outputSample) || std::isinf(outputSample)) return 0.0f;
        return outputSample;
    }
};

class HumToMIDIProcessor : public juce::AudioProcessor {
public:
    friend class HumToMIDIEditor;
    friend class PianoRollVisualizer;

    HumToMIDIProcessor();
    ~HumToMIDIProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HumToMIDI"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    std::atomic<float> currentRms { 0.0f };
    std::atomic<float> currentPeakInput { 0.0f }; // Live peak for VU Meter
    std::atomic<float> currentPitch { 0.0f };
    std::atomic<int> selectedWaveform { 0 };
    
    // 6 Master Calibration Parameters
    std::atomic<float> inputSensitivity { 1.0f };       // 0.1x to 3.5x
    std::atomic<float> noiseGateCutoff { 0.0015f };     // 0.0001 to 0.02 (firm gate against breath air)
    std::atomic<float> attackSpeedMs { 18.0f };         // 8ms to 45ms
    std::atomic<float> pitchStabilityCents { 75.0f };   // 20c to 85c (rock-solid intonation lock)
    std::atomic<float> octaveLock { 0.90f };            // 0.0 to 1.0 (heavy fundamental lock)
    std::atomic<float> minNoteDurationMs { 80.0f };     // 30ms to 160ms
    
    std::atomic<int> selectedPreset { 0 }; // 0 = Custom, 1 = PC Mic, 2 = Headset, 3 = Fast Melodies, 4 = Solid Chords
    void applyPreset(int presetIndex);

    std::atomic<int> selectedInputSource { 0 }; // 0 = Vocal, 1 = Whistle, 2 = Guitar, 3 = Bass, 4 = Wind, 5 = Percussion
    void applyInputSourceProfile(int sourceIndex);
    
    std::atomic<int> selectedKey { 0 }; // 0 = C, 1 = C#, ...
    std::atomic<int> selectedScale { 0 }; // 0 = Chromatic, 1 = Major, ...
    void setKeyAndScale(int key, int scale);
    void applyKeyAndScaleSnapping();
    void sanitizeRecordedSequence();

    std::atomic<int> selectedChordMode { 0 }; // 0 = Single Note, 1 = Triad, 2 = 7th, ...
    std::atomic<int> selectedQuantize { 0 }; // 0 = Off, 1 = 1/4, 2 = 1/8, 3 = 1/16, ...
    std::atomic<float> quantizeStrength { 1.0f }; // 0.0 to 1.0 (0% to 100%)
    std::atomic<double> hostBpm { 120.0 };

    // Calibration interface
    std::atomic<bool> isCalibrating { false };
    std::atomic<float> calibrationProgress { 0.0f };
    void startCalibration();

    // MIDI Recording & Playback interface
    std::atomic<bool> isRecordingMidi { false };
    std::atomic<bool> isPlayingMidi { false };
    std::atomic<bool> isLooping { false };  // Loop playback
    void startRecording();
    void stopRecording();
    void togglePlayback();
    void toggleLoop();
    void writeMidiFile(const juce::File& file);

    // Note editing: delete note-on/off pair at a given index
    void deleteNoteAtIndex(int noteOnEventIndex);

    // Key Detection
    std::atomic<bool>  autoDetectKey { false };
    std::atomic<int>   detectedKey   { 0 };  // 0-11
    std::atomic<int>   detectedScale { 1 };  // 1=Major, 2=Minor
    void runKeyDetection(); // call after stopRecording()

    std::atomic<bool> useNeuralEngine { false };

private:
    PitchDetector pitchDetector;
    BasicPitchEngine basicPitchEngine;
    NoteTracker noteTracker;
    SimpleOscillator oscillator;

    double calibrationTimeSec = 0.0;
    float maxNoiseRmsObserved = 0.0f;

    juce::MidiMessageSequence rawRecordedSequence;
    juce::MidiMessageSequence recordedSequence;
    double recordingTimeSec = 0.0;
    double playbackTimeSec = 0.0;
    int playbackEventIndex = 0;
    juce::CriticalSection recordLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HumToMIDIProcessor)
};
