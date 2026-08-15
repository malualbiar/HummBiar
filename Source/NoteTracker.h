#pragma once
#include <JuceHeader.h>
#include "PitchDetector.h"

class NoteTracker {
public:
    NoteTracker(float sampleRate);

    // Processes a pitch result, updating the internal state machine and generating MIDI events
    void process(const PitchResult& currentPitch, float timeElapsedMs, int sampleOffset, juce::MidiBuffer& midiBuffer, float attackSpeedMs = 18.0f, float pitchStabilityCents = 60.0f, int selectedKey = 0, int selectedScale = 0);

    int getCurrentNote() const { return currentMidiNote; }
    float getCurrentVelocity() const { return lastVelocity; }
    float getAttackBackdateSec() const { return (attackTimeMs / 1000.0f); }

private:
    float sampleRate;
    
    enum class State {
        Idle,
        Attack,
        Sustaining,
        Transition,
        Hangover,
        Release
    };

    State currentState = State::Idle;
    
    int currentMidiNote = -1;
    int targetMidiNote = -1;
    float lastVelocity = 0.0f;
    
    float stateTimeMs = 0.0f; 
    float noteDurationMs = 0.0f;
    float hangoverTimeMs = 0.0f;
    float prevRms = 0.0f;
    
    // Hysteresis / Time thresholds (in milliseconds)
    float attackTimeMs = 25.0f;     
    float transitionTimeMs = 25.0f; 
    float releaseTimeMs = 35.0f;    
    static constexpr float glottalHangoverMaxMs = 45.0f;
    static constexpr float minNoteDurationMs = 50.0f;
    
    void sendNoteOn(int note, float velocity, int sampleOffset, juce::MidiBuffer& midiBuffer);
    void sendNoteOff(int note, int sampleOffset, juce::MidiBuffer& midiBuffer);
};
