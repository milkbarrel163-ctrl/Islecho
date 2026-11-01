#pragma once
#include "IslechoTypes.h"
#include <vector>
#include <set>
#include <optional>

// ─── Ecology intent / birth / emergence ─────────────────────────────────────

struct EcologyIntent {
    int32_t id;
    EcologyKind kind;
    double alarm = 0;
};

struct EcologyBirth {
    int32_t parentA, parentB;
    double x, y;
    uint32_t seed;
    uint32_t species;
    int generation;
};

struct EcologyEmergence {
    EcologyKind kind;
    double x, y;
    uint32_t seed;
};

// ─── Succession ──────────────────────────────────────────────────────────────

struct EcologySuccession {
    uint32_t random = 0x6E624EB7;
    double flowerWait = 5, flyWait = 9, fishWait = 17;

    double unit() {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return static_cast<double>(random) / static_cast<double>(0xFFFFFFFFu);
    }

    EcologyEmergence emergence(EcologyKind kind) {
        double u = 0.5, v = 0.85;
        for (int attempt = 0; attempt < 48; ++attempt) {
            double tu, tv;
            if (kind == EcologyKind::fish) {
                tu = -0.28 + unit() * 1.56; tv = -0.28 + unit() * 1.56;
            } else {
                tu = unit(); tv = unit() * 0.60;
            }
            if (habitatAccepts(kind, tu, tv)) { u = tu; v = tv; break; }
        }
        boundedPosition(u, v);
        (void)unit();
        return { kind, u, v, random };
    }

    std::vector<EcologyEmergence> step(double dt, bool night,
                                       const std::vector<SoundObject>& objects) {
        flowerWait = std::max(0.0, flowerWait - dt * 1.2);
        fishWait   = std::max(0.0, fishWait   - dt);
        flyWait    = night ? std::max(0.0, flyWait - dt * 1.2) : 5.0;

        int alive = 0;
        for (auto& o : objects)
            if (!o.departure && o.ecology && o.ecology->life &&
                o.ecology->life->death == LifeDeath::none) ++alive;
        if (alive >= 36 || (int)objects.size() >= 64) return {};

        std::optional<EcologyKind> selected;
        if (flowerWait <= 0) {
            flowerWait = 12 + unit() * 18;
            int plants = 0;
            for (auto& o : objects)
                if (!o.departure && o.ecology && o.ecology->kind == EcologyKind::plant &&
                    o.ecology->life && o.ecology->life->death == LifeDeath::none) ++plants;
            if (plants < 12) selected = EcologyKind::plant;
        } else if (flyWait <= 0) {
            flyWait = 7 + unit() * 12;
            if (night) {
                int flies = 0;
                for (auto& o : objects)
                    if (!o.departure && o.ecology && o.ecology->kind == EcologyKind::firefly &&
                        o.ecology->life && o.ecology->life->death == LifeDeath::none) ++flies;
                if (flies < 10) selected = EcologyKind::firefly;
            }
        } else if (fishWait <= 0) {
            fishWait = 24 + unit() * 30;
            int fish = 0;
            for (auto& o : objects)
                if (!o.departure && o.ecology && o.ecology->kind == EcologyKind::fish &&
                    o.ecology->life && o.ecology->life->death == LifeDeath::none) ++fish;
            if (fish < 7) selected = EcologyKind::fish;
        }
        if (!selected) return {};
        return { emergence(*selected) };
    }
};

// ─── EcologySimulation ────────────────────────────────────────────────────────

class EcologySimulation {
public:
    std::vector<EcologyBirth> births;
    std::vector<EcologyEmergence> emergences;

    void resetTime() { previousTime = -1.0; }

    static double eventsPerSecond(const EcologyBody& body, double x, double y,
                                  const EnvironmentState& env, int nearbyPlants = 0) {
        if (!habitatOccupies(body.kind, x, y)) return 0;
        if (body.life && body.life->death != LifeDeath::none) return 0;
        if (body.life && body.life->behavior == LifeBehavior::hiding) return 0;
        if (body.kind == EcologyKind::fish && !habitatAccepts(EcologyKind::fish, x, y)) return 0;
        const double variation = xorunit(xorshift(body.seed));
        const bool rain = env.weather == EcologyWeather::rain;
        switch (body.kind) {
        case EcologyKind::fish: {
            const double activity = (rain ? 1.18 : 1.0) * (0.82 + 0.18 * env.daylight());
            return activity / (5.0 + 4.0 * variation);
        }
        case EcologyKind::plant: {
            const double activity = 0.16 + env.wind * 0.9 + (rain ? 0.35 : 0.0);
            return activity / (4.0 + 3.0 * variation);
        }
        case EcologyKind::firefly: {
            if (env.dayPhase >= 0.25 && env.dayPhase <= 0.75) return 0;
            const double darkness = std::max(0.0, -std::sin((env.dayPhase - 0.25) * 2.0 * M_PI));
            const double twilight = std::min(1.0, darkness * 3.0);
            const double shelter = 1.0 + 0.12 * std::min(3, std::max(0, nearbyPlants));
            return std::min(0.8, twilight * shelter * (rain ? 0.6 : 1.0) /
                            (1.8 + 1.6 * variation));
        }
        }
        return 0;
    }

    std::vector<EcologyIntent> step(EnvironmentState& env,
                                     std::vector<SoundObject>& objects,
                                     const std::set<int32_t>& held,
                                     double time);

private:
    double previousTime = -1.0;
    EcologySuccession succession;
};
