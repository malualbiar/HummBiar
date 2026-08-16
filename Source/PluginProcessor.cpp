#include "PluginProcessor.h"
#include "PluginEditor.h"

HumToMIDIProcessor::HumToMIDIProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      pitchDetector(44100.0f, 512),
      noteTracker(44100.0f)
{
}

HumToMIDIProcessor::~HumToMIDIProcessor() {}

void HumToMIDIProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    pitchDetector = PitchDetector(static_cast<float>(sampleRate), samplesPerBlock);
    noteTracker = NoteTracker(static_cast<float>(sampleRate));
}

void HumToMIDIProcessor::releaseResources() {}

void HumToMIDIProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
    midiMessages.clear();
    double sampleRate = getSampleRate();
    if (sampleRate <= 0.0) return;

    // Fetch live DAW host BPM
    if (auto* currentPlayHead = getPlayHead()) {
        if (auto pos = currentPlayHead->getPosition()) {
            if (pos->getBpm().hasValue()) {
                double currentBpm = *(pos->getBpm());
                if (currentBpm > 10.0 && currentBpm < 999.0) {
                    hostBpm.store(currentBpm);
                }
            }
        }
    }

    if (isPlayingMidi.load()) {
        const juce::ScopedLock sl(recordLock);
        double bufferDurationSec = static_cast<double>(buffer.getNumSamples()) / sampleRate;
        double endPlaybackTime = playbackTimeSec + bufferDurationSec;

        while (playbackEventIndex < recordedSequence.getNumEvents()) {
            if (auto* event = recordedSequence.getEventPointer(playbackEventIndex)) {
                if (event->message.getTimeStamp() <= endPlaybackTime) {
                    if (event->message.isNoteOn()) {
                        oscillator.setNote(event->message.getNoteNumber(), event->message.getVelocity() / 127.0f);
                    } else if (event->message.isNoteOff()) {
                        if (oscillator.activeNote == event->message.getNoteNumber()) {
                            oscillator.setNote(-1, 0.0f);
                        }
                    }
                    playbackEventIndex++;
                } else {
                    break;
                }
            } else {
                playbackEventIndex++;
            }
        }
        
        playbackTimeSec = endPlaybackTime;
        if (playbackTimeSec >= recordingTimeSec && recordingTimeSec > 0.0) {
            if (isLooping.load()) {
                // Seamlessly restart from the beginning
                playbackTimeSec = 0.0;
                playbackEventIndex = 0;
                oscillator.setNote(-1, 0.0f);
            } else {
                isPlayingMidi.store(false);
                oscillator.setNote(-1, 0.0f);
            }
        }
        
        currentRms.store(0.0f);
        currentPitch.store(0.0f);

    } else {
        // Normal Audio Pitch Tracking mode
        float sens = inputSensitivity.load();
        buffer.applyGain(sens);

        if (getTotalNumInputChannels() > 0) {
            auto* channelData = buffer.getReadPointer(0);
            int numSamples = buffer.getNumSamples();
            
            // Compute live peak for VU Meter
            float peak = 0.0f;
            for (int i = 0; i < numSamples; ++i) {
                float absVal = std::abs(channelData[i]);
                if (absVal > peak) peak = absVal;
            }
            currentPeakInput.store(peak);

            // If calibrating, measure ambient room noise floor for 1.5s
            if (isCalibrating.load()) {
                float blockRms = 0.0f;
                for (int i = 0; i < numSamples; ++i) {
                    blockRms += channelData[i] * channelData[i];
                }
                blockRms = std::sqrt(blockRms / static_cast<float>(numSamples));
                if (blockRms > maxNoiseRmsObserved) maxNoiseRmsObserved = blockRms;
                
                calibrationTimeSec += (static_cast<double>(numSamples) / sampleRate);
                calibrationProgress.store(static_cast<float>(calibrationTimeSec / 1.5));
                
                if (calibrationTimeSec >= 1.5) {
                    float newThreshold = std::max(0.0004f, maxNoiseRmsObserved * 1.5f);
                    noiseGateCutoff.store(newThreshold);
                    isCalibrating.store(false);
                    calibrationProgress.store(1.0f);
                }
                
                currentRms.store(blockRms);
                currentPitch.store(0.0f);
                oscillator.setNote(-1, 0.0f);
                return;
            }

            float currentGate = noiseGateCutoff.load();
            float currentOctaveLock = octaveLock.load();
            PitchResult result = pitchDetector.process(channelData, numSamples, currentGate, currentOctaveLock);

            float timeElapsedMs = (static_cast<float>(numSamples) / static_cast<float>(sampleRate)) * 1000.0f;
            
            float atkSpeed = attackSpeedMs.load();
            float stability = pitchStabilityCents.load();
            float minDur = minNoteDurationMs.load();
            noteTracker.process(result, timeElapsedMs, 0, midiMessages, atkSpeed, stability, minDur, selectedKey.load(), selectedScale.load());

            // MIDI Recording Logic
            if (isRecordingMidi.load()) {
                const juce::ScopedLock sl(recordLock);
                for (const auto metadata : midiMessages) {
                    auto msg = metadata.getMessage();
                    double eventTime = recordingTimeSec + (static_cast<double>(metadata.samplePosition) / sampleRate);
                    if (msg.isNoteOn()) {
                        double backdate = static_cast<double>(noteTracker.getAttackBackdateSec());
                        eventTime = std::max(0.0, eventTime - backdate);
                    }
                    msg.setTimeStamp(eventTime);
                    recordedSequence.addEvent(msg);
                }
                recordingTimeSec += (static_cast<double>(buffer.getNumSamples()) / sampleRate);
            }

            currentRms.store(result.rms);
            if (result.voiced) currentPitch.store(result.frequency);
            else currentPitch.store(0.0f);
            
            oscillator.setNote(noteTracker.getCurrentNote(), noteTracker.getCurrentVelocity());
        }
    }

    oscillator.waveType = static_cast<SimpleOscillator::WaveType>(selectedWaveform.load());

    int numSamples = buffer.getNumSamples();
    int numChannels = buffer.getNumChannels();
    for (int sample = 0; sample < numSamples; ++sample) {
        float synthSample = oscillator.getNextSample(sampleRate);
        for (int channel = 0; channel < numChannels; ++channel) {
            buffer.setSample(channel, sample, synthSample);
        }
    }
}

