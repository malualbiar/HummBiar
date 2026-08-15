#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "MidiConverter.h"

HumToMIDIEditor::HumToMIDIEditor(HumToMIDIProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p), visualizer(p),
      calibrateButton(p), loopButton(p), playButton(p), recordButton(p)
{
    setSize(920, 640);
    setLookAndFeel(&customLookAndFeel);

    // Title
    titleLabel.setText("HUMMBIAR PRO", juce::dontSendNotification);
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xffe8e8e8));
    titleLabel.setFont(juce::FontOptions(14.0f).withStyle("Bold"));
    addAndMakeVisible(titleLabel);

    // Preset Selector
    presetLabel.setText("Preset:", juce::dontSendNotification);
    presetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd0d0d0));
    presetLabel.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(presetLabel);

    presetSelector.addItem("Custom Settings", 1);
    presetSelector.addItem("PC / Laptop Mic", 2);
    presetSelector.addItem("Headphone Mic", 3);
    presetSelector.addItem("Fast Melodies", 4);
    presetSelector.addItem("Solid Chords/Bass", 5);
    presetSelector.setSelectedId(1, juce::dontSendNotification);
    presetSelector.onChange = [this] {
        int id = presetSelector.getSelectedId() - 1;
        if (id > 0) {
            audioProcessor.applyPreset(id);
            updateSliderValuesFromProcessor();
        }
    };
    addAndMakeVisible(presetSelector);

    // Header Buttons
    calibrateButton.onClick = [this] {
        audioProcessor.startCalibration();
    };
    
    copyMidiButton.onDragStart = [this](const juce::MouseEvent&) {
        audioProcessor.writeMidiFile(tempMidiFile);
        if (tempMidiFile.existsAsFile()) {
            this->performExternalDragDropOfFiles({ tempMidiFile.getFullPathName() }, false, &copyMidiButton);
        }
    };
    
    playButton.onClick = [this] {
        audioProcessor.togglePlayback();
    };

    loopButton.setToggleState(audioProcessor.isLooping.load(), juce::dontSendNotification);
    loopButton.onClick = [this] {
        audioProcessor.toggleLoop();
    };
    
    recordButton.onClick = [this] {
        if (!audioProcessor.isRecordingMidi.load()) {
            audioProcessor.startRecording();
            isReadyToDrag = false;
        } else {
            audioProcessor.stopRecording();
            audioProcessor.writeMidiFile(tempMidiFile);
            isReadyToDrag = true;
        }
    };
    
    visualizer.onDragStart = [this] {
        audioProcessor.writeMidiFile(tempMidiFile);
        if (tempMidiFile.existsAsFile()) {
            this->performExternalDragDropOfFiles({ tempMidiFile.getFullPathName() }, false, &visualizer);
        }
    };
    
    addAndMakeVisible(calibrateButton);
    addAndMakeVisible(copyMidiButton);
    addAndMakeVisible(playButton);
    addAndMakeVisible(loopButton);
    addAndMakeVisible(recordButton);

    // Controls - Instrument Sound
    waveformLabel.setText("Sound:", juce::dontSendNotification);
    waveformLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd0d0d0));
    waveformLabel.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(waveformLabel);

    waveformSelector.addItem("Sine", 1);
    waveformSelector.addItem("Triangle", 2);
    waveformSelector.addItem("Saw", 3);
    waveformSelector.addItem("Square", 4);
    waveformSelector.addItem("Soft Piano", 5);
    waveformSelector.addItem("Flute", 6);
    waveformSelector.addItem("Pluck", 7);
    waveformSelector.addItem("Strings", 8);
    waveformSelector.addItem("Bell", 9);
    waveformSelector.addItem("Bass", 10);
    waveformSelector.addItem("Muted", 11);
    waveformSelector.setSelectedId(audioProcessor.selectedWaveform.load() + 1, juce::dontSendNotification);
    waveformSelector.onChange = [this] {
        audioProcessor.selectedWaveform.store(waveformSelector.getSelectedId() - 1);
    };
    addAndMakeVisible(waveformSelector);

    // Chord Mode
    chordLabel.setText("Chord:", juce::dontSendNotification);
    chordLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd0d0d0));
    chordLabel.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(chordLabel);

    chordSelector.addItem("Single (Off)", 1);
    chordSelector.addItem("Triad (1-3-5)", 2);
    chordSelector.addItem("7th (1-3-5-7)", 3);
    chordSelector.addItem("9th (1-3-5-7-9)", 4);
    chordSelector.addItem("Sus4 (1-4-5)", 5);
    chordSelector.addItem("Power (1-5-8)", 6);
    chordSelector.addItem("Octaves (1-8)", 7);
    chordSelector.setSelectedId(audioProcessor.selectedChordMode.load() + 1, juce::dontSendNotification);
    chordSelector.onChange = [this] {
        audioProcessor.selectedChordMode.store(chordSelector.getSelectedId() - 1);
    };
    addAndMakeVisible(chordSelector);

    // Quantize Grid
    quantizeLabel.setText("Quantize:", juce::dontSendNotification);
    quantizeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd0d0d0));
    quantizeLabel.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(quantizeLabel);

    quantizeSelector.addItem("Off", 1);
    quantizeSelector.addItem("1/4 Note", 2);
    quantizeSelector.addItem("1/8 Note", 3);
    quantizeSelector.addItem("1/16 Note", 4);
    quantizeSelector.addItem("1/32 Note", 5);
    quantizeSelector.addItem("1/8 Triplet", 6);
    quantizeSelector.addItem("1/16 Triplet", 7);
    quantizeSelector.setSelectedId(audioProcessor.selectedQuantize.load() + 1, juce::dontSendNotification);
    quantizeSelector.onChange = [this] {
        audioProcessor.selectedQuantize.store(quantizeSelector.getSelectedId() - 1);
    };
    addAndMakeVisible(quantizeSelector);

    // Quantize Strength
    quantizeStrengthSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    quantizeStrengthSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    quantizeStrengthSlider.setRange(0.0, 1.0, 0.01);
    quantizeStrengthSlider.setValue(audioProcessor.quantizeStrength.load());
    quantizeStrengthSlider.onValueChange = [this] {
        audioProcessor.quantizeStrength.store(static_cast<float>(quantizeStrengthSlider.getValue()));
    };
    addAndMakeVisible(quantizeStrengthSlider);

    quantizeStrengthLabel.setText("SNAP %", juce::dontSendNotification);
    quantizeStrengthLabel.setColour(juce::Label::textColourId, juce::Colour(0xffa0a0a0));
    quantizeStrengthLabel.setFont(juce::FontOptions(9.0f).withStyle("Bold"));
    quantizeStrengthLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(quantizeStrengthLabel);

    // 6 Master Calibration Sliders Setup
    auto setupRotary = [this](juce::Slider& slider, juce::Label& label, const juce::String& text) {
        slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(slider);

        label.setText(text, juce::dontSendNotification);
        label.setColour(juce::Label::textColourId, juce::Colour(0xffa0a0a0));
        label.setFont(juce::FontOptions(9.0f).withStyle("Bold"));
        label.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(label);
    };

    setupRotary(sensitivitySlider, sensitivityLabel, "SENSITIVITY");
    sensitivitySlider.setRange(0.1, 3.5, 0.01);
    sensitivitySlider.setValue(audioProcessor.inputSensitivity.load());
    sensitivitySlider.onValueChange = [this] {
        audioProcessor.inputSensitivity.store(static_cast<float>(sensitivitySlider.getValue()));
    };

    setupRotary(noiseGateSlider, noiseGateLabel, "NOISE GATE");
    noiseGateSlider.setRange(0.0001, 0.0150, 0.0001);
    noiseGateSlider.setValue(audioProcessor.noiseGateCutoff.load());
    noiseGateSlider.onValueChange = [this] {
        audioProcessor.noiseGateCutoff.store(static_cast<float>(noiseGateSlider.getValue()));
    };

    setupRotary(attackSpeedSlider, attackSpeedLabel, "ATTACK ms");
    attackSpeedSlider.setRange(8.0, 45.0, 1.0);
    attackSpeedSlider.setValue(audioProcessor.attackSpeedMs.load());
    attackSpeedSlider.onValueChange = [this] {
        audioProcessor.attackSpeedMs.store(static_cast<float>(attackSpeedSlider.getValue()));
    };

    setupRotary(pitchStabilitySlider, pitchStabilityLabel, "STABILITY");
    pitchStabilitySlider.setRange(20.0, 85.0, 1.0);
    pitchStabilitySlider.setValue(audioProcessor.pitchStabilityCents.load());
    pitchStabilitySlider.onValueChange = [this] {
        audioProcessor.pitchStabilityCents.store(static_cast<float>(pitchStabilitySlider.getValue()));
    };

    setupRotary(octaveLockSlider, octaveLockLabel, "OCTAVE LOCK");
    octaveLockSlider.setRange(0.0, 1.0, 0.01);
    octaveLockSlider.setValue(audioProcessor.octaveLock.load());
    octaveLockSlider.onValueChange = [this] {
        audioProcessor.octaveLock.store(static_cast<float>(octaveLockSlider.getValue()));
    };

    setupRotary(minNoteSlider, minNoteLabel, "MIN NOTE ms");
    minNoteSlider.setRange(30.0, 160.0, 5.0);
    minNoteSlider.setValue(audioProcessor.minNoteDurationMs.load());
    minNoteSlider.onValueChange = [this] {
        audioProcessor.minNoteDurationMs.store(static_cast<float>(minNoteSlider.getValue()));
    };

    // Status Labels
    statusLiveSignal.setText("INPUT LEVEL", juce::dontSendNotification);
    statusRms.setText("RMS:\n0.0000", juce::dontSendNotification);
    statusPitch.setText("No Pitch\nDetected", juce::dontSendNotification);
    statusBpm.setText("BPM:\n120.0", juce::dontSendNotification);

    juce::Colour statusCol = juce::Colour(0xffe8e8e8);
    juce::FontOptions statusFont(11.0f);
    for (auto* l : {&statusLiveSignal, &statusRms, &statusPitch, &statusBpm}) {
        l->setColour(juce::Label::textColourId, statusCol);
        l->setFont(statusFont);
        addAndMakeVisible(l);
    }

    // Key Selector
    keyLabel.setText("Key", juce::dontSendNotification);
    keyLabel.setColour(juce::Label::textColourId, statusCol);
    keyLabel.setFont(statusFont);
    addAndMakeVisible(keyLabel);

    const char* keys[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int i = 0; i < 12; ++i) keySelector.addItem(keys[i], i + 1);
    keySelector.setSelectedId(audioProcessor.selectedKey.load() + 1, juce::dontSendNotification);
    keySelector.onChange = [this] {
        int key = keySelector.getSelectedId() - 1;
        int scale = scaleSelector.getSelectedId() - 1;
        audioProcessor.setKeyAndScale(key, scale);
        visualizer.repaint();
    };
    addAndMakeVisible(keySelector);

    // Scale Selector
    scaleLabel.setText("Scale", juce::dontSendNotification);
    scaleLabel.setColour(juce::Label::textColourId, statusCol);
    scaleLabel.setFont(statusFont);
    addAndMakeVisible(scaleLabel);

    scaleSelector.addItem("Chromatic (Off)", 1);
    scaleSelector.addItem("Major", 2);
    scaleSelector.addItem("Minor", 3);
    scaleSelector.addItem("Dorian", 4);
    scaleSelector.addItem("Mixolydian", 5);
    scaleSelector.addItem("Major Pentatonic", 6);
    scaleSelector.addItem("Minor Pentatonic", 7);
    scaleSelector.addItem("Harmonic Minor", 8);
    scaleSelector.setSelectedId(audioProcessor.selectedScale.load() + 1, juce::dontSendNotification);
    scaleSelector.onChange = [this] {
        int key = keySelector.getSelectedId() - 1;
        int scale = scaleSelector.getSelectedId() - 1;
        audioProcessor.setKeyAndScale(key, scale);
        visualizer.repaint();
    };
    addAndMakeVisible(scaleSelector);

    // Auto Key Detection Button
    autoKeyButton.setButtonText("AUTO KEY");
    autoKeyButton.setClickingTogglesState(true);
    autoKeyButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1a1c22));
    autoKeyButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2a8a4a));
    autoKeyButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff6ee7b7));
    autoKeyButton.setColour(juce::TextButton::textColourOnId, juce::Colour(0xffffffff));
    autoKeyButton.setToggleState(audioProcessor.autoDetectKey.load(), juce::dontSendNotification);
    autoKeyButton.onClick = [this] {
        audioProcessor.autoDetectKey.store(autoKeyButton.getToggleState());
    };
    addAndMakeVisible(autoKeyButton);

    detectedKeyLabel.setText("", juce::dontSendNotification);
    detectedKeyLabel.setColour(juce::Label::textColourId, juce::Colour(0xff6ee7b7));
    detectedKeyLabel.setJustificationType(juce::Justification::centred);
    detectedKeyLabel.setFont(juce::Font(10.0f, juce::Font::bold));
    addAndMakeVisible(detectedKeyLabel);

    addAndMakeVisible(visualizer);

    tempMidiFile = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("HumToMIDI_Recording.mid");

    startTimerHz(30);
}

