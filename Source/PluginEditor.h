#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "MidiConverter.h"

class PianoRollVisualizer : public juce::Component, public juce::KeyListener {
public:
    PianoRollVisualizer(HumToMIDIProcessor& p) : processor(p) {
        setWantsKeyboardFocus(true);
        addKeyListener(this);
    }

    std::function<void()> onDragStart;
    bool hasDragged = false;
    int selectedNoteOnIndex = -1;  // index into recordedSequence of the selected note-on event

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
            && selectedNoteOnIndex >= 0) {
            processor.deleteNoteAtIndex(selectedNoteOnIndex);
            selectedNoteOnIndex = -1;
            repaint();
            return true;
        }
        return false;
    }

    void mouseDown(const juce::MouseEvent& e) override {
        hasDragged = false;
        grabKeyboardFocus();

        // Hit-test: find which note-on event the user clicked
        int hit = hitTestNote(e.x, e.y);
        if (hit >= 0) {
            if (e.mods.isRightButtonDown()) {
                // Right-click = instant delete
                processor.deleteNoteAtIndex(hit);
                selectedNoteOnIndex = -1;
            } else {
                selectedNoteOnIndex = (selectedNoteOnIndex == hit) ? -1 : hit;
            }
            repaint();
        } else {
            selectedNoteOnIndex = -1;
            repaint();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (!hasDragged && e.getDistanceFromDragStart() > 4) {
            hasDragged = true;
            if (onDragStart) onDragStart();
        }
    }

    void mouseUp(const juce::MouseEvent&) override {
        hasDragged = false;
    }

    void paint(juce::Graphics& g) override {
        // Very dark background matching the mockup
        g.fillAll(juce::Colour(0xff0d0e12)); 
        
        int minNote = 127;
        int maxNote = 0;
        double maxTime = 1.0;
        
        {
            const juce::ScopedLock sl(processor.recordLock);
            auto& seq = processor.recordedSequence;
            
            for (int i = 0; i < seq.getNumEvents(); ++i) {
                if (auto* event = seq.getEventPointer(i)) {
                    auto msg = event->message;
                    if (msg.isNoteOn()) {
                        int note = msg.getNoteNumber();
                        if (note < minNote) minNote = note;
                        if (note > maxNote) maxNote = note;
                        if (msg.getTimeStamp() > maxTime) maxTime = msg.getTimeStamp();
                    }
                    if (msg.isNoteOff()) {
                        if (msg.getTimeStamp() > maxTime) maxTime = msg.getTimeStamp();
                    }
                }
            }
            if (processor.recordingTimeSec > maxTime) maxTime = processor.recordingTimeSec;
            if (processor.playbackTimeSec > maxTime) maxTime = processor.playbackTimeSec;
        }

        if (minNote > maxNote) {
            minNote = 60;
            maxNote = 72;
        } else {
            minNote = std::max(0, minNote - 2);
            maxNote = std::min(127, maxNote + 2);
        }
        
        int numSemitones = maxNote - minNote + 1;
        auto bounds = getLocalBounds().toFloat();
        float pianoRollLeft = 55.0f; 
        float drawWidth = bounds.getWidth() - pianoRollLeft;
        float drawHeight = bounds.getHeight();
        
        float rowHeight = drawHeight / numSemitones;
        for (int s = 0; s < numSemitones; ++s) {
            int midiNote = maxNote - s;
            float y = s * rowHeight;
            
            int noteInOctave = midiNote % 12;
            bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 || noteInOctave == 8 || noteInOctave == 10);
            
            // Highlight in-scale notes
            int scaleType = processor.selectedScale.load();
            int root = processor.selectedKey.load();
            bool inScale = true;
            if (scaleType != 0) {
                auto intervals = MidiConverter::getScaleIntervals(scaleType);
                int relativeNote = (midiNote + 120 - root) % 12;
                inScale = (std::find(intervals.begin(), intervals.end(), relativeNote) != intervals.end());
            }
            
            // Grid background row
            if (inScale) {
                g.setColour(isBlackKey ? juce::Colour(0xff121318) : juce::Colour(0xff17181e));
            } else {
                g.setColour(juce::Colour(0xff060709));
            }
            
            g.fillRect(pianoRollLeft, y, drawWidth, rowHeight);
            
            // Faint horizontal grid lines
            g.setColour(juce::Colour(0xff2a2b32));
            g.drawHorizontalLine(static_cast<int>(y), pianoRollLeft, bounds.getWidth());

            // Y-Axis Labels
            g.setColour(juce::Colour(0xffa0a0a0));
            g.setFont(juce::FontOptions(11.0f).withStyle("Bold"));
            juce::String noteStr = MidiConverter::midiNoteToString(midiNote);
            g.drawText(noteStr, 0, static_cast<int>(y), static_cast<int>(pianoRollLeft - 15), static_cast<int>(rowHeight), juce::Justification::centredRight);
        }

        // Faint vertical grid lines (seconds)
        g.setColour(juce::Colour(0xff2a2b32));
        int seconds = static_cast<int>(maxTime);
        for (int sec = 1; sec <= seconds; ++sec) {
            float x = pianoRollLeft + (static_cast<float>(sec) / static_cast<float>(maxTime)) * drawWidth;
            g.drawVerticalLine(static_cast<int>(x), 0.0f, drawHeight);
        }

        // Cache note geometry for hit-testing
        noteGeometry.clear();

        struct ActiveNote {
            int noteNumber;
            double startTime;
            float velocity;
            int eventIndex;
        };
        std::vector<ActiveNote> activeNotes;

        {
            const juce::ScopedLock sl(processor.recordLock);
            auto& seq = processor.recordedSequence;
            
            for (int i = 0; i < seq.getNumEvents(); ++i) {
                if (auto* event = seq.getEventPointer(i)) {
                    auto msg = event->message;
                    double time = msg.getTimeStamp();
                    
                    if (msg.isNoteOn()) {
                        activeNotes.push_back({ msg.getNoteNumber(), time, msg.getVelocity() / 127.0f, i });
                    } 
                    else if (msg.isNoteOff()) {
                        int noteNum = msg.getNoteNumber();
                        auto it = std::find_if(activeNotes.begin(), activeNotes.end(), 
                            [noteNum](const ActiveNote& an) { return an.noteNumber == noteNum; });
                        
                        if (it != activeNotes.end()) {
                            bool isSelected = (it->eventIndex == selectedNoteOnIndex);
                            drawNoteBlock(g, it->noteNumber, it->startTime, time, it->velocity,
                                          minNote, maxNote, maxTime, pianoRollLeft, drawWidth, rowHeight,
                                          it->eventIndex, isSelected);
                            activeNotes.erase(it);
                        }
                    }
                }
            }

            for (const auto& activeNote : activeNotes) {
                bool isSelected = (activeNote.eventIndex == selectedNoteOnIndex);
                drawNoteBlock(g, activeNote.noteNumber, activeNote.startTime, processor.recordingTimeSec, activeNote.velocity,
                              minNote, maxNote, maxTime, pianoRollLeft, drawWidth, rowHeight,
                              activeNote.eventIndex, isSelected);
            }
        }
        
        // Draw Playhead
        double playheadTime = processor.isPlayingMidi.load() ? processor.playbackTimeSec : processor.recordingTimeSec;
        if (playheadTime > 0.0) {
            float px = pianoRollLeft + (static_cast<float>(playheadTime) / static_cast<float>(maxTime)) * drawWidth;
            g.setColour(juce::Colour(0xff33c4c9));
            g.drawVerticalLine(static_cast<int>(px), 0.0f, drawHeight);
            g.setColour(juce::Colour(0x3333c4c9));
            g.fillRect(px - 1.0f, 0.0f, 3.0f, drawHeight);
        }

        // Delete hint
        if (selectedNoteOnIndex >= 0) {
            g.setColour(juce::Colour(0xaaf87171));
            g.setFont(juce::FontOptions(11.0f));
            g.drawText("Press Delete to remove", getLocalBounds().reduced(8, 4), juce::Justification::bottomRight);
        }
    }

