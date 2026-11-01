#pragma once
#include "IslechoTypes.h"
#include "PulseDSP.h"
#include "WaterCrests.h"
#include <array>
#include <cmath>
#include <limits>

// Drives the eight EnvironmentSynth wave channels from world state.
// Port of Swift EnvironmentAudioBridge. The visible and audible crests use
// the same world-space patch positions and heights.
struct EnvironmentAudioBridge {
    double lastTime = -std::numeric_limits<double>::infinity();
    bool wasEnabled = false;

    void reset() {
        lastTime = -std::numeric_limits<double>::infinity();
        wasEnabled = false;
    }

    // Call once per timer tick (30 Hz). Rate-limits internally.
    void update(const EnvironmentState& env, const CompositionSettings& settings,
                PFCore* core, double time, bool enabled) {
        if (enabled == wasEnabled && time >= lastTime && time - lastTime < 1.0 / 30.0) return;
        auto frame = snapshot(env, settings, env.elapsed, enabled);
        if (pf_set_environment(core, &frame)) {
            lastTime = time;
            wasEnabled = enabled;
        }
    }

    static PFEnvironmentSound snapshot(const EnvironmentState& env,
                                        const CompositionSettings& settings,
                                        double time, bool enabled) {
        PFEnvironmentSound frame{};
        frame.enabled = enabled;
        frame.wind    = windBreath(env);

        // Pick notes from the current scale
        MusicalScale::Type scaleType = MusicalScale::pentatonic;
        switch (settings.scale) {
        case MusicalScaleType::minor:           scaleType = MusicalScale::minor;          break;
        case MusicalScaleType::minorPentatonic: scaleType = MusicalScale::minorPentatonic; break;
        case MusicalScaleType::major:           scaleType = MusicalScale::major;           break;
        default: break;
        }
        const auto notes = MusicalScale::notes(scaleType, settings.root);
        const int noteCount = static_cast<int>(notes.size());

        for (int i = 0; i < PF_WAVE_CHANNELS; ++i) {
            const auto wave = WaterCrests::patch(i, time);
            const float rise = wave.height / 1.15f;
            const float y = std::clamp((wave.z / 1.5f + 8.f) / 14.f, 0.f, 1.f);
            const int idx = static_cast<int>(
                std::round((1.f - y) * (noteCount - 1) * 0.66f));
            const int noteIdx = std::max(0, std::min(idx, noteCount - 1));

            const float cutoff = 500.f + 1500.f * rise;

            // Pan from geographic X
            const float pan = std::clamp(wave.x / (11.f * 1.5f), -1.f, 1.f);

            const float gain = rise;

            frame.waves[i] = PFWaveSound{
                MusicalScale::midiToHz(notes[noteIdx]),
                pan,
                cutoff,
                gain
            };
        }
        return frame;
    }

    // Wind intensity → wind synthesis level
    static float windBreath(const EnvironmentState& env) {
        return static_cast<float>(
            std::min(1.0, env.windyAmount() * 0.65 + env.rainAmount() * 0.35));
    }
};