void HumToMIDIProcessor::applyPreset(int presetIndex) {
    selectedPreset.store(presetIndex);
    switch (presetIndex) {
        case 1: // PC / Laptop Mic
            inputSensitivity.store(1.6f);
            noiseGateCutoff.store(0.0018f);
            attackSpeedMs.store(20.0f);
            pitchStabilityCents.store(75.0f);
            octaveLock.store(0.90f);
            minNoteDurationMs.store(85.0f);
            break;
        case 2: // Headphone / Headset Mic
            inputSensitivity.store(0.60f);
            noiseGateCutoff.store(0.0014f);
            attackSpeedMs.store(18.0f);
            pitchStabilityCents.store(70.0f);
            octaveLock.store(0.85f);
            minNoteDurationMs.store(75.0f);
            break;
        case 3: // Fast Melodies / Riffs
            inputSensitivity.store(1.0f);
            noiseGateCutoff.store(0.0016f);
            attackSpeedMs.store(16.0f);
            pitchStabilityCents.store(75.0f);
            octaveLock.store(0.90f);
            minNoteDurationMs.store(70.0f);
            break;
        case 4: // Rock-Solid Chords & Bass
            inputSensitivity.store(1.2f);
            noiseGateCutoff.store(0.0018f);
            attackSpeedMs.store(25.0f);
            pitchStabilityCents.store(80.0f);
            octaveLock.store(0.95f);
            minNoteDurationMs.store(110.0f);
            break;
        default:
            break;
    }
}

void HumToMIDIProcessor::startCalibration() {
    maxNoiseRmsObserved = 0.0f;
    calibrationTimeSec = 0.0;
    calibrationProgress.store(0.0f);
    isCalibrating.store(true);
}

void HumToMIDIProcessor::setKeyAndScale(int key, int scale) {
    selectedKey.store(key);
    selectedScale.store(scale);
    applyKeyAndScaleSnapping();
}

void HumToMIDIProcessor::applyKeyAndScaleSnapping() {
    const juce::ScopedLock sl(recordLock);
    int key = selectedKey.load();
    int scale = selectedScale.load();

    if (rawRecordedSequence.getNumEvents() == 0 && recordedSequence.getNumEvents() > 0) {
        rawRecordedSequence = recordedSequence;
    }

    recordedSequence.clear();

    for (int i = 0; i < rawRecordedSequence.getNumEvents(); ++i) {
        const auto* event = rawRecordedSequence.getEventPointer(i);
        if (!event) continue;

        auto msg = event->message;
        if (msg.isNoteOn() || msg.isNoteOff()) {
            int originalNote = msg.getNoteNumber();
            int snappedNote = MidiConverter::snapToScale(originalNote, key, scale);
            msg.setNoteNumber(snappedNote);
        }
        recordedSequence.addEvent(msg);
    }
    recordedSequence.updateMatchedPairs();
}