private:
    struct NoteRect {
        juce::Rectangle<float> rect;
        int noteOnEventIndex;
    };
    std::vector<NoteRect> noteGeometry;

    int hitTestNote(int mx, int my) const {
        for (auto it = noteGeometry.rbegin(); it != noteGeometry.rend(); ++it) {
            if (it->rect.contains((float)mx, (float)my))
                return it->noteOnEventIndex;
        }
        return -1;
    }

    void drawNoteBlock(juce::Graphics& g, int noteNumber, double startTime, double endTime, float velocity,
                        int minNote, int maxNote, double maxTime, float pianoRollLeft, float drawWidth, float rowHeight,
                        int eventIndex, bool isSelected) {
        
        float xStart = pianoRollLeft + (static_cast<float>(startTime) / static_cast<float>(maxTime)) * drawWidth;
        float xEnd   = pianoRollLeft + (static_cast<float>(endTime)   / static_cast<float>(maxTime)) * drawWidth;
        float noteWidth = xEnd - xStart;
        if (noteWidth < 5.0f) noteWidth = 5.0f;
        
        int rowIdx = maxNote - noteNumber;
        float y = rowIdx * rowHeight;
        float vPad = rowHeight * 0.2f;

        juce::Rectangle<float> rect(xStart + 1.0f, y + vPad, noteWidth - 2.0f, rowHeight - (vPad * 2.0f));
        noteGeometry.push_back({ rect, eventIndex });

        if (isSelected) {
            // Selected = bright white/red outline + dimmed fill
            g.setColour(juce::Colour(0xfff87171).withAlpha(0.85f));
            g.fillRoundedRectangle(rect, 4.0f);
            g.setColour(juce::Colours::white);
            g.drawRoundedRectangle(rect, 4.0f, 1.5f);
        } else {
            juce::ColourGradient grad(juce::Colour(0xffff7c8c), xStart, y, juce::Colour(0xffffca58), xEnd, y, false);
            g.setGradientFill(grad);
            g.fillRoundedRectangle(rect, 4.0f);
        }
    }

    HumToMIDIProcessor& processor;
};