HumToMIDIEditor::~HumToMIDIEditor() {
    setLookAndFeel(nullptr);
}

void HumToMIDIEditor::updateSliderValuesFromProcessor() {
    sensitivitySlider.setValue(audioProcessor.inputSensitivity.load(), juce::dontSendNotification);
    noiseGateSlider.setValue(audioProcessor.noiseGateCutoff.load(), juce::dontSendNotification);
    attackSpeedSlider.setValue(audioProcessor.attackSpeedMs.load(), juce::dontSendNotification);
    pitchStabilitySlider.setValue(audioProcessor.pitchStabilityCents.load(), juce::dontSendNotification);
    octaveLockSlider.setValue(audioProcessor.octaveLock.load(), juce::dontSendNotification);
    minNoteSlider.setValue(audioProcessor.minNoteDurationMs.load(), juce::dontSendNotification);
}

void HumToMIDIEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff15161b));

    auto bounds = getLocalBounds();
    
    // Header
    auto header = bounds.removeFromTop(50);
    g.setColour(juce::Colour(0xff1b1c21));
    g.fillRect(header);
    g.setColour(juce::Colour(0xff2a2b32));
    g.drawHorizontalLine(header.getBottom(), 0.0f, static_cast<float>(bounds.getWidth()));

    // Footer Control & Calibration Bars
    auto footer = bounds.removeFromBottom(130);
    auto controlBar = footer.removeFromTop(75);
    g.setColour(juce::Colour(0xff1b1c21));
    g.fillRect(controlBar);
    g.setColour(juce::Colour(0xff2a2b32));
    g.drawHorizontalLine(controlBar.getY(), 0.0f, static_cast<float>(bounds.getWidth()));
    
    // Status Bar
    g.setColour(juce::Colour(0xff0d0e12));
    g.fillRect(footer);
    
    g.setColour(juce::Colour(0xff2a2b32));
    int colWidth = bounds.getWidth() / 6;
    for (int i = 1; i < 6; ++i) {
        g.drawVerticalLine(i * colWidth, static_cast<float>(footer.getY() + 8), static_cast<float>(footer.getBottom() - 8));
    }

    // Live Peak VU Meter Bar in Column 1
    float peak = audioProcessor.currentPeakInput.load();
    auto vuRect = juce::Rectangle<int>(15, footer.getY() + 28, colWidth - 30, 12);
    g.setColour(juce::Colour(0xff222329));
    g.fillRoundedRectangle(vuRect.toFloat(), 3.0f);
    
    if (peak > 0.001f) {
        float fillWidth = juce::jlimit(2.0f, static_cast<float>(vuRect.getWidth()), peak * vuRect.getWidth());
        auto fillRect = juce::Rectangle<float>(vuRect.getX(), vuRect.getY(), fillWidth, vuRect.getHeight());
        
        juce::Colour vuColour = (peak >= 0.95f) ? juce::Colour(0xfff87171) : // Red (clipping)
                                (peak >= 0.70f) ? juce::Colour(0xfffacc15) : // Amber
                                                  juce::Colour(0xff4ade80);  // Green
        g.setColour(vuColour);
        g.fillRoundedRectangle(fillRect, 3.0f);
    }
}