void HumToMIDIProcessor::startRecording() {
    const juce::ScopedLock sl(recordLock);
    recordedSequence.clear();
    rawRecordedSequence.clear();
    recordingTimeSec = 0.0;
    isRecordingMidi.store(true);
}

void HumToMIDIProcessor::stopRecording() {
    const juce::ScopedLock sl(recordLock);
    int activeNote = noteTracker.getCurrentNote();
    if (activeNote != -1) {
        auto msg = juce::MidiMessage::noteOff(1, activeNote);
        msg.setTimeStamp(recordingTimeSec);
        recordedSequence.addEvent(msg);
    }
    isRecordingMidi.store(false);
    
    // Keep a raw copy for dynamic scale re-snapping
    rawRecordedSequence = recordedSequence;

    // Auto-detect key if enabled
    if (autoDetectKey.load()) {
        runKeyDetection();
    } else {
        applyKeyAndScaleSnapping();
    }
}

void HumToMIDIProcessor::runKeyDetection() {
    std::vector<int> notes;
    for (int i = 0; i < rawRecordedSequence.getNumEvents(); ++i) {
        const auto* e = rawRecordedSequence.getEventPointer(i);
        if (e && e->message.isNoteOn()) {
            notes.push_back(e->message.getNoteNumber());
        }
    }
    if (notes.empty()) return;

    auto [key, scale] = MidiConverter::detectKey(notes);
    detectedKey.store(key);
    detectedScale.store(scale);
    
    setKeyAndScale(key, scale);
}

void HumToMIDIProcessor::togglePlayback() {
    const juce::ScopedLock sl(recordLock);
    bool isPlaying = isPlayingMidi.load();
    if (!isPlaying && recordedSequence.getNumEvents() > 0) {
        playbackTimeSec = 0.0;
        playbackEventIndex = 0;
        isPlayingMidi.store(true);
    } else {
        isPlayingMidi.store(false);
        oscillator.setNote(-1, 0.0f);
    }
}

void HumToMIDIProcessor::toggleLoop() {
    isLooping.store(!isLooping.load());
}

void HumToMIDIProcessor::deleteNoteAtIndex(int noteOnEventIndex) {
    const juce::ScopedLock sl(recordLock);
    auto& seq = recordedSequence;
    if (noteOnEventIndex < 0 || noteOnEventIndex >= seq.getNumEvents()) return;
    auto* onEvent = seq.getEventPointer(noteOnEventIndex);
    if (!onEvent || !onEvent->message.isNoteOn()) return;
    int noteNum = onEvent->message.getNoteNumber();
    double onTime = onEvent->message.getTimeStamp();

    // Find the matching note-off that follows this note-on
    int offIndex = -1;
    for (int i = noteOnEventIndex + 1; i < seq.getNumEvents(); ++i) {
        auto* ev = seq.getEventPointer(i);
        if (ev && ev->message.isNoteOff() && ev->message.getNoteNumber() == noteNum
            && ev->message.getTimeStamp() >= onTime) {
            offIndex = i;
            break;
        }
    }

    if (noteOnEventIndex < rawRecordedSequence.getNumEvents()) {
        if (offIndex >= 0 && offIndex < rawRecordedSequence.getNumEvents()) {
            rawRecordedSequence.deleteEvent(offIndex, false);
        }
        rawRecordedSequence.deleteEvent(noteOnEventIndex, false);
        rawRecordedSequence.updateMatchedPairs();
    }

    if (offIndex >= 0) seq.deleteEvent(offIndex, false);
    seq.deleteEvent(noteOnEventIndex, false);
    seq.updateMatchedPairs();

    recordingTimeSec = 0.0;
    for (int i = 0; i < seq.getNumEvents(); ++i) {
        if (auto* ev = seq.getEventPointer(i))
            if (ev->message.getTimeStamp() > recordingTimeSec)
                recordingTimeSec = ev->message.getTimeStamp();
    }
}