class HummbiarLookAndFeel : public juce::LookAndFeel_V4 {
public:
    HummbiarLookAndFeel() {
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff15161b));
        setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff2a2b32));
        setColour(juce::ComboBox::textColourId, juce::Colours::white);
        setColour(juce::ComboBox::arrowColourId, juce::Colour(0xffffca58));
        
        setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xffffca58));
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff2a2b32));
        setColour(juce::Slider::thumbColourId, juce::Colour(0xffffca58));
    }
    
    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override {
        auto bounds = juce::Rectangle<int>(0, 0, width, height).toFloat();
        g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle(bounds, 4.0f);
        
        g.setColour(box.findColour(juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
        
        // Draw little arrow
        juce::Path p;
        p.addTriangle(buttonX + buttonW * 0.3f, buttonY + buttonH * 0.4f,
                      buttonX + buttonW * 0.7f, buttonY + buttonH * 0.4f,
                      buttonX + buttonW * 0.5f, buttonY + buttonH * 0.6f);
        g.setColour(box.findColour(juce::ComboBox::arrowColourId));
        g.fillPath(p);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override {
        auto bounds = juce::Rectangle<float>(x, y, width, height);
        float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f - 2.0f;
        float centreX = bounds.getCentreX();
        float centreY = bounds.getCentreY();
        float rx = centreX - radius;
        float ry = centreY - radius;
        float rw = radius * 2.0f;
        
        float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        
        g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId));
        g.fillEllipse(rx, ry, rw, rw);
        
        juce::Path p;
        float pointerLength = radius * 0.7f;
        float pointerThickness = 3.0f;
        p.addRectangle(-pointerThickness * 0.5f, -radius + 2.0f, pointerThickness, pointerLength);
        p.applyTransform(juce::AffineTransform::rotation(angle).translated(centreX, centreY));
        
        g.setColour(slider.findColour(juce::Slider::thumbColourId));
        g.fillPath(p);
    }
};

class DragButton : public juce::Button {
public:
    DragButton(const juce::String& name) : juce::Button(name) {}

    std::function<void(const juce::MouseEvent&)> onDragStart;
    bool hasDragged = false;
    
    void mouseDown(const juce::MouseEvent& e) override {
        hasDragged = false;
        juce::Button::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        if (!hasDragged) {
            hasDragged = true;
            if (onDragStart) onDragStart(e);
        }
        juce::Button::mouseDrag(e);
    }
};

class RecordIconButton : public juce::Button {
public:
    RecordIconButton(HumToMIDIProcessor& p) : juce::Button("Record"), processor(p) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        auto bounds = getLocalBounds().toFloat();
        bool isRecording = processor.isRecordingMidi.load();

        juce::Colour bg = isRecording ? juce::Colour(0xffff5566) : 
                          shouldDrawButtonAsDown ? juce::Colour(0xff15161a) : 
                          shouldDrawButtonAsHighlighted ? juce::Colour(0xff2d2e36) : juce::Colour(0xff222329);
        g.setColour(bg);
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colour(0xff2a2b32));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

        float cy = bounds.getCentreY();
        float cx = bounds.getX() + 16.0f;
        
        if (isRecording) {
            g.setColour(juce::Colours::black);
            g.fillRoundedRectangle(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, 2.0f);
            
            g.setFont(juce::FontOptions(12.0f).withStyle("Bold"));
            auto textBounds = bounds;
            textBounds.removeFromLeft(27.0f);
            g.drawText("STOP", textBounds, juce::Justification::centredLeft);
        } else {
            g.setColour(juce::Colour(0xffff5566));
            g.fillEllipse(cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
            
            g.setColour(juce::Colours::white);
            g.setFont(juce::FontOptions(12.0f).withStyle("Bold"));
            auto textBounds = bounds;
            textBounds.removeFromLeft(27.0f);
            g.drawText("REC", textBounds, juce::Justification::centredLeft);
        }
    }