void HumToMIDIEditor::resized() {
    auto bounds = getLocalBounds();
    
    auto header = bounds.removeFromTop(50);
    titleLabel.setBounds(header.removeFromLeft(150).reduced(15, 0));
    
    presetLabel.setBounds(header.removeFromLeft(45).reduced(0, 15));
    presetSelector.setBounds(header.removeFromLeft(145).reduced(0, 12));
    
    header.removeFromRight(15);
    int btnPad = 6;
    recordButton.setBounds(header.removeFromRight(80).reduced(0, 8));
    header.removeFromRight(btnPad);
    playButton.setBounds(header.removeFromRight(42).reduced(0, 8));
    header.removeFromRight(btnPad);
    loopButton.setBounds(header.removeFromRight(48).reduced(0, 8));
    header.removeFromRight(btnPad);
    copyMidiButton.setBounds(header.removeFromRight(80).reduced(0, 8));
    header.removeFromRight(btnPad);
    calibrateButton.setBounds(header.removeFromRight(75).reduced(0, 8));
    
    // Footer
    auto footer = bounds.removeFromBottom(130);
    auto controlBar = footer.removeFromTop(75);
    
    // Top Row of footer: Dropdowns
    auto dropRow = controlBar.removeFromLeft(330);
    waveformLabel.setBounds(dropRow.removeFromLeft(42).reduced(0, 24));
    waveformSelector.setBounds(dropRow.removeFromLeft(70).reduced(0, 22));
    dropRow.removeFromLeft(6);

    chordLabel.setBounds(dropRow.removeFromLeft(40).reduced(0, 24));
    chordSelector.setBounds(dropRow.removeFromLeft(85).reduced(0, 22));
    dropRow.removeFromLeft(6);

    quantizeLabel.setBounds(dropRow.removeFromLeft(50).reduced(0, 24));
    quantizeSelector.setBounds(dropRow.removeFromLeft(80).reduced(0, 22));
    
    // 7 Knobs on the right of controlBar
    controlBar.removeFromRight(10);
    auto placeKnob = [&](juce::Slider& s, juce::Label& l, int w = 62) {
        auto area = controlBar.removeFromRight(w);
        s.setBounds(area.removeFromTop(46).reduced(6, 3));
        l.setBounds(area);
    };

    placeKnob(minNoteSlider, minNoteLabel);
    placeKnob(octaveLockSlider, octaveLockLabel);
    placeKnob(pitchStabilitySlider, pitchStabilityLabel);
    placeKnob(attackSpeedSlider, attackSpeedLabel);
    placeKnob(noiseGateSlider, noiseGateLabel);
    placeKnob(sensitivitySlider, sensitivityLabel);
    placeKnob(quantizeStrengthSlider, quantizeStrengthLabel);

    // Status Bar: 4 info columns, then Key / Scale / AUTO KEY
    int colWidth = footer.getWidth() / 7;
    statusLiveSignal.setBounds(footer.removeFromLeft(colWidth).reduced(15, 2));
    statusRms.setBounds(footer.removeFromLeft(colWidth).reduced(15, 4));
    statusPitch.setBounds(footer.removeFromLeft(colWidth).reduced(15, 4));
    statusBpm.setBounds(footer.removeFromLeft(colWidth).reduced(15, 4));
    
    auto keyArea = footer.removeFromLeft(colWidth).reduced(8, 4);
    keyLabel.setBounds(keyArea.removeFromTop(18));
    keySelector.setBounds(keyArea.removeFromTop(24));
    
    auto scaleArea = footer.removeFromLeft(colWidth).reduced(8, 4);
    scaleLabel.setBounds(scaleArea.removeFromTop(18));
    scaleSelector.setBounds(scaleArea.removeFromTop(24));

    // AUTO KEY button in the last column
    auto autoKeyArea = footer.removeFromLeft(colWidth).reduced(6, 4);
    autoKeyButton.setBounds(autoKeyArea.removeFromTop(26));
    detectedKeyLabel.setBounds(autoKeyArea.removeFromTop(16));


    // Visualizer gets remaining space
    visualizer.setBounds(bounds.reduced(10));
}

