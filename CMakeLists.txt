cmake_minimum_required(VERSION 3.20)
project(HumToMIDI VERSION 1.0.0)

add_subdirectory(JUCE)

juce_add_plugin(HumToMIDI
    PLUGIN_MANUFACTURER_CODE Juce
    PLUGIN_CODE Htmd
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT TRUE
    PRODUCT_NAME "HumToMIDI")

juce_generate_juce_header(HumToMIDI)

target_sources(HumToMIDI PRIVATE
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp
    Source/PluginEditor.h
    Source/PluginEditor.cpp
    Source/PitchDetector.h
    Source/PitchDetector.cpp
    Source/NoteTracker.h
    Source/NoteTracker.cpp
    Source/MidiConverter.h
    Source/MidiConverter.cpp
    Source/InstrumentPresets.h
    Source/InstrumentPresets.cpp
)

target_link_libraries(HumToMIDI PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_plugin_client
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

target_compile_definitions(HumToMIDI PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0)
