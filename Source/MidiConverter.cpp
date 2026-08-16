#include "MidiConverter.h"
#include <cmath>
#include <algorithm>

int MidiConverter::frequencyToMidiNote(float frequency) {
    if (std::isnan(frequency) || std::isinf(frequency) || frequency <= 0.0f) return 0;
    return static_cast<int>(std::round(69.0f + 12.0f * std::log2(frequency / 440.0f)));
}

float MidiConverter::frequencyToFractionalMidiNote(float frequency) {
    if (std::isnan(frequency) || std::isinf(frequency) || frequency <= 0.0f) return 0.0f;
    return 69.0f + 12.0f * std::log2(frequency / 440.0f);
}

std::string MidiConverter::midiNoteToString(int midiNote) {
    static const char* noteNames[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    
    if (midiNote < 0 || midiNote > 127) return "???";

    int octave = (midiNote / 12) - 1;
    int noteIndex = midiNote % 12;

    return std::string(noteNames[noteIndex]) + std::to_string(octave);
}

std::vector<int> MidiConverter::getScaleIntervals(int scaleType) {
    // 0 = Chromatic (Off)
    if (scaleType == 0) return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    // 1 = Major
    if (scaleType == 1) return {0, 2, 4, 5, 7, 9, 11};
    // 2 = Minor
    if (scaleType == 2) return {0, 2, 3, 5, 7, 8, 10};
    // 3 = Dorian
    if (scaleType == 3) return {0, 2, 3, 5, 7, 9, 10};
    // 4 = Mixolydian
    if (scaleType == 4) return {0, 2, 4, 5, 7, 9, 10};
    // 5 = Major Pentatonic
    if (scaleType == 5) return {0, 2, 4, 7, 9};
    // 6 = Minor Pentatonic
    if (scaleType == 6) return {0, 3, 5, 7, 10};
    // 7 = Harmonic Minor
    if (scaleType == 7) return {0, 2, 3, 5, 7, 8, 11};
    
    return {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
}

int MidiConverter::snapToScale(int midiNote, int rootNote, int scaleType) {
    if (scaleType == 0 || midiNote < 0 || midiNote > 127) return midiNote;
    
    std::vector<int> intervals = getScaleIntervals(scaleType);
    int root = rootNote % 12;
    int octave = midiNote / 12;
    
    int bestNote = midiNote;
    int minDistance = 127;
    
    for (int interval : intervals) {
        int scaleNote = (root + interval) % 12;
        // Check adjacent octaves just in case
        for (int oct = octave - 1; oct <= octave + 1; oct++) {
            int testNote = (oct * 12) + scaleNote;
            if (testNote >= 0 && testNote <= 127) {
                int dist = std::abs(testNote - midiNote);
                if (dist < minDistance) {
                    minDistance = dist;
                    bestNote = testNote;
                }
            }
        }
    }
    return bestNote;
}

int MidiConverter::quantizeWithHysteresis(float fractionalNote, int activeNote, int rootNote, int scaleType, float deadbandSemitones) {
    if (fractionalNote <= 0.0f) return -1;
    
    int rawCandidate = static_cast<int>(std::round(fractionalNote));
    int snappedCandidate = snapToScale(rawCandidate, rootNote, scaleType);
    
    // If we are currently holding an active note, check if fractional pitch is within the deadband
    if (activeNote >= 0 && activeNote <= 127) {
        float distance = std::abs(fractionalNote - static_cast<float>(activeNote));
        if (distance <= deadbandSemitones) {
            // Stay locked to active note (absorbs vibrato & micro-pitch jitter)
            return activeNote;
        }
    }
    
    return snappedCandidate;
}

std::vector<int> MidiConverter::generateScaleChord(int rootMidiNote, int keyRoot, int scaleType, int chordMode) {
    if (rootMidiNote < 0 || rootMidiNote > 127) return {};
    if (chordMode == 0) return { rootMidiNote }; // Single Note / Off

    int snappedRoot = snapToScale(rootMidiNote, keyRoot, scaleType);
    
    std::vector<int> chord;
    chord.push_back(snappedRoot);

    if (chordMode == 5) { // Power Chord (1 - 5 - 8)
        chord.push_back(std::clamp(snappedRoot + 7, 0, 127));
        chord.push_back(std::clamp(snappedRoot + 12, 0, 127));
        return chord;
    }
    if (chordMode == 6) { // Octaves (1 - 8)
        chord.push_back(std::clamp(snappedRoot + 12, 0, 127));
        return chord;
    }

    std::vector<int> scaleIntervals = getScaleIntervals(scaleType);
    int key = keyRoot % 12;

    // Build absolute pitch list of all scale notes across the entire MIDI pitch range (0 to 127)
    std::vector<int> allScaleNotes;
    for (int oct = -1; oct <= 10; ++oct) {
        for (int interval : scaleIntervals) {
            int note = oct * 12 + key + interval;
            if (note >= 0 && note <= 127) {
                allScaleNotes.push_back(note);
            }
        }
    }
    std::sort(allScaleNotes.begin(), allScaleNotes.end());
    allScaleNotes.erase(std::unique(allScaleNotes.begin(), allScaleNotes.end()), allScaleNotes.end());

    if (allScaleNotes.empty()) return chord;

    // Find index of snappedRoot in allScaleNotes
    auto it = std::lower_bound(allScaleNotes.begin(), allScaleNotes.end(), snappedRoot);
    int rootIdx = static_cast<int>(std::distance(allScaleNotes.begin(), it));
    if (rootIdx >= static_cast<int>(allScaleNotes.size())) rootIdx = static_cast<int>(allScaleNotes.size()) - 1;

    auto getDiatonicNoteByIdx = [&](int degreeOffset) -> int {
        int targetIdx = std::clamp(rootIdx + degreeOffset, 0, static_cast<int>(allScaleNotes.size()) - 1);
        return allScaleNotes[targetIdx];
    };

    switch (chordMode) {
        case 1: // Triad (1 - 3 - 5)
            chord.push_back(getDiatonicNoteByIdx(2)); // 3rd degree
            chord.push_back(getDiatonicNoteByIdx(4)); // 5th degree
            break;
        case 2: // 7th Chord (1 - 3 - 5 - 7)
            chord.push_back(getDiatonicNoteByIdx(2)); // 3rd degree
            chord.push_back(getDiatonicNoteByIdx(4)); // 5th degree
            chord.push_back(getDiatonicNoteByIdx(6)); // 7th degree
            break;
        case 3: // 9th Chord (1 - 3 - 5 - 7 - 9)
            chord.push_back(getDiatonicNoteByIdx(2)); // 3rd degree
            chord.push_back(getDiatonicNoteByIdx(4)); // 5th degree
            chord.push_back(getDiatonicNoteByIdx(6)); // 7th degree
            chord.push_back(getDiatonicNoteByIdx(8)); // 9th degree
            break;
        case 4: // Sus4 (1 - 4 - 5)
            chord.push_back(getDiatonicNoteByIdx(3)); // 4th degree
            chord.push_back(getDiatonicNoteByIdx(4)); // 5th degree
            break;
        default:
            break;
    }

    return chord;
}

void MidiConverter::quantizeNoteTimes(double& startTimeSec, double& endTimeSec, double bpm, int quantizeGrid, float strength) {
    if (quantizeGrid <= 0 || strength <= 0.001f || bpm <= 10.0) return;

    double beatDuration = 60.0 / bpm;
    double gridDuration = beatDuration;

    switch (quantizeGrid) {
        case 1: gridDuration = beatDuration; break;          // 1/4 note
        case 2: gridDuration = beatDuration * 0.5; break;    // 1/8 note
        case 3: gridDuration = beatDuration * 0.25; break;   // 1/16 note
        case 4: gridDuration = beatDuration * 0.125; break;  // 1/32 note
        case 5: gridDuration = beatDuration * (1.0 / 3.0); break; // 1/8 Triplet
        case 6: gridDuration = beatDuration * (1.0 / 6.0); break; // 1/16 Triplet
        default: gridDuration = beatDuration * 0.25; break;
    }

    if (gridDuration <= 0.001) return;

    double targetStart = std::round(startTimeSec / gridDuration) * gridDuration;
    startTimeSec = startTimeSec + (targetStart - startTimeSec) * strength;
    if (startTimeSec < 0.0) startTimeSec = 0.0;

    double targetEnd = std::round(endTimeSec / gridDuration) * gridDuration;
    if (targetEnd <= targetStart) targetEnd = targetStart + gridDuration;

    endTimeSec = endTimeSec + (targetEnd - endTimeSec) * strength;
    if (endTimeSec <= startTimeSec + 0.03) {
        endTimeSec = startTimeSec + gridDuration * strength;
    }
}

// ---------------------------------------------------------------------------
// Krumhansl-Schmuckler Key-Finding Algorithm
// ---------------------------------------------------------------------------
// Correlates the pitch-class histogram of the input notes against known
// major and minor key profiles to find the best matching key and mode.
std::pair<int,int> MidiConverter::detectKey(const std::vector<int>& midiNotes) {
    if (midiNotes.empty()) return {0, 1}; // Default: C Major

    // Pitch class histogram: accumulate duration-weight (count here)
    double histogram[12] = {};
    for (int note : midiNotes) {
        if (note >= 0 && note <= 127)
            histogram[note % 12] += 1.0;
    }

    // Krumhansl-Kessler tonal hierarchy profiles (major and natural minor)
    static const double majorProfile[12] = {
        6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
        2.52, 5.19, 2.39, 3.66, 2.29, 2.88
    };
    static const double minorProfile[12] = {
        6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
        2.54, 4.75, 3.98, 2.69, 3.34, 3.17
    };

    // Normalise histogram
    double histSum = 0.0;
    for (double v : histogram) histSum += v;
    if (histSum < 1.0) return {0, 1};
    double normHist[12];
    for (int i = 0; i < 12; ++i) normHist[i] = histogram[i] / histSum;

    // Compute mean of histogram
    double histMean = 0.0;
    for (double v : normHist) histMean += v;
    histMean /= 12.0;

    double bestScore = -1e9;
    int bestKey = 0;
    int bestMode = 1; // 1 = Major, 2 = Minor

    for (int root = 0; root < 12; ++root) {
        for (int mode = 0; mode < 2; ++mode) {
            const double* profile = (mode == 0) ? majorProfile : minorProfile;

            // Compute profile mean
            double profileMean = 0.0;
            for (int i = 0; i < 12; ++i) profileMean += profile[i];
            profileMean /= 12.0;

            // Pearson correlation coefficient
            double num = 0.0, denomA = 0.0, denomB = 0.0;
            for (int i = 0; i < 12; ++i) {
                int idx = (i + root) % 12;
                double h = normHist[i] - histMean;
                double p = profile[idx] - profileMean;
                num    += h * p;
                denomA += h * h;
                denomB += p * p;
            }
            double denom = std::sqrt(denomA * denomB);
            double score = (denom > 1e-9) ? (num / denom) : 0.0;

            if (score > bestScore) {
                bestScore = score;
                bestKey   = root;
                bestMode  = (mode == 0) ? 1 : 2; // 1=Major, 2=Minor
            }
        }
    }

    return {bestKey, bestMode};
}
