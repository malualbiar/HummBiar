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
    basicPitchEngine.prepareToPlay(static_cast<float>(sampleRate), samplesPerBlock);
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

            PitchResult result;
            if (useNeuralEngine.load()) {
                basicPitchEngine.pushAudioBlock(channelData, numSamples);
                float blockRms = 0.0f;
                for (int i = 0; i < numSamples; ++i) blockRms += channelData[i] * channelData[i];
                blockRms = std::sqrt(blockRms / static_cast<float>(numSamples));
                result = basicPitchEngine.getPitchResult(blockRms);
            } else {
                result = pitchDetector.process(channelData, numSamples, currentGate, currentOctaveLock);
            }

            float timeElapsedMs = (static_cast<float>(numSamples) / static_cast<float>(sampleRate)) * 1000.0f;
            
            float atkSpeed = attackSpeedMs.load();
            float stability = pitchStabilityCents.load();
            float minDur = minNoteDurationMs.load();
            int currentKey = selectedKey.load();
            int currentScale = selectedScale.load();
            int chordMode = selectedChordMode.load();

            noteTracker.process(result, timeElapsedMs, 0, midiMessages, atkSpeed, stability, minDur, currentKey, currentScale);

            // Expand live MIDI messages for DAW output and Live Recording if Chord Mode is enabled
            if (chordMode != 0 && !midiMessages.isEmpty()) {
                juce::MidiBuffer chordMidiBuffer;
                for (const auto metadata : midiMessages) {
                    auto msg = metadata.getMessage();
                    int samplePos = metadata.samplePosition;
                    if (msg.isNoteOn()) {
                        std::vector<int> chordPitches = MidiConverter::generateScaleChord(msg.getNoteNumber(), currentKey, currentScale, chordMode);
                        for (int pitch : chordPitches) {
                            chordMidiBuffer.addEvent(juce::MidiMessage::noteOn(msg.getChannel(), pitch, msg.getVelocity()), samplePos);
                        }
                    } else if (msg.isNoteOff()) {
                        std::vector<int> chordPitches = MidiConverter::generateScaleChord(msg.getNoteNumber(), currentKey, currentScale, chordMode);
                        for (int pitch : chordPitches) {
                            chordMidiBuffer.addEvent(juce::MidiMessage::noteOff(msg.getChannel(), pitch), samplePos);
                        }
                    } else {
                        chordMidiBuffer.addEvent(msg, samplePos);
                    }
                }
                midiMessages = chordMidiBuffer;
            }

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

