#include <JuceHeader.h>
#include "PitchDetector.h"
#include "MidiConverter.h"
#include <iostream>
#include <iomanip>

class HumAnalyzer : public juce::AudioIODeviceCallback {
public:
    HumAnalyzer() : pitchDetector(44100.0f, 512) {}

    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override 
    {
        juce::ignoreUnused(outputChannelData, numOutputChannels, context);
        
        if (numInputChannels > 0 && inputChannelData[0] != nullptr) {
            PitchResult result = pitchDetector.process(inputChannelData[0], numSamples);
            
            if (result.rms > 0.0001f) {
                // Audio is definitely hitting the mic
                if (result.voiced && result.confidence > 0.4f) {
                    int midiNote = MidiConverter::frequencyToMidiNote(result.frequency);
                    std::string noteName = MidiConverter::midiNoteToString(midiNote);
                    
                    std::cout << "\rRMS: " << std::fixed << std::setprecision(4) << result.rms 
                              << " | Pitch: " << std::setprecision(1) << result.frequency << " Hz"
                              << " | Note: " << noteName 
                              << " | Conf: " << static_cast<int>(result.confidence * 100) << "%       " << std::flush;
                } else {
                    std::cout << "\rRMS: " << std::fixed << std::setprecision(4) << result.rms 
                              << " | Unvoiced/Low Conf                                " << std::flush;
                }
            } else {
                std::cout << "\rSilence (RMS: " << std::fixed << std::setprecision(4) << result.rms << ")                                     " << std::flush;
            }
        }
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        pitchDetector = PitchDetector(static_cast<float>(device->getCurrentSampleRate()), device->getCurrentBufferSizeSamples());
    }

    void audioDeviceStopped() override {}

private:
    PitchDetector pitchDetector;
};

int main(int argc, char* argv[]) {
    juce::ignoreUnused(argc, argv);
    
    std::cout << "Starting HumToMIDI Prototype...\n";
    
    juce::AudioDeviceManager deviceManager;
    
    // Print available devices for debugging
    std::cout << "--- Available Audio Devices ---\n";
    for (auto* type : deviceManager.getAvailableDeviceTypes()) {
        std::cout << "[" << type->getTypeName() << "]\n";
        type->scanForDevices();
        juce::StringArray inDevices = type->getDeviceNames(true);
        for (const auto& name : inDevices) {
            std::cout << "  IN:  " << name << "\n";
        }
    }
    std::cout << "-------------------------------\n";

    // Try to force WASAPI ("Windows Audio") if available, as DirectSound can be buggy for capture
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.initialise(1, 0, nullptr, true, juce::String(), nullptr);
    deviceManager.getAudioDeviceSetup(setup);
    
    // Let's explicitly try to find a "Windows Audio" device type
    deviceManager.setCurrentAudioDeviceType("Windows Audio", true);
    
    juce::String error = deviceManager.initialiseWithDefaultDevices(1, 0); 
    
    if (error.isNotEmpty()) {
        std::cerr << "Audio device error: " << error << "\n";
        return 1;
    }
    
    std::cout << "\nAudio device initialized: " << deviceManager.getCurrentAudioDevice()->getName() << "\n";
    std::cout << "Hum into your microphone (Press Enter to exit)\n\n";
    
    HumAnalyzer analyzer;
    deviceManager.addAudioCallback(&analyzer);
    
    std::cin.get();
    
    deviceManager.removeAudioCallback(&analyzer);
    return 0;
}