private:
    HumToMIDIProcessor& processor;
};

class PlayIconButton : public juce::Button {
public:
    PlayIconButton(HumToMIDIProcessor& p) : juce::Button("Play"), processor(p) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        auto bounds = getLocalBounds().toFloat();
        bool isPlaying = processor.isPlayingMidi.load();

        juce::Colour bg = isPlaying ? juce::Colour(0xff33c4c9) : 
                          shouldDrawButtonAsDown ? juce::Colour(0xff15161a) : 
                          shouldDrawButtonAsHighlighted ? juce::Colour(0xff2d2e36) : juce::Colour(0xff222329);
        g.setColour(bg);
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colour(0xff2a2b32));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

        g.setColour(isPlaying ? juce::Colours::black : juce::Colours::white);
        float cx = bounds.getCentreX();
        float cy = bounds.getCentreY();

        if (isPlaying) {
            // Pause icon
            g.fillRoundedRectangle(cx - 5.0f, cy - 6.0f, 3.5f, 12.0f, 1.0f);
            g.fillRoundedRectangle(cx + 1.5f, cy - 6.0f, 3.5f, 12.0f, 1.0f);
        } else {
            // Play triangle
            juce::Path p;
            p.addTriangle(cx - 3.5f, cy - 6.5f, cx - 3.5f, cy + 6.5f, cx + 5.5f, cy);
            g.fillPath(p);
        }
    }

private:
    HumToMIDIProcessor& processor;
};

class LoopIconButton : public juce::Button {
public:
    LoopIconButton(HumToMIDIProcessor& p) : juce::Button("Loop"), processor(p) {
        setClickingTogglesState(true);
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        auto bounds = getLocalBounds().toFloat();
        bool isLooping = processor.isLooping.load();

        juce::Colour bg = isLooping ? juce::Colour(0xff33c4c9) : 
                          shouldDrawButtonAsDown ? juce::Colour(0xff15161a) : 
                          shouldDrawButtonAsHighlighted ? juce::Colour(0xff2d2e36) : juce::Colour(0xff222329);
        g.setColour(bg);
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colour(0xff2a2b32));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

        g.setColour(isLooping ? juce::Colours::black : juce::Colours::white);
        float cx = bounds.getCentreX();
        float cy = bounds.getCentreY();
        float r = 5.5f;

        // Circular loop arrows
        juce::Path p;
        p.addCentredArc(cx, cy, r, r, 0.0f, 0.2f * juce::MathConstants<float>::pi, 1.55f * juce::MathConstants<float>::pi, true);
        g.strokePath(p, juce::PathStrokeType(1.8f));

        juce::Path tip;
        tip.addTriangle(cx + r - 3.0f, cy - 3.5f, cx + r + 3.0f, cy - 3.5f, cx + r, cy + 2.0f);
        g.fillPath(tip);
    }

private:
    HumToMIDIProcessor& processor;
};

class CopyMidiIconButton : public DragButton {
public:
    CopyMidiIconButton() : DragButton("Midi") {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        auto bounds = getLocalBounds().toFloat();
        juce::Colour bg = shouldDrawButtonAsDown ? juce::Colour(0xff15161a) : 
                          shouldDrawButtonAsHighlighted ? juce::Colour(0xff2d2e36) : juce::Colour(0xff222329);
        g.setColour(bg);
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colour(0xff2a2b32));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

        float cy = bounds.getCentreY();
        g.setColour(juce::Colour(0xffffca58));

        // Arrow pointing down into document / tray
        juce::Path arrow;
        float x = bounds.getX() + 15.0f;
        arrow.addRectangle(x - 1.5f, cy - 6.0f, 3.0f, 7.0f);
        arrow.addTriangle(x - 4.5f, cy + 1.0f, x + 4.5f, cy + 1.0f, x, cy + 6.0f);
        g.fillPath(arrow);

        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(12.0f).withStyle("Bold"));
        auto textBounds = bounds;
        textBounds.removeFromLeft(24.0f);
        g.drawText("MIDI", textBounds, juce::Justification::centredLeft);
    }
};