void HumToMIDIEditor::timerCallback() {
    float rms = audioProcessor.currentRms.load();
    float pitch = audioProcessor.currentPitch.load();
    double bpm = audioProcessor.hostBpm.load();

    statusRms.setText("RMS:\n" + juce::String(rms, 4), juce::dontSendNotification);
    if (pitch > 0.0f) {
        float fractionalNote = MidiConverter::frequencyToFractionalMidiNote(pitch);
        int nearestMidiNote = static_cast<int>(std::round(fractionalNote));
        float cents = (fractionalNote - static_cast<float>(nearestMidiNote)) * 100.0f;
        juce::String noteName = MidiConverter::midiNoteToString(nearestMidiNote);
        
        juce::String centsStr = (cents >= 0.0f ? "+" : "") + juce::String(static_cast<int>(std::round(cents))) + "c";
        statusPitch.setText(noteName + " (" + centsStr + ")\n" + juce::String(pitch, 1) + " Hz", juce::dontSendNotification);
        
        if (std::abs(cents) <= 15.0f) {
            statusPitch.setColour(juce::Label::textColourId, juce::Colour(0xff4ade80));
        } else if (std::abs(cents) <= 30.0f) {
            statusPitch.setColour(juce::Label::textColourId, juce::Colour(0xfffacc15));
        } else {
            statusPitch.setColour(juce::Label::textColourId, juce::Colour(0xfff87171));
        }
    } else {
        statusPitch.setText("No Pitch\nDetected", juce::dontSendNotification);
        statusPitch.setColour(juce::Label::textColourId, juce::Colour(0xffe8e8e8));
    }

    statusBpm.setText("Host BPM:\n" + juce::String(bpm, 1), juce::dontSendNotification);
    
    // Repaint custom vector buttons
    calibrateButton.repaint();
    playButton.repaint();
    recordButton.repaint();
    loopButton.repaint();

    // Sync loop button toggle state
    if (loopButton.getToggleState() != audioProcessor.isLooping.load()) {
        loopButton.setToggleState(audioProcessor.isLooping.load(), juce::dontSendNotification);
    }

    // Sync key/scale dropdowns to processor (picks up auto-detected values)
    int procKey   = audioProcessor.selectedKey.load();
    int procScale = audioProcessor.selectedScale.load();
    if (keySelector.getSelectedId() != procKey + 1)
        keySelector.setSelectedId(procKey + 1, juce::dontSendNotification);
    if (scaleSelector.getSelectedId() != procScale + 1)
        scaleSelector.setSelectedId(procScale + 1, juce::dontSendNotification);

    // Show detected key label when Auto Key is active
    if (audioProcessor.autoDetectKey.load()) {
        static const char* keyNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        int dk = audioProcessor.detectedKey.load();
        int ds = audioProcessor.detectedScale.load();
        juce::String modeName = (ds == 2) ? " Minor" : " Major";
        detectedKeyLabel.setText(juce::String(keyNames[dk]) + modeName, juce::dontSendNotification);
    } else {
        detectedKeyLabel.setText("", juce::dontSendNotification);
    }

    visualizer.repaint();
    repaint(); // Repaints live VU meter
}

void HumToMIDIEditor::mouseDown(const juce::MouseEvent& e) {
    juce::ignoreUnused(e);
}

void HumToMIDIEditor::mouseDrag(const juce::MouseEvent& e) {
    juce::ignoreUnused(e);
}
