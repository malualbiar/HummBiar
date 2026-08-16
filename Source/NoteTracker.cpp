#include "NoteTracker.h"
#include "MidiConverter.h"

NoteTracker::NoteTracker(float sampleRate) : sampleRate(sampleRate) {}

void NoteTracker::process(const PitchResult& pitchResult, float timeElapsedMs, int sampleOffset, juce::MidiBuffer& midiBuffer, float attackSpeedMs, float pitchStabilityCents, float minNoteDurationMs, int selectedKey, int selectedScale) {
    configuredMinNoteDurationMs = std::max(30.0f, minNoteDurationMs);
    attackTimeMs = std::max(8.0f, attackSpeedMs);
    transitionTimeMs = std::max(12.0f, attackSpeedMs * 1.5f);
    releaseTimeMs = std::max(18.0f, attackSpeedMs * 1.8f);
    float deadband = std::max(0.35f, pitchStabilityCents / 100.0f);
    
    int rawNote = -1;
    
    // Accept valid pitch only if voiced with high confidence (rejects plosive consonant noise)
    if (pitchResult.voiced && pitchResult.confidence > 0.55f && pitchResult.rms > 0.0004f && pitchResult.frequency > 0.0f) {
        float fractionalNote = MidiConverter::frequencyToFractionalMidiNote(pitchResult.frequency);
        rawNote = MidiConverter::quantizeWithHysteresis(fractionalNote, currentMidiNote, selectedKey, selectedScale, deadband);
    }

    // Acoustic Transient Spike Detector: Fast consecutive staccato shots ("Tu-Tu-Tu", "Da-Da-Da")
    float rmsRatio = (prevRms > 0.0001f) ? (pitchResult.rms / prevRms) : 1.0f;
    bool isAttackTransient = (pitchResult.rms > 0.0008f && rmsRatio >= 2.20f && pitchResult.confidence > 0.65f && noteDurationMs >= configuredMinNoteDurationMs && rawNote != currentMidiNote);

    switch (currentState) {
        case State::Idle:
            if (rawNote != -1) {
                targetMidiNote = rawNote;
                stateTimeMs = 0.0f;
                currentState = State::Attack;
            }
            break;

        case State::Attack:
            if (rawNote == -1) {
                currentState = State::Idle;
            } else if (rawNote == targetMidiNote) {
                stateTimeMs += timeElapsedMs;
                if (stateTimeMs >= attackTimeMs) {
                    currentMidiNote = targetMidiNote;
                    noteDurationMs = 0.0f;
                    float velocity = juce::jlimit(0.25f, 1.0f, pitchResult.rms * 16.0f);
                    sendNoteOn(currentMidiNote, velocity, sampleOffset, midiBuffer);
                    currentState = State::Sustaining;
                }
            } else {
                // Pitch shifted during vocal attack scoop - update target without dropping to idle
                targetMidiNote = rawNote;
                stateTimeMs = 0.0f;
            }
            break;

        case State::Sustaining:
            noteDurationMs += timeElapsedMs;
            if (isAttackTransient && rawNote != -1 && rawNote != currentMidiNote) {
                // Retrigger distinct staccato shot note on energy transient
                sendNoteOff(currentMidiNote, sampleOffset, midiBuffer);
                currentMidiNote = rawNote;
                noteDurationMs = 0.0f;
                float velocity = juce::jlimit(0.25f, 1.0f, pitchResult.rms * 16.0f);
                sendNoteOn(currentMidiNote, velocity, sampleOffset, midiBuffer);
                currentState = State::Sustaining;
            } else if (rawNote == -1) {
                // Glottal / breath micro-dropout - enter Hangover state instead of immediately cutting note
                hangoverTimeMs = 0.0f;
                currentState = State::Hangover;
            } else if (rawNote != currentMidiNote) {
                targetMidiNote = rawNote;
                stateTimeMs = 0.0f;
                currentState = State::Transition;
            }
            break;

        case State::Hangover:
            noteDurationMs += timeElapsedMs;
            hangoverTimeMs += timeElapsedMs;
            if (rawNote == currentMidiNote) {
                // Voice resumed at identical pitch - smooth continuation
                currentState = State::Sustaining;
            } else if (rawNote != -1) {
                // Voice resumed at different pitch - transition
                targetMidiNote = rawNote;
                stateTimeMs = 0.0f;
                currentState = State::Transition;
            } else if (hangoverTimeMs >= glottalHangoverMaxMs) {
                // Hangover expired - proceed to release
                stateTimeMs = 0.0f;
                currentState = State::Release;
            }
            break;

        case State::Transition:
            noteDurationMs += timeElapsedMs;
            if (rawNote == -1) {
                hangoverTimeMs = 0.0f;
                currentState = State::Hangover;
            } else if (rawNote == targetMidiNote) {
                stateTimeMs += timeElapsedMs;
                if (stateTimeMs >= transitionTimeMs && noteDurationMs >= configuredMinNoteDurationMs) {
                    sendNoteOff(currentMidiNote, sampleOffset, midiBuffer);
                    currentMidiNote = targetMidiNote;
                    noteDurationMs = 0.0f;
                    float velocity = juce::jlimit(0.25f, 1.0f, pitchResult.rms * 16.0f);
                    sendNoteOn(currentMidiNote, velocity, sampleOffset, midiBuffer);
                    currentState = State::Sustaining;
                }
            } else if (rawNote == currentMidiNote) {
                currentState = State::Sustaining;
            } else {
                targetMidiNote = rawNote;
                stateTimeMs = 0.0f;
            }
            break;

        case State::Release:
            noteDurationMs += timeElapsedMs;
            if (rawNote == -1) {
                stateTimeMs += timeElapsedMs;
                if (stateTimeMs >= releaseTimeMs && noteDurationMs >= configuredMinNoteDurationMs) {
                    sendNoteOff(currentMidiNote, sampleOffset, midiBuffer);
                    currentMidiNote = -1;
                    currentState = State::Idle;
                }
            } else if (rawNote == currentMidiNote) {
                currentState = State::Sustaining;
            } else {
                if (noteDurationMs >= configuredMinNoteDurationMs) {
                    sendNoteOff(currentMidiNote, sampleOffset, midiBuffer);
                    currentMidiNote = -1;
                    targetMidiNote = rawNote;
                    stateTimeMs = 0.0f;
                    currentState = State::Attack;
                }
            }
            break;
    }
    prevRms = pitchResult.rms;
}

void NoteTracker::sendNoteOn(int note, float velocityFloat, int sampleOffset, juce::MidiBuffer& midiBuffer) {
    if (note < 0 || note > 127) return;
    lastVelocity = velocityFloat;
    juce::uint8 velocity = static_cast<juce::uint8>(velocityFloat * 127.0f);
    if (velocity == 0) velocity = 1;
    auto msg = juce::MidiMessage::noteOn(1, note, velocity);
    midiBuffer.addEvent(msg, sampleOffset);
}

void NoteTracker::sendNoteOff(int note, int sampleOffset, juce::MidiBuffer& midiBuffer) {
    if (note < 0 || note > 127) return;
    lastVelocity = 0.0f;
    auto msg = juce::MidiMessage::noteOff(1, note);
    midiBuffer.addEvent(msg, sampleOffset);
}
