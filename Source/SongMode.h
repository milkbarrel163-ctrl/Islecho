#pragma once
#include "IslechoTypes.h"
#include "PulseDSP.h"
#include <array>
#include <cmath>
#include <random>
#include <vector>
#include <map>
#include <optional>

// ─── RhythmLearning ──────────────────────────────────────────────────────────
// Port of Swift RhythmLearning. Learns only completed short contacts using
// monotonic touch timestamps and the exact note chosen for that contact.

struct RhythmLearning {
    struct Tap { int32_t id; int midi; double time; };

    std::vector<Tap> taps;
    int sinceSubmission = 0;

    void reset() { taps.clear(); sinceSubmission = 0; }

    // Returns a filled PFSongScore on success, nullopt otherwise.
    std::optional<PFSongScore> record(int32_t id, float frequency, double time,
                                       MusicalScaleType scaleType, int root) {
        if (id <= 0 || !std::isfinite(frequency) || frequency <= 0 || !std::isfinite(time))
            { reset(); return std::nullopt; }

        // MIDI from frequency
        const int midi = static_cast<int>(std::round(69.0 + 12.0 * std::log2(frequency / 440.0)));

        // Must be in scale
        auto notes = MusicalScale::notes(musicalScaleTypeToEnum(scaleType), root);
        bool inScale = false;
        for (int n : notes) if (n == midi) { inScale = true; break; }
        if (!inScale) { reset(); return std::nullopt; }

        // Gap filter
        if (!taps.empty()) {
            const double gap = time - taps.back().time;
            if (gap < 0.16 || gap > 1.8) reset();
        }

        taps.push_back({ id, midi, time });
        ++sinceSubmission;
        if ((int)taps.size() > 8) taps.erase(taps.begin());

        if ((int)taps.size() < 5 || sinceSubmission < 4) return std::nullopt;

        // Compute inter-onset gaps
        std::vector<double> gaps;
        for (int i = 1; i < (int)taps.size(); ++i)
            gaps.push_back(taps[i].time - taps[i-1].time);

        // Candidate period detection: same logic as Swift
        std::vector<std::pair<double,double>> candidates; // (score, period)
        for (double gap : gaps) {
            for (double multiple : {0.5, 1.0, 1.5, 2.0}) {
                const double period = gap / multiple;
                const double bpm = 60.0 / period;
                if (bpm < 60.0 || bpm > 160.0) continue;
                double error = 0; int quarters = 0; bool valid = true;
                for (double dt : gaps) {
                    const double steps = std::round(dt / period * 2.0);
                    const double distance = std::abs(dt - steps * period / 2.0);
                    if (steps < 1 || steps > 4 || distance > std::min(0.055, period * 0.12))
                        { valid = false; break; }
                    error += distance / period;
                    if (static_cast<int>(steps) == 2) ++quarters;
                }
                if (valid && quarters >= 2)
                    candidates.push_back({ error - quarters * 0.06 + std::abs(bpm - 110) * 0.0001, period });
            }
        }
        if (candidates.empty()) return std::nullopt;
        auto best = std::min_element(candidates.begin(), candidates.end(),
                                     [](auto& a, auto& b){ return a.first < b.first; });
        const double period = best->second;

        // Build the score
        PFSongScore score{};
        score.count = static_cast<int32_t>(taps.size());
        score.root  = static_cast<int32_t>(root);
        score.bpm   = 60.0 / period;
        score.durationSeconds = 60.0;

        auto intervals = MusicalScale::intervals(musicalScaleTypeToEnum(scaleType));
        score.scaleCount = static_cast<int32_t>(intervals.size());
        for (int i = 0; i < (int)intervals.size() && i < 7; ++i)
            score.scale[i] = static_cast<int32_t>(intervals[i]);

        // Compute beat offsets
        std::vector<double> offsets(taps.size(), 0.0);
        for (int i = 1; i < (int)taps.size(); ++i)
            offsets[i] = offsets[i-1] + std::round((taps[i].time - taps[i-1].time) / period * 2.0) / 2.0;
        score.lengthBeats = std::ceil((offsets.back() + 1.0) / 4.0) * 4.0;

        for (int i = 0; i < (int)taps.size(); ++i)
            score.notes[i] = PFSongNote{ taps[i].id, static_cast<int32_t>(taps[i].midi), offsets[i] };

        sinceSubmission = 0;
        return score;
    }

private:
    static MusicalScale::Type musicalScaleTypeToEnum(MusicalScaleType t) {
        switch (t) {
        case MusicalScaleType::minor:          return MusicalScale::minor;
        case MusicalScaleType::minorPentatonic: return MusicalScale::minorPentatonic;
        case MusicalScaleType::major:          return MusicalScale::major;
        default:                               return MusicalScale::pentatonic;
        }
    }
};