void HumToMIDIProcessor::applyInputSourceProfile(int sourceIndex) {
    selectedInputSource.store(sourceIndex);
    auto profile = InstrumentPresets::getProfileByIndex(sourceIndex);
    inputSensitivity.store(profile.sensitivity);
    noiseGateCutoff.store(profile.noiseGateCutoff);
    attackSpeedMs.store(profile.attackSpeedMs);
    pitchStabilityCents.store(profile.pitchStabilityCents);
    octaveLock.store(profile.octaveLock);
    minNoteDurationMs.store(profile.minNoteDurationMs);
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
    sanitizeRecordedSequence();
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
    
    // Keep a raw copy for dynamic scale re-snapping & de-glitching
    rawRecordedSequence = recordedSequence;

    // Run automatic 5-stage de-glitcher on sequence immediately when recording stops
    sanitizeRecordedSequence();

    // Auto-detect key if enabled
    if (autoDetectKey.load()) {
        runKeyDetection();
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

void HumToMIDIProcessor::deleteSelectedNotes(const std::vector<int>& noteOnIndices) {
    if (noteOnIndices.empty()) return;
    std::vector<int> sortedIndices = noteOnIndices;
    std::sort(sortedIndices.begin(), sortedIndices.end(), std::greater<int>());
    for (int idx : sortedIndices) {
        deleteNoteAtIndex(idx);
    }
}

void HumToMIDIProcessor::transposeSelectedNotes(const std::vector<int>& noteOnIndices, int semitoneDelta) {
    if (noteOnIndices.empty() || semitoneDelta == 0) return;
    const juce::ScopedLock sl(recordLock);
    auto& seq = recordedSequence;

    for (int idx : noteOnIndices) {
        if (idx < 0 || idx >= seq.getNumEvents()) continue;
        auto* onEvent = seq.getEventPointer(idx);
        if (!onEvent || !onEvent->message.isNoteOn()) continue;

        int noteNum = onEvent->message.getNoteNumber();
        int newPitch = std::clamp(noteNum + semitoneDelta, 0, 127);
        double onTime = onEvent->message.getTimeStamp();

        onEvent->message.setNoteNumber(newPitch);

        for (int i = idx + 1; i < seq.getNumEvents(); ++i) {
            auto* ev = seq.getEventPointer(i);
            if (ev && ev->message.isNoteOff() && ev->message.getNoteNumber() == noteNum && ev->message.getTimeStamp() >= onTime) {
                ev->message.setNoteNumber(newPitch);
                break;
            }
        }
    }
    seq.updateMatchedPairs();
    rawRecordedSequence = seq;
}

void HumToMIDIProcessor::nudgeSelectedNotes(const std::vector<int>& noteOnIndices, double timeDeltaSec) {
    if (noteOnIndices.empty() || timeDeltaSec == 0.0) return;
    const juce::ScopedLock sl(recordLock);
    auto& seq = recordedSequence;

    for (int idx : noteOnIndices) {
        if (idx < 0 || idx >= seq.getNumEvents()) continue;
        auto* onEvent = seq.getEventPointer(idx);
        if (!onEvent || !onEvent->message.isNoteOn()) continue;

        int noteNum = onEvent->message.getNoteNumber();
        double onTime = onEvent->message.getTimeStamp();
        double newOnTime = std::max(0.0, onTime + timeDeltaSec);
        onEvent->message.setTimeStamp(newOnTime);

        for (int i = idx + 1; i < seq.getNumEvents(); ++i) {
            auto* ev = seq.getEventPointer(i);
            if (ev && ev->message.isNoteOff() && ev->message.getNoteNumber() == noteNum && ev->message.getTimeStamp() >= onTime) {
                double dur = ev->message.getTimeStamp() - onTime;
                ev->message.setTimeStamp(newOnTime + dur);
                break;
            }
        }
    }
    seq.updateMatchedPairs();
    rawRecordedSequence = seq;
}

void HumToMIDIProcessor::sanitizeRecordedSequence() {
    const juce::ScopedLock sl(recordLock);
    if (rawRecordedSequence.getNumEvents() == 0 && recordedSequence.getNumEvents() > 0) {
        rawRecordedSequence = recordedSequence;
    }

    recordedSequence.clear();

    struct NoteBlock {
        int noteNumber;
        double startTimeSec;
        double endTimeSec;
        float velocity;
    };
    std::vector<NoteBlock> noteBlocks;
    std::vector<std::pair<int, std::pair<double, float>>> activeNotes;

    int key = selectedKey.load();
    int scale = selectedScale.load();

    for (int i = 0; i < rawRecordedSequence.getNumEvents(); ++i) {
        if (auto* event = rawRecordedSequence.getEventPointer(i)) {
            auto msg = event->message;
            if (msg.isNoteOn() || msg.isNoteOff()) {
                int noteNum = msg.getNoteNumber();
                if (scale != 0) {
                    noteNum = MidiConverter::snapToScale(noteNum, key, scale);
                }
                if (msg.isNoteOn()) {
                    activeNotes.push_back({ noteNum, { msg.getTimeStamp(), msg.getVelocity() / 127.0f } });
                } else if (msg.isNoteOff()) {
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
    }
    for (const auto& an : activeNotes) {
        noteBlocks.push_back({ an.first, an.second.first, recordingTimeSec, an.second.second });
    }

    std::sort(noteBlocks.begin(), noteBlocks.end(), [](const NoteBlock& a, const NoteBlock& b) {
        return a.startTimeSec < b.startTimeSec;
    });

    float atkSpeed = attackSpeedMs.load();
    float mergeGapSec = std::max(0.040f, std::min(0.120f, (atkSpeed / 18.0f) * 0.065f));
    float maxVibratoDipSec = std::max(0.060f, std::min(0.150f, (atkSpeed / 18.0f) * 0.100f));
    float maxGraceNoteSec = std::max(0.045f, std::min(0.090f, (atkSpeed / 18.0f) * 0.065f));
    float minDurationSec = std::max(0.040f, minNoteDurationMs.load() / 1000.0f);

    // STAGE 1: Monophonic Time-Collision & Subharmonic Glitch Resolver
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
                prev = nb;
            }
        } else {
            nonOverlapping.push_back(nb);
        }
    }

    // STAGE 2: Vocal Vibrato & Micro-Trill Flattener
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
                curr.noteNumber = prev.noteNumber;
            }
        }
    }

    // STAGE 3: Merge Same-Pitch Legato Notes
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

    // STAGE 4: Onset Grace Note & Release Tail Absorber
    std::vector<NoteBlock> cleanNotes;
    for (size_t i = 0; i < merged.size(); ++i) {
        const auto& nb = merged[i];
        double dur = nb.endTimeSec - nb.startTimeSec;

        bool isGraceNote = false;
        if (dur < maxGraceNoteSec) {
            if (i > 0) {
                const auto& prev = merged[i - 1];
                double prevDur = prev.endTimeSec - prev.startTimeSec;
                if (prevDur >= 0.120 && (nb.startTimeSec - prev.endTimeSec) <= 0.060) {
                    isGraceNote = true;
                }
            }
            if (i + 1 < merged.size()) {
                const auto& next = merged[i + 1];
                double nextDur = next.endTimeSec - next.startTimeSec;
                if (nextDur >= 0.120 && (next.startTimeSec - nb.endTimeSec) <= 0.060) {
                    isGraceNote = true;
                }
            }
        }

        if (!isGraceNote) {
            cleanNotes.push_back(nb);
        }
    }

    // STAGE 5: Minimum Duration Filter & Chord Expansion for Piano Roll & Loop Playback
    int chordMode = selectedChordMode.load();
    for (const auto& note : cleanNotes) {
        if ((note.endTimeSec - note.startTimeSec) >= minDurationSec) {
            std::vector<int> chordPitches = MidiConverter::generateScaleChord(note.noteNumber, key, scale, chordMode);
            juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(1, 127, static_cast<int>(note.velocity * 127.0f)));
            for (int pitch : chordPitches) {
                auto onMsg = juce::MidiMessage::noteOn(1, pitch, vel);
                onMsg.setTimeStamp(note.startTimeSec);
                recordedSequence.addEvent(onMsg);

                auto offMsg = juce::MidiMessage::noteOff(1, pitch);
                offMsg.setTimeStamp(note.endTimeSec);
                recordedSequence.addEvent(offMsg);
            }
        }
    }

    recordedSequence.updateMatchedPairs();
}

