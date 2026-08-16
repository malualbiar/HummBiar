#pragma once
#include <vector>
#include <string>

enum class InputSourceType {
    VocalLead = 0,
    Whistling,
    Guitar,
    Bass,
    WindBrass,
    Percussion
};

struct InputSourceProfile {
    std::string name;
    float sensitivity;
    float noiseGateCutoff;
    float attackSpeedMs;
    float pitchStabilityCents;
    float octaveLock;
    float minNoteDurationMs;
    float hpfCutoffHz;
};

class InstrumentPresets {
public:
    static InputSourceProfile getProfile(InputSourceType type);
    static InputSourceProfile getProfileByIndex(int index);
    static std::vector<std::string> getSourceNames();
};