// ─── EcologyMusicBridge ───────────────────────────────────────────────────────
// Schedules bounded ecology intents as audio commands (pf_trigger_alarm).
// Tracks one pending command per entity to avoid flooding the queue.

struct EcologyMusicBridge {
    struct Pending { double requestedBeat, scheduledBeat; };
    std::map<int32_t, Pending> pending;

    void reset() { pending.clear(); }

    // Schedule up to 8 intents per call.
    // Returns number submitted.
    int schedule(const std::vector<EcologyIntent>& intents,
                 const std::vector<SoundObject>& objects,
                 const EnvironmentState& env,
                 PFCore* core,
                 const CompositionSettings& settings,
                 std::mt19937& pitchRng)
    {
        const auto clock = pf_clock(core);
        const double passedBeat = clock.beat - clock.bpm / (60.0 * clock.sampleRate) - 1e-9;

        // Build eligible set and prune stale pending
        std::set<int32_t> existing;
        for (auto& o : objects) existing.insert(o.id);
        for (auto it = pending.begin(); it != pending.end(); ) {
            if (!existing.count(it->first) || it->second.scheduledBeat < passedBeat)
                it = pending.erase(it);
            else ++it;
        }

        // Build eligible IDs (alive, no departure, in habitat)
        std::set<int32_t> eligible;
        for (auto& o : objects) {
            if (o.departure) continue;
            if (!o.ecology) continue;
            if (o.ecology->life && o.ecology->life->death != LifeDeath::none) continue;
            if (!habitatOccupies(o.ecology->kind, o.x, o.y)) continue;
            eligible.insert(o.id);
        }

        int submitted = 0;
        for (auto& intent : intents) {
            if (submitted >= 8) break;
            if (!eligible.count(intent.id)) continue;
            if (pending.count(intent.id)) continue;

            // Find the object
            auto it = std::find_if(objects.begin(), objects.end(),
                                   [&](const SoundObject& o){ return o.id == intent.id; });
            if (it == objects.end()) continue;
            if (it->ecology->kind != intent.kind) continue;

            const double alarm = std::min(1.0, std::max(0.0, intent.alarm));
            if (alarm <= 0.08) {
                // Skip hiding fish not in water
                if (it->ecology->life && it->ecology->life->behavior == LifeBehavior::hiding) continue;
                if (intent.kind == EcologyKind::fish && !habitatAccepts(EcologyKind::fish, it->x, it->y)) continue;
            }

            const float freq = nextNoteHz(settings, pitchRng, alarm);
            const float cutoff = cutoffForKind(intent.kind, env);
            const float pan = static_cast<float>(2.0 * it->x - 1.0);
            const double target = clock.beat;

            if (!pf_trigger_alarm(core, it->id, static_cast<int>(it->material),
                                  freq, pan, target, cutoff, static_cast<float>(alarm)))
                break; // queue full – drop remaining

            pending[it->id] = { target, target };
            ++submitted;
        }
        return submitted;
    }

    void acknowledge(const PFScheduleAck& ack) {
        auto it = pending.find(ack.id);
        if (it == pending.end()) return;
        if (std::abs(it->second.requestedBeat - ack.requestedBeat) < 1e-6)
            it->second.scheduledBeat = ack.scheduledBeat;
    }

private:
    static float nextNoteHz(const CompositionSettings& s, std::mt19937& rng, double alarm) {
        MusicalScale::Type t = MusicalScale::pentatonic;
        switch (s.scale) {
        case MusicalScaleType::minor:          t = MusicalScale::minor; break;
        case MusicalScaleType::minorPentatonic: t = MusicalScale::minorPentatonic; break;
        case MusicalScaleType::major:          t = MusicalScale::major; break;
        default: break;
        }
        auto notes = MusicalScale::notes(t, s.root);
        if (alarm > 0.08) {
            std::vector<int> high;
            for (int n : notes) if (n >= 72 + s.root) high.push_back(n);
            if (!high.empty()) {
                std::uniform_int_distribution<int> d(0, (int)high.size()-1);
                return MusicalScale::midiToHz(high[d(rng)]);
            }
        }
        std::uniform_int_distribution<int> d(0, (int)notes.size()-1);
        return MusicalScale::midiToHz(notes[d(rng)]);
    }

    static float cutoffForKind(EcologyKind kind, const EnvironmentState& env) {
        const double sl = env.sunlight();
        switch (kind) {
        case EcologyKind::fish:    return static_cast<float>(500.0 + sl * 1500.0);
        case EcologyKind::plant:   return static_cast<float>(500.0 + sl * 1500.0);
        case EcologyKind::firefly: return static_cast<float>(800.0 + (1.0 - sl) * 1200.0);
        }
        return 1000.f;
    }
};