void HumToMIDIProcessor::writeMidiFile(const juce::File& file) {
    const juce::ScopedLock sl(recordLock);
    
    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(960);
    
    juce::MidiMessageSequence trackSequence;
    
    double bpm = hostBpm.load();
    if (bpm <= 10.0 || bpm > 999.0) bpm = 120.0;
    
    // Add tempo event at start matching host BPM
    int microsecsPerQuarter = static_cast<int>(std::round(60000000.0 / bpm));
    trackSequence.addEvent(juce::MidiMessage::tempoMetaEvent(microsecsPerQuarter), 0.0);
    
    double ticksPerSecond = (bpm / 60.0) * 960.0;
    
    // Pair up note events from recordedSequence
    struct NoteBlock {
        int noteNumber;
        double startTimeSec;
        double endTimeSec;
        float velocity;
    };
    std::vector<NoteBlock> noteBlocks;
    std::vector<std::pair<int, std::pair<double, float>>> activeNotes;
    
    for (int i = 0; i < recordedSequence.getNumEvents(); ++i) {
        if (auto* event = recordedSequence.getEventPointer(i)) {
            auto msg = event->message;
            if (msg.isNoteOn()) {
                activeNotes.push_back({ msg.getNoteNumber(), { msg.getTimeStamp(), msg.getVelocity() / 127.0f } });
            } else if (msg.isNoteOff()) {
                int noteNum = msg.getNoteNumber();
                for (auto it = activeNotes.begin(); it != activeNotes.end(); ++it) {
                    if (it->first == noteNum) {
                        noteBlocks.push_back({ noteNum, it->second.first, msg.getTimeStamp(), it->second.second });
                        activeNotes.erase(it);
                        break;
                    }
                }
            }
        }
    }
    for (const auto& an : activeNotes) {
        noteBlocks.push_back({ an.first, an.second.first, recordingTimeSec, an.second.second });
    }
    
    std::sort(noteBlocks.begin(), noteBlocks.end(), [](const NoteBlock& a, const NoteBlock& b) {
        return a.startTimeSec < b.startTimeSec;
    });
    
    float atkSpeed = attackSpeedMs.load();
    float mergeGapSec = std::max(0.035f, std::min(0.120f, (atkSpeed / 18.0f) * 0.065f));
    float maxVibratoDipSec = std::max(0.060f, std::min(0.150f, (atkSpeed / 18.0f) * 0.100f));
    float maxGraceNoteSec = std::max(0.035f, std::min(0.080f, (atkSpeed / 18.0f) * 0.055f));
    float minDurationSec = std::max(0.035f, minNoteDurationMs.load() / 1000.0f);

    // -------------------------------------------------------------
    // STAGE 1: Monophonic Time-Collision & Subharmonic Glitch Resolver
    // If two notes start within collision window or overlap in time:
    // Keep ONLY the primary longer/stronger note and discard the collision artifact.
    // -------------------------------------------------------------
    float collisionWindowSec = std::max(0.045f, (atkSpeed / 18.0f) * 0.060f);
    std::vector<NoteBlock> nonOverlapping;
    for (const auto& nb : noteBlocks) {
        if (nonOverlapping.empty()) {
            nonOverlapping.push_back(nb);
            continue;
        }
        auto& prev = nonOverlapping.back();
        if (nb.startTimeSec < (prev.startTimeSec + collisionWindowSec) || (nb.startTimeSec < prev.endTimeSec && (prev.endTimeSec - nb.startTimeSec) > 0.025)) {
            double prevDur = prev.endTimeSec - prev.startTimeSec;
            double currDur = nb.endTimeSec - nb.startTimeSec;
            if (currDur > prevDur) {
                prev = nb; // Replace shorter/subharmonic glitch with the real note
            }
        } else {
            nonOverlapping.push_back(nb);
        }
    }

    // -------------------------------------------------------------
    // STAGE 2: Vocal Vibrato & Micro-Trill Flattener (e.g. B2 -> A#2 -> B2)
    // If pitch dips by 1-2 semitones for < maxVibratoDipSec between identical target notes,
    // flatten the dip to the target note so it doesn't split the note into pieces!
    // -------------------------------------------------------------
    std::vector<NoteBlock> flattened = nonOverlapping;
    if (flattened.size() >= 3) {
        for (size_t i = 1; i + 1 < flattened.size(); ++i) {
            auto& prev = flattened[i - 1];
            auto& curr = flattened[i];
            auto& next = flattened[i + 1];
            
            double currDur = curr.endTimeSec - curr.startTimeSec;
            int diff1 = std::abs(curr.noteNumber - prev.noteNumber);
            int diff2 = std::abs(curr.noteNumber - next.noteNumber);
            
            if (prev.noteNumber == next.noteNumber && diff1 <= 2 && diff2 <= 2 && currDur <= maxVibratoDipSec) {
                curr.noteNumber = prev.noteNumber; // Flatten vibrato wobble
            }
        }
    }

    // -------------------------------------------------------------
    // STAGE 3: Merge Same-Pitch Legato Notes (preserves fast staccato gaps)
    // -------------------------------------------------------------
    std::vector<NoteBlock> merged;
    for (const auto& nb : flattened) {
        if (!merged.empty() && 
            merged.back().noteNumber == nb.noteNumber &&
            (nb.startTimeSec - merged.back().endTimeSec) <= mergeGapSec) {
            merged.back().endTimeSec = std::max(merged.back().endTimeSec, nb.endTimeSec);
            merged.back().velocity = std::max(merged.back().velocity, nb.velocity);
        } else {
            merged.push_back(nb);
        }
    }

    // -------------------------------------------------------------
    // STAGE 4: Onset Grace Note / Release Tail Absorber
    // If a short note (< maxGraceNoteSec) is directly adjacent (<= 50ms) to a longer note,
    // it was an attack scoop (e.g. F3 before E3) or release droop - purge it!
    // -------------------------------------------------------------
    std::vector<NoteBlock> cleanNotes;
    for (size_t i = 0; i < merged.size(); ++i) {
        const auto& nb = merged[i];
        double dur = nb.endTimeSec - nb.startTimeSec;
        
        bool isGraceNote = false;
        if (dur < maxGraceNoteSec) {
            if (i > 0) {
                const auto& prev = merged[i - 1];
                double prevDur = prev.endTimeSec - prev.startTimeSec;
                if (prevDur >= 0.140 && (nb.startTimeSec - prev.endTimeSec) <= 0.050) {
                    isGraceNote = true; // Release tail droop
                }
            }
            if (i + 1 < merged.size()) {
                const auto& next = merged[i + 1];
                double nextDur = next.endTimeSec - next.startTimeSec;
                if (nextDur >= 0.140 && (next.startTimeSec - nb.endTimeSec) <= 0.050) {
                    isGraceNote = true; // Attack onset scoop (e.g. F3 before E3)
                }
            }
        }
        
        if (!isGraceNote) {
            cleanNotes.push_back(nb);
        }
    }

    // -------------------------------------------------------------
    // STAGE 5: Minimum Duration Filter & Dynamic User Threshold
    // -------------------------------------------------------------
    std::vector<NoteBlock> monoNotes;
    for (const auto& nb : cleanNotes) {
        if ((nb.endTimeSec - nb.startTimeSec) >= minDurationSec) {
            monoNotes.push_back(nb);
        }
    }
    
    int chordMode = selectedChordMode.load();
    int keyRoot = selectedKey.load();
    int scaleType = selectedScale.load();
    int quantizeGrid = selectedQuantize.load();
    float quantStrength = quantizeStrength.load();

    for (auto note : monoNotes) {
        // Apply Rhythmic Quantization
        MidiConverter::quantizeNoteTimes(note.startTimeSec, note.endTimeSec, bpm, quantizeGrid, quantStrength);

        // Apply Diatonic Scale Chord Generator
        std::vector<int> chordNotes = MidiConverter::generateScaleChord(note.noteNumber, keyRoot, scaleType, chordMode);

        for (int chordNote : chordNotes) {
            juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(1, 127, static_cast<int>(note.velocity * 127.0f)));
            auto onMsg = juce::MidiMessage::noteOn(1, chordNote, vel);
            onMsg.setTimeStamp(note.startTimeSec * ticksPerSecond);
            trackSequence.addEvent(onMsg);
            
            auto offMsg = juce::MidiMessage::noteOff(1, chordNote);
            offMsg.setTimeStamp(note.endTimeSec * ticksPerSecond);
            trackSequence.addEvent(offMsg);
        }
    }
    
    trackSequence.updateMatchedPairs();
    midiFile.addTrack(trackSequence);
    
    if (file.existsAsFile()) {
        file.deleteFile();
    }
    
    if (auto outStream = file.createOutputStream()) {
        midiFile.writeTo(*outStream);
    }
}

juce::AudioProcessorEditor* HumToMIDIProcessor::createEditor() {
    return new HumToMIDIEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new HumToMIDIProcessor();
}
