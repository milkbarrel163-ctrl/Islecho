#pragma once
#include "IslechoTypes.h"
#include "EcologySimulation.h"
#include <vector>
#include <set>
#include <cmath>
#include <algorithm>

// Port of Swift LifeDynamics. Drives age, predation, reproduction, wind and
// firefly flight entirely in 2D world coordinates. No 3D contacts or geometry.
namespace LifeDynamics {

// ─── Shelter system (the eight water refuges from the iOS world) ────────────

struct Refuge { int id; double u, v; };
static constexpr double kRefugeCapacity = 0.68;
static const Refuge kRefuges[] = {
    {0, 0.20, 1.10}, {1, 0.43, 1.17}, {2, 0.65, 1.10},
    {3, 0.83, 1.18}, {4, -0.28, 0.25}, {5, 1.28, 0.27},
    {6, 0.24, -0.30}, {7, 0.76, -0.30}
};
static constexpr int kRefugeCount = 8;

static double refugeDist(double ax, double ay, double bx, double by) {
    return std::hypot(ax - bx, (ay - by) * 0.8);
}
static const Refuge* findRefuge(int id) {
    for (const auto& r : kRefuges) if (r.id == id) return &r;
    return nullptr;
}
static bool protects(const EcologyBody& body, double px, double py) {
    if (!body.life || body.life->shelterID < 0) return false;
    const Refuge* r = findRefuge(body.life->shelterID);
    return r && body.life->size <= kRefugeCapacity
              && body.life->depthFraction > 0.80
              && refugeDist(px, py, r->u, r->v) < 0.009;
}

// ─── Main step ───────────────────────────────────────────────────────────────

static constexpr double kMaxSize  = 1.85;
static constexpr double kMaturity = 30.0;

inline std::vector<EcologyBirth> step(
    std::vector<SoundObject>& objects,
    const std::set<int32_t>& held,
    const EnvironmentState& env,
    double dt)
{
    // Build fish index for predation / reproduction lookups
    std::vector<int> fishIdx;
    for (int i = 0; i < (int)objects.size(); ++i) {
        const auto& o = objects[i];
        if (o.ecology && o.ecology->kind == EcologyKind::fish
            && o.ecology->life && o.ecology->life->death == LifeDeath::none
            && !o.departure && !held.count(o.id)
            && habitatAccepts(EcologyKind::fish, o.x, o.y))
            fishIdx.push_back(i);
    }
    const auto snapshot = objects; // read-only pose snapshot for targeting

    // ── Per-object update ────────────────────────────────────────────────────
    for (int idx = 0; idx < (int)objects.size(); ++idx) {
        auto& o = objects[idx];
        if (o.departure || !o.ecology) continue;
        auto& body = *o.ecology;
        if (!body.life || body.life->death != LifeDeath::none) continue;
        if (held.count(o.id)) continue;
        if (!habitatOccupies(body.kind, o.x, o.y)) continue;

        auto& life = *body.life;
        double px = o.x, py = o.y;

        // Age + breed cooldown
        life.age = std::min(600.0, life.age + dt);
        life.breedCooldown = std::max(0.0, life.breedCooldown - dt);

        // ── Fish ─────────────────────────────────────────────────────────────
        if (body.kind == EcologyKind::fish) {
            if (!habitatAccepts(EcologyKind::fish, px, py)) {
                // Beached
                if (body.motion) {
                    body.motion->beachedTime = (body.motion->beachedTime < 0 ? 0
                        : std::min(4.0, body.motion->beachedTime + dt));
                    if (body.motion->beachedTime >= 4.0)
                        { life.death = LifeDeath::stranded; life.deathX = px; life.deathY = py; }
                }
            } else {
                if (body.motion) body.motion->beachedTime = -1;
                // Growth
                if (life.size < 0.75f) life.size = std::min(0.75f, life.size + float(dt * 0.0025));
                if (life.size >= float(kMaxSize) - 0.001f)
                    life.senescence = std::max(0.0, life.senescence - dt);
                if (life.age >= life.lifespan || life.senescence <= 0)
                    life.death = LifeDeath::oldAge;

                // Build behavior targets
                double targetX = -1e9, targetY = -1e9;
                double urgency = 0, depthTarget = 0.32, speedScale = 0.8;
                life.behavior = LifeBehavior::wandering;
                life.shelterTime = std::max(0.0, life.shelterTime - dt);

                // Threat (larger fish within 0.26)
                int threatIdx = -1;
                for (int fi : fishIdx) {
                    if (fi == idx) continue;
                    const auto& predLife = *snapshot[fi].ecology->life;
                    if (predLife.size > life.size * 1.35f
                        && refugeDist(snapshot[fi].x, snapshot[fi].y, px, py) < 0.26) {
                        if (threatIdx < 0
                            || refugeDist(snapshot[fi].x, snapshot[fi].y, px, py)
                             < refugeDist(snapshot[threatIdx].x, snapshot[threatIdx].y, px, py))
                            threatIdx = fi;
                    }
                }

                // Prey (smaller fish or non-fish within range, not sheltered)
                int preyIdx = -1;
                for (int pi = 0; pi < (int)objects.size(); ++pi) {
                    if (pi == idx || objects[pi].departure) continue;
                    const auto& po = snapshot[pi];
                    if (!po.ecology || !po.ecology->life) continue;
                    if (po.ecology->life->death != LifeDeath::none) continue;
                    if (held.count(po.id)) continue;
                    if (po.ecology->kind == EcologyKind::fish) {
                        if (!po.ecology->life->canBeEaten()) continue;
                        if (po.ecology->life->size >= life.size / 1.35f) continue;
                        if (protects(*po.ecology, po.x, po.y)) continue;
                        if (refugeDist(po.x, po.y, px, py) >= 0.33) continue;
                    } else {
                        // plant/firefly – fish eats them too
                        if (!habitatAccepts(EcologyKind::fish, po.x, po.y)) continue;
                        if ((po.ecology->motion ? po.ecology->motion->growth : 1.0) < 1.0) continue;
                        if (refugeDist(po.x, po.y, px, py) >= 0.28) continue;
                    }
                    if (preyIdx < 0
                        || refugeDist(snapshot[pi].x, snapshot[pi].y, px, py)
                         < refugeDist(snapshot[preyIdx].x, snapshot[preyIdx].y, px, py))
                        preyIdx = pi;
                }

                if (threatIdx >= 0) {
                    urgency = 1; speedScale = 1; life.behavior = LifeBehavior::fleeing;
                    // Find nearest shelter
                    const Refuge* best = nullptr; double bestD = 1e9;
                    for (const auto& r : kRefuges) {
                        if (life.size > kRefugeCapacity) continue;
                        double d = refugeDist(r.u, r.v, px, py);
                        if (d < bestD) { bestD = d; best = &r; }
                    }
                    if (best && refugeDist(best->u, best->v, px, py) < 0.24) {
                        life.shelterID = best->id;
                        targetX = best->u; targetY = best->v;
                        depthTarget = 0.92; life.shelterTime = 4;
                        if (refugeDist(px, py, best->u, best->v) < 0.007
                            && life.depthFraction > 0.80)
                            { life.behavior = LifeBehavior::hiding; speedScale = 0; }
                    } else {
                        const auto& e = snapshot[threatIdx];
                        double dx = px - e.x, dy = py - e.y;
                        double d = std::max(0.001, std::hypot(dx, dy));
                        targetX = px + dx/d * 0.15; targetY = py + dy/d * 0.15;
                        depthTarget = 0.70;
                    }
                } else if (life.shelterTime > 0) {
                    const Refuge* r = findRefuge(life.shelterID);
                    if (r) {
                        targetX = r->u; targetY = r->v;
                        depthTarget = 0.92; life.behavior = LifeBehavior::hiding;
                        speedScale = 0.5;
                    }
                } else if (preyIdx >= 0) {
                    life.shelterID = -1; life.behavior = LifeBehavior::hunting;
                    urgency = 0.8; speedScale = 0.94;
                    const auto& food = snapshot[preyIdx];
                    double vx = food.ecology->swimming ? food.ecology->swimming->velocityX : 0;
                    double vy = food.ecology->swimming ? food.ecology->swimming->velocityY : 0;
                    targetX = food.x + vx * 0.35; targetY = food.y + vy * 0.35;
                    depthTarget = food.ecology->life->depthFraction;
                } else if (life.age >= kMaturity && life.breedCooldown <= 0) {
                    // Seek same-species mate
                    int mateIdx = -1;
                    for (int fi : fishIdx) {
                        if (fi == idx) continue;
                        const auto& ml = *snapshot[fi].ecology->life;
                        if (ml.species != life.species) continue;
                        if (ml.age < kMaturity || ml.breedCooldown > 0) continue;
                        if (mateIdx < 0
                            || refugeDist(snapshot[fi].x, snapshot[fi].y, px, py)
                             < refugeDist(snapshot[mateIdx].x, snapshot[mateIdx].y, px, py))
                            mateIdx = fi;
                    }
                    if (mateIdx >= 0) {
                        targetX = snapshot[mateIdx].x; targetY = snapshot[mateIdx].y;
                        life.behavior = LifeBehavior::courting;
                        depthTarget = snapshot[mateIdx].ecology->life->depthFraction;
                    } else { life.shelterID = -1; }
                } else { life.shelterID = -1; }

                // Escape obstacle has priority (brief override)
                if (body.motion && body.motion->escapeTime > 0) { targetX = -1e9; targetY = -1e9; }

                // Clamp target to water bounds
                if (targetX > -1e8) { boundedPosition(targetX, targetY); }

                // Swim step
                if (body.swimming) {
                    if (env.storm > 0.3)
                        speedScale *= 1.0 + env.storm * 2.5;
                    if (targetX > -1e8 && speedScale > 0)
                        body.swimming->stepToward(px, py, dt, env.sunlight(),
                                                  targetX, targetY, urgency, speedScale);
                    else
                        body.swimming->step(px, py, dt, env.sunlight());
                }
                life.depthFraction += (depthTarget - life.depthFraction) * (1 - std::exp(-dt / 0.65));

                // Large fish avoid shelter openings
                for (const auto& r : kRefuges) {
                    if (life.size <= kRefugeCapacity || life.depthFraction <= 0.7) continue;
                    double dx = px - r.u, dy = (py - r.v) * 0.8, d = std::hypot(dx, dy);
                    if (d < 0.095 && d > 1e-5) {
                        double push = (0.095 - d) * (1 - std::exp(-dt / 0.12));
                        px += dx / d * push; py += dy / d * push / 0.8;
                    }
                }
            }
        }

        // ── Firefly ──────────────────────────────────────────────────────────
        else if (body.kind == EcologyKind::firefly) {
            life.behavior = LifeBehavior::wandering;
            if (env.storm > 0.3)
                life.flightWait = std::max(0.0, life.flightWait - env.storm * dt * 3.0);
            if (life.flightWait <= 0 && life.flightX < 0.11) life.chooseFlightTarget();
            double dx = life.flightX - px, dy = life.flightY - py;
            double d = std::hypot(dx, dy);
            life.flightWait = std::max(0.0, life.flightWait - dt);
            if (d < 0.03 && life.flightWait <= 0) life.chooseFlightTarget();
            const double speed = std::min(0.046 * (1.0 + env.storm), d * 0.5 * (1.0 + env.storm));
            const double tx = d > 0.001 ? dx/d*speed : 0;
            const double ty = d > 0.001 ? dy/d*speed : 0;
            const double decay = std::exp(-dt / 0.45);
            px += tx*dt + (life.flightVX - tx) * 0.45 * (1 - decay);
            py += ty*dt + (life.flightVY - ty) * 0.45 * (1 - decay);
            life.flightVX = tx + (life.flightVX - tx) * decay;
            life.flightVY = ty + (life.flightVY - ty) * decay;
            // Dawn accumulation → death
            if (env.daylight() > 0.12)
                life.dayFade = std::min(6.0, life.dayFade + dt);
            else
                life.dayFade = std::max(0.0, life.dayFade - dt);
            if (life.dayFade >= 6.0) {
                life.death = LifeDeath::dawn;
                life.deathX = px; life.deathY = py;
            }
        }

        // ── All organisms: wind drag ─────────────────────────────────────────
        {
            const double wind  = std::pow(env.wind, 3.0);
            const double gust  = 0.85 + 0.15 * std::sin(env.elapsed * 1.7 + double(body.appearanceSeed % 17));
            const double force = wind * gust;
            const double accel = force * 1.05 - life.bend * 7 - life.bendVelocity * 4;
            life.bendVelocity = std::max(-8.0, std::min(8.0, life.bendVelocity + accel * dt));
            life.bend         = std::max(-1.5, std::min(1.5, life.bend + life.bendVelocity * dt));

            // Plant root stress
            if (body.kind == EcologyKind::plant) {
                const double stressDelta = env.wind > 0.76 ? (env.wind - 0.76) * 2.6 : -0.3;
                life.rootStress = std::max(0.0, std::min(1.1,
                    life.rootStress + dt * (stressDelta + env.storm * 0.45)));
                if (life.rootStress >= 1.0) life.uprooted = true;
            }

            // Wind displacement (skip held / fish in deep water)
            const bool exposed = body.kind != EcologyKind::plant || life.uprooted;
            const double mass   = body.kind == EcologyKind::fish ? 0.65 + life.size * life.size
                                : body.kind == EcologyKind::plant ? 0.80 : 0.55;
            const double shelter = body.kind == EcologyKind::fish ? 1 - life.depthFraction * 0.68 : 1.0;
            const double tgtX = exposed ? force * 0.032 / mass * shelter : 0;
            const double tgtY = exposed ? std::max(-0.28, -force * 0.19 / mass * shelter) : 0;
            const double decay = std::exp(-dt / 0.55);
            px += tgtX * dt + (life.windX - tgtX) * 0.55 * (1 - decay);
            py += tgtY * dt + (life.windY - tgtY) * 0.55 * (1 - decay);
            life.windX = tgtX + (life.windX - tgtX) * decay;
            life.windY = tgtY + (life.windY - tgtY) * decay;
            if (env.wind > 0.8 && exposed) life.behavior = LifeBehavior::windblown;

            // Out-of-habitat → blow away or clamp
            if (!habitatOccupies(body.kind, px, py)
                || (body.kind == EcologyKind::fish && !habitatAccepts(EcologyKind::fish, px, py))) {
                if (env.wind > 0.72 || life.uprooted
                    || std::hypot(life.windX, life.windY) > 0.035) {
                    const auto hab = habitatAt(px, py);
                    life.death = (body.kind == EcologyKind::fish
                                  && (hab == WorldHabitat::shore || hab == WorldHabitat::woodland))
                                 ? LifeDeath::stranded : LifeDeath::blownAway;
                    life.deathX = std::max(-1.0, std::min(2.0, px));
                    life.deathY = std::max(-1.0, std::min(2.0, py));
                } else {
                    boundedPosition(px, py);
                }
            }
        }

        // Plant old age
        if (body.kind != EcologyKind::fish && life.age >= life.lifespan)
            { life.death = LifeDeath::oldAge; life.deathX = px; life.deathY = py; }

        if (life.death != LifeDeath::none) {
            life.behavior = LifeBehavior::windblown; // visual departure cue
            px = o.x; py = o.y; // keep at last valid position
            if (life.death == LifeDeath::oldAge) { life.deathX = o.x; life.deathY = o.y; }
        }
        o.x = px; o.y = py;
    }

    // ── Predation bites (largest fish first) ────────────────────────────────
    for (int predIdx : [&]{
        auto sorted = fishIdx;
        std::sort(sorted.begin(), sorted.end(), [&](int a, int b) {
            return (objects[a].ecology->life->size) > (objects[b].ecology->life->size);
        });
        return sorted;
    }()) {
        if (!objects[predIdx].ecology->life) continue;
        if (objects[predIdx].ecology->life->death != LifeDeath::none) continue;
        auto& bigLife = *objects[predIdx].ecology->life;
        for (int pi = 0; pi < (int)objects.size(); ++pi) {
            if (pi == predIdx || objects[pi].departure) continue;
            auto& po = objects[pi];
            if (!po.ecology || !po.ecology->life) continue;
            if (po.ecology->life->death != LifeDeath::none) continue;
            if (held.count(po.id)) continue;
            if (!po.ecology->life->canBeEaten()) continue;
            if (po.ecology->kind == EcologyKind::fish
                && bigLife.size <= po.ecology->life->size * 1.35f) continue;
            if (protects(*po.ecology, po.x, po.y)) continue;
            if (po.ecology->kind == EcologyKind::fish
                && std::abs(bigLife.depthFraction - po.ecology->life->depthFraction) >= 0.24) continue;
            const double bite = 0.018 + bigLife.size * 0.012;
            if (refugeDist(objects[predIdx].x, objects[predIdx].y, po.x, po.y) >= bite) continue;
            // Eat
            po.ecology->life->death = LifeDeath::eaten;
            po.ecology->life->deathX = po.x; po.ecology->life->deathY = po.y;
            const double nutrition = po.ecology->kind == EcologyKind::fish ? 0.75 : 0.12;
            const double s = bigLife.size, p = po.ecology->life->size;
            bigLife.size = float(std::min(kMaxSize,
                std::pow(std::pow(s, 3) + std::pow(p, 3) * nutrition, 1.0 / 3.0)));
            break;
        }
    }

    // ── Reproduction ────────────────────────────────────────────────────────
    std::vector<EcologyBirth> births;
    std::set<int32_t> paired;
    if ((int)objects.size() >= 64) return births;

    for (int ai : fishIdx) {
        auto& aObj = objects[ai];
        if (!aObj.ecology->life) continue;
        auto& aLife = *aObj.ecology->life;
        if (aLife.death != LifeDeath::none) continue;
        if (aLife.age < kMaturity || aLife.breedCooldown > 0) continue;
        if (aLife.behavior == LifeBehavior::fleeing
            || aLife.behavior == LifeBehavior::hiding) continue;
        if (paired.count(aObj.id)) continue;

        for (int bi : fishIdx) {
            if (bi <= ai) continue;
            auto& bObj = objects[bi];
            auto& bLife = *bObj.ecology->life;
            if (bLife.death != LifeDeath::none) continue;
            if (bLife.species != aLife.species) continue;
            if (bLife.age < kMaturity || bLife.breedCooldown > 0) continue;
            if (bLife.behavior == LifeBehavior::fleeing
                || bLife.behavior == LifeBehavior::hiding) continue;
            if (paired.count(bObj.id)) continue;
            if (std::abs(aLife.depthFraction - bLife.depthFraction) >= 0.2) continue;
            if (refugeDist(aObj.x, aObj.y, bObj.x, bObj.y) >= 0.065) continue;

            // Breed
            const double mx = (aObj.x + bObj.x) * 0.5;
            const double my = (aObj.y + bObj.y) * 0.5;
            const uint32_t childSeed = (aLife.lifeRng ^ bLife.lifeRng * 2654435761u) | 1u;
            births.push_back({ aObj.id, bObj.id, mx, my, childSeed,
                                uint32_t(aLife.species),
                                std::min(1000, std::max(aLife.generation, bLife.generation) + 1) });
            // Cooldown is committed by the caller only after placement + audio acceptance.
            paired.insert(aObj.id); paired.insert(bObj.id);
            break;
        }
        if (births.size() >= 2 || births.size() + objects.size() >= 64) break;
    }
    return births;
}

} // namespace LifeDynamics