class CalibrateIconButton : public juce::Button {
public:
    CalibrateIconButton(HumToMIDIProcessor& p) : juce::Button("Calibrate"), processor(p) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override {
        auto bounds = getLocalBounds().toFloat();
        bool isCal = processor.isCalibrating.load();

        juce::Colour bg = isCal ? juce::Colour(0xff33c4c9) : 
                          shouldDrawButtonAsDown ? juce::Colour(0xff15161a) : 
                          shouldDrawButtonAsHighlighted ? juce::Colour(0xff2d2e36) : juce::Colour(0xff222329);
        g.setColour(bg);
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour(juce::Colour(0xff2a2b32));
        g.drawRoundedRectangle(bounds, 4.0f, 1.0f);

        float cy = bounds.getCentreY();
        g.setColour(isCal ? juce::Colours::black : juce::Colour(0xff33c4c9));

        // Lightning bolt vector
        juce::Path bolt;
        float x = bounds.getX() + 14.0f;
        bolt.startNewSubPath(x + 1.0f, cy - 6.0f);
        bolt.lineTo(x - 4.0f, cy + 1.0f);
        bolt.lineTo(x, cy + 1.0f);
        bolt.lineTo(x - 1.0f, cy + 6.0f);
        bolt.lineTo(x + 4.0f, cy - 1.0f);
        bolt.lineTo(x, cy - 1.0f);
        bolt.closeSubPath();
        g.fillPath(bolt);

        g.setColour(isCal ? juce::Colours::black : juce::Colours::white);
        g.setFont(juce::FontOptions(12.0f).withStyle("Bold"));

        auto textBounds = bounds;
        textBounds.removeFromLeft(22.0f);

        if (isCal) {
            int pct = static_cast<int>(processor.calibrationProgress.load() * 100.0f);
            g.drawText(juce::String(pct) + "%", textBounds, juce::Justification::centredLeft);
        } else {
            g.drawText("CAL", textBounds, juce::Justification::centredLeft);
        }
    }

private:
    HumToMIDIProcessor& processor;
};

class HumToMIDIEditor : public juce::AudioProcessorEditor, 
                         public juce::Timer,
                         public juce::DragAndDropContainer {
public:
    HumToMIDIEditor(HumToMIDIProcessor&);
    ~HumToMIDIEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    
    // Allows external drop from the "COPY MIDI" button
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

private:
    HumToMIDIProcessor& audioProcessor;
    HummbiarLookAndFeel customLookAndFeel;
    
    PianoRollVisualizer visualizer;

    // Header Components
    juce::Label titleLabel;
    juce::Label presetLabel;
    juce::ComboBox presetSelector;
    CalibrateIconButton calibrateButton;
    CopyMidiIconButton copyMidiButton;
    LoopIconButton loopButton;
    PlayIconButton playButton;
    RecordIconButton recordButton;
    
    // Bottom Controls - Sound, Chord & Quantize
    juce::Label waveformLabel;
    juce::ComboBox waveformSelector;
    
    juce::Label chordLabel;
    juce::ComboBox chordSelector;
    
    juce::Label quantizeLabel;
    juce::ComboBox quantizeSelector;
    
    juce::Slider quantizeStrengthSlider;
    juce::Label quantizeStrengthLabel;
    
    // 6 Master Calibration Sliders
    juce::Slider sensitivitySlider;
    juce::Label sensitivityLabel;

    juce::Slider noiseGateSlider;
    juce::Label noiseGateLabel;

    juce::Slider attackSpeedSlider;
    juce::Label attackSpeedLabel;

    juce::Slider pitchStabilitySlider;
    juce::Label pitchStabilityLabel;

    juce::Slider octaveLockSlider;
    juce::Label octaveLockLabel;

    juce::Slider minNoteSlider;
    juce::Label minNoteLabel;

    // Status bar labels
    juce::Label statusLiveSignal;
    juce::Label statusRms;
    juce::Label statusPitch;
    juce::Label statusBpm;

    // Key and Scale Dropdowns
    juce::Label keyLabel;
    juce::ComboBox keySelector;
    juce::Label scaleLabel;
    juce::ComboBox scaleSelector;

    // Auto Key Detection
    juce::TextButton autoKeyButton;
    juce::Label detectedKeyLabel;

    juce::File tempMidiFile;
    bool isReadyToDrag = false;

    void updateSliderValuesFromProcessor();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HumToMIDIEditor)
};
