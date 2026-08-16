#include "InstrumentPresets.h"

InputSourceProfile InstrumentPresets::getProfile(InputSourceType type) {
    switch (type) {
        case InputSourceType::VocalLead:
            return {
                "Vocal / Humming",
                1.0f,      // Sensitivity
                0.0015f,   // Noise Gate Cutoff
                18.0f,     // Attack Speed Ms
                75.0f,     // Pitch Stability Cents
                0.90f,     // Octave Lock
                80.0f,     // Min Note Duration Ms
                80.0f      // HPF Cutoff Hz
            };

        case InputSourceType::VocalSolfege:
            return {
                "Vocal Scale (Do-Re-Mi)",
                1.1f,      // Sensitivity
                0.0012f,   // Noise Gate Cutoff
                14.0f,     // 14ms Attack Speed for fast scale steps
                60.0f,     // 60c Pitch Stability Cents
                0.92f,     // 0.92 Octave Lock
                65.0f,     // 65ms Min Note Duration (prevents glide scoops)
                90.0f      // 90Hz HPF Cutoff Hz
            };

        case InputSourceType::Whistling:
            return {
                "Whistling",
                1.2f,      // Higher sensitivity for subtle whistle air
                0.0008f,   // Low noise gate (whistle sound is pure/quiet)
                8.0f,      // Ultra-fast 8ms attack speed
                45.0f,     // Low stability cents for fast pitch slides
                0.40f,     // Low octave lock (whistles have no subharmonics)
                35.0f,     // Short 35ms min note duration for rapid trills
                450.0f     // 450Hz High-Pass to cut out room rumble & chest hum
            };

        case InputSourceType::Guitar:
            return {
                "Guitar / Plucked",
                1.1f,      // Sensitivity
                0.0012f,   // Noise Gate Cutoff
                12.0f,     // Fast 12ms attack for pick transients
                65.0f,     // Pitch Stability Cents
                0.80f,     // Octave Lock
                50.0f,     // Min Note Duration Ms
                100.0f     // HPF Cutoff Hz
            };

        case InputSourceType::Bass:
            return {
                "Bass / Low Voice",
                1.3f,      // High Sensitivity for deep fundamentals
                0.0018f,   // Noise Gate Cutoff
                25.0f,     // Smooth 25ms attack to process long low waves
                85.0f,     // High pitch stability cents
                0.96f,     // Maximum Octave Lock to prevent 2nd harmonic jump
                110.0f,    // Min Note Duration Ms
                35.0f      // Deep 35Hz HPF Cutoff Hz
            };

        case InputSourceType::WindBrass:
            return {
                "Wind / Brass / Sax",
                1.0f,      // Sensitivity
                0.0018f,   // Firmer Noise Gate against breath puff
                16.0f,     // Attack Speed Ms
                80.0f,     // High stability cents (absorbs wide vibrato)
                0.85f,     // Octave Lock
                75.0f,     // Min Note Duration Ms
                140.0f     // HPF Cutoff Hz
            };

        case InputSourceType::Percussion:
            return {
                "Beatbox / Percussion",
                1.4f,      // High Sensitivity for rapid transients
                0.0020f,   // Firm Gate against background noise
                6.0f,      // Ultra-instant 6ms attack
                90.0f,     // High stability
                0.50f,     // Octave Lock
                30.0f,     // Short 30ms min note duration
                60.0f      // HPF Cutoff Hz
            };

        default:
            return getProfile(InputSourceType::VocalLead);
    }
}

InputSourceProfile InstrumentPresets::getProfileByIndex(int index) {
    if (index < 0 || index > 6) index = 0;
    return getProfile(static_cast<InputSourceType>(index));
}

std::vector<std::string> InstrumentPresets::getSourceNames() {
    return {
        "Vocal / Humming",
        "Vocal Scale (Do-Re-Mi)",
        "Whistling",
        "Guitar / Plucked",
        "Bass / Low Hum",
        "Wind / Brass",
        "Beatbox / Percussion"
    };
}