void HumToMIDIProcessor::writeMidiFile(const juce::File& file) {
    const juce::ScopedLock sl(recordLock);
    
    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(960);
    
    juce::MidiMessageSequence trackSequence;
    
    double bpm = hostBpm.load();
    if (bpm <= 10.0 || bpm > 999.0) bpm = 120.0;
    
    int microsecsPerQuarter = static_cast<int>(std::round(60000000.0 / bpm));
    trackSequence.addEvent(juce::MidiMessage::tempoMetaEvent(microsecsPerQuarter), 0.0);
    
    double ticksPerSecond = (bpm / 60.0) * 960.0;

    int chordMode = selectedChordMode.load();
    int keyRoot = selectedKey.load();
    int scaleType = selectedScale.load();
    int quantizeGrid = selectedQuantize.load();
    float quantStrength = quantizeStrength.load();

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
                        double startSec = it->second.first;
                        double endSec = msg.getTimeStamp();
                        float velFloat = it->second.second;
                        activeNotes.erase(it);

                        // Apply Rhythmic Quantization
                        MidiConverter::quantizeNoteTimes(startSec, endSec, bpm, quantizeGrid, quantStrength);

                        // Apply Diatonic Scale Chord Generator
                        std::vector<int> chordNotes = MidiConverter::generateScaleChord(noteNum, keyRoot, scaleType, chordMode);
                        for (int chordNote : chordNotes) {
                            juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(1, 127, static_cast<int>(velFloat * 127.0f)));
                            auto onMsg = juce::MidiMessage::noteOn(1, chordNote, vel);
                            onMsg.setTimeStamp(startSec * ticksPerSecond);
                            trackSequence.addEvent(onMsg);
                            
                            auto offMsg = juce::MidiMessage::noteOff(1, chordNote);
                            offMsg.setTimeStamp(endSec * ticksPerSecond);
                            trackSequence.addEvent(offMsg);
                        }
                        break;
                    }
                }
            }
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
