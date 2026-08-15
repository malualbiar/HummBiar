#pragma once
#include <string>
#include <vector>

class MidiConverter {
public:
    static int frequencyToMidiNote(float frequency);
    static float frequencyToFractionalMidiNote(float frequency);
    static std::string midiNoteToString(int midiNote);
    static std::vector<int> getScaleIntervals(int scaleType);
    static int snapToScale(int midiNote, int rootNote, int scaleType);
    static int quantizeWithHysteresis(float fractionalNote, int activeNote, int rootNote, int scaleType, float deadbandSemitones = 0.65f);
    static std::vector<int> generateScaleChord(int rootMidiNote, int keyRoot, int scaleType, int chordMode);
    static void quantizeNoteTimes(double& startTimeSec, double& endTimeSec, double bpm, int quantizeGrid, float strength);

    // Key Detection: Krumhansl-Schmuckler algorithm
    // Returns {rootNote (0-11), scaleType (1=Major, 2=Minor)}
    // Pass a list of MIDI note numbers from the recorded sequence.
    static std::pair<int,int> detectKey(const std::vector<int>& midiNotes);
};
