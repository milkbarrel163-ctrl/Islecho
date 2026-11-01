#include "EcologySimulation.h"
#include "LifeDynamics.h"
#include <algorithm>
#include <cmath>

std::vector<EcologyIntent> EcologySimulation::step(
    EnvironmentState& env,
    std::vector<SoundObject>& objects,
    const std::set<int32_t>& held,
    double time)
{
    births.clear();
    emergences.clear();
    if (!std::isfinite(time)) { resetTime(); return {}; }
    if (!env.isRunning) { resetTime(); return {}; }

    const double prev = previousTime;
    previousTime = time;
    if (prev < 0 || time <= prev) return {};

    const double dt = std::min(time - prev, 1.0 / 30.0);

    // Weather
    if (!env.weatherCycle) env.weatherCycle = WeatherCycle{};
    env.weatherCycle->step(dt, env.weather, env.automaticWeather);

    // Day phase
    EnvironmentState mid = env;
    mid.elapsed += dt * 0.5;
    mid.dayPhase = std::fmod(env.dayPhase + dt / (2.0 * env.dayDuration), 1.0);
    env.dayPhase = std::fmod(env.dayPhase + dt / env.dayDuration, 1.0);
    env.elapsed  += dt;

    // Succession
    const bool night = env.dayPhase < 0.25 || env.dayPhase > 0.75;
    auto emg = succession.step(dt, night, objects);
    emergences = std::move(emg);

    // Per-object event scheduling
    std::vector<double> plantX, plantY;
    for (auto& o : objects)
        if (o.ecology && o.ecology->kind == EcologyKind::plant && !o.departure &&
            o.ecology->life && o.ecology->life->death == LifeDeath::none)
            plantX.push_back(o.x), plantY.push_back(o.y);

    const int steps = std::max(1, std::min(2, static_cast<int>(std::ceil(dt / (1.0/60 + 0.0005)))));
    const double h = dt / steps;

    std::set<int32_t> pairedBirths;
    for (int s = 0; s < steps; ++s) {
        // Life dynamics: age, predation, reproduction, wind, firefly flight
        for (auto& birth : LifeDynamics::step(objects, held, mid, h)) {
            if (!pairedBirths.count(birth.parentA) && !pairedBirths.count(birth.parentB)) {
                births.push_back(birth);
                pairedBirths.insert(birth.parentA);
                pairedBirths.insert(birth.parentB);
            }
        }

        // Autonomous swimming for fish that have no life (legacy / plain define'd fish)
        for (auto& o : objects) {
            if (!o.ecology) continue;
            if (held.count(o.id)) { if (o.ecology->swimming) o.ecology->swimming->stopForTouch(); continue; }
            if (o.departure) continue;
            // LifeDynamics handles fish-with-life swimming via stepToward; only advance
            // life-less fish here (shouldn't happen in practice after factory init).
            if (o.ecology->kind == EcologyKind::fish && !o.ecology->life) {
                if (o.ecology->swimming)
                    o.ecology->swimming->step(o.x, o.y, h, mid.sunlight());
            }
        }
    }

    std::vector<EcologyIntent> intents;
    const double panic = std::clamp(env.storm, 0.0, 1.0);
    for (auto& o : objects) {
        if (!o.ecology) continue;
        if (o.departure) continue;
        auto& body = *o.ecology;
        if (held.count(o.id)) continue;

        if (body.motion && body.motion->attackPending) {
            intents.push_back({ o.id, body.kind });
            body.motion->attackPending = false;
        }

        // Nearby plant count for fireflies
        int nearbyPlants = 0;
        if (body.kind == EcologyKind::firefly) {
            for (size_t pi = 0; pi < plantX.size(); ++pi) {
                double dx = plantX[pi] - o.x, dy = plantY[pi] - o.y;
                if (dx*dx + dy*dy <= 0.0625) ++nearbyPlants;
            }
        }

        const double normalRate = eventsPerSecond(body, o.x, o.y, mid, nearbyPlants);
        const int nLive = (int)objects.size();
        const double alarmRate = std::min(3.5, 150.0 / std::max(1, nLive)) *
                                 (0.75 + 0.25 * xorunit(body.seed)) * panic;
        const double rate = (body.life && body.life->death == LifeDeath::none) ?
                            std::max(normalRate, alarmRate) : 0.0;
        if (rate <= 0) continue;

        body.cycleProgress += dt * rate;
        if (body.cycleProgress >= 1.0) {
            const double remainingTime = (body.cycleProgress - 1.0) / rate;
            body.seed = xorshift(body.seed);
            const double nextRate = std::max(eventsPerSecond(body, o.x, o.y, mid, nearbyPlants), alarmRate);
            body.cycleProgress = remainingTime * nextRate;
            intents.push_back({ o.id, body.kind, panic });
        }
    }
    return intents;
}
