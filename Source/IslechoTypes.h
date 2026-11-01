#pragma once
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <vector>
const float M_PI = 3.14159265358979323846;
// ─── Core enumerations ───────────────────────────────────────────────────────

enum class EcologyKind : int { fish = 0, plant = 1, firefly = 2 };
enum class EcologyWeather : int { clear = 0, overcast = 1, windy = 2, rain = 3 };
enum class WorldHabitat : int { woodland = 0, shore = 1, water = 2 };
enum class SoundMaterial : int { sine = 0, noise = 1, train = 2, grains = 3, drone = 4 };
enum class LifeBehavior : int { wandering, hunting, fleeing, hiding, courting, windblown };
enum class LifeDeath : int { none = -1, stranded, eaten, oldAge, blownAway, dawn, drowned };

// ─── Xorshift RNG ─────────────────────────────────────────────────────────────

struct XorShift32 {
    uint32_t state;
    explicit XorShift32(uint32_t seed) : state(seed == 0 ? 1 : seed) {}
    uint32_t next() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    }
    double unit() { return static_cast<double>(next()) / static_cast<double>(0xFFFFFFFFu); }
};

static inline uint32_t xorshift(uint32_t v) {
    v ^= v << 13; v ^= v >> 17; v ^= v << 5; return v;
}
static inline double xorunit(uint32_t v) { return static_cast<double>(v) / static_cast<double>(0xFFFFFFFFu); }

// ─── Layout constants ────────────────────────────────────────────────────────

struct WorldBounds {
    static constexpr double minX = -0.32, maxX = 1.32;
    static constexpr double minY = -0.32, maxY = 1.32;
    static constexpr double placementInset = 0.015;
};

// ─── Habitat helpers ─────────────────────────────────────────────────────────

inline WorldHabitat habitatAt(double x, double y) {
    if (x <= 0 || x >= 1 || y <= 0 || y >= 0.60) return WorldHabitat::water;
    if (x > 0.10 && x < 0.90 && y > 0.10 && y < 0.34) return WorldHabitat::woodland;
    if (y < 0.60) return WorldHabitat::shore;
    return WorldHabitat::water;
}

inline bool habitatInBounds(double x, double y) {
    return x >= WorldBounds::minX && x <= WorldBounds::maxX &&
           y >= WorldBounds::minY && y <= WorldBounds::maxY;
}

inline bool habitatAccepts(EcologyKind kind, double x, double y) {
    if (!habitatInBounds(x, y)) return false;
    WorldHabitat h = habitatAt(x, y);
    switch (kind) {
    case EcologyKind::fish:    return h == WorldHabitat::water;
    case EcologyKind::plant:   return h == WorldHabitat::shore || h == WorldHabitat::woodland;
    case EcologyKind::firefly: return h == WorldHabitat::woodland || h == WorldHabitat::shore;
    }
    return false;
}

inline bool habitatOccupies(EcologyKind, double x, double y) {
    return habitatInBounds(x, y);
}

inline void boundedPosition(double& x, double& y) {
    const double lo = WorldBounds::minX + WorldBounds::placementInset;
    const double hi = WorldBounds::maxX - WorldBounds::placementInset;
    x = std::max(lo, std::min(hi, x));
    y = std::max(lo, std::min(hi, y));
}

// ─── Material / sound role helpers ───────────────────────────────────────────

inline SoundMaterial materialForKind(EcologyKind kind) {
    switch (kind) {
    case EcologyKind::fish:    return SoundMaterial::sine;
    case EcologyKind::plant:   return SoundMaterial::grains;
    case EcologyKind::firefly: return SoundMaterial::train;
    }
    return SoundMaterial::sine;
}

// ─── Musical helpers ─────────────────────────────────────────────────────────

struct MusicalScale {
    enum Type { pentatonic, minor, minorPentatonic, major };
    static std::vector<int> intervals(Type t) {
        switch (t) {
        case pentatonic:      return {0,2,4,7,9};
        case minor:           return {0,2,3,5,7,8,10};
        case minorPentatonic: return {0,3,5,7,10};
        case major:           return {0,2,4,5,7,9,11};
        }
        return {0,2,4,7,9};
    }
    static std::vector<int> notes(Type t, int root) {
        auto iv = intervals(t);
        std::vector<int> result;
        for (int octave = 0; octave < 3; ++octave)
            for (int i : iv) result.push_back(48 + root + octave * 12 + i);
        result.push_back(84 + root);
        return result;
    }
    static float midiToHz(int midi) {
        return static_cast<float>(440.0 * std::pow(2.0, (midi - 69) / 12.0));
    }
};

// ─── FishSwimming ────────────────────────────────────────────────────────────

struct FishSwimming {
    double targetX = 0.5, targetY = 0.85;
    double velocityX = 0, velocityY = 0;
    double restRemaining = 0;
    double heading = 0;
    uint32_t seed;
    static constexpr double maximumSpeed = 0.055;

    explicit FishSwimming(uint32_t s) : seed((s ^ 0xA3C59AC3) | 1) { chooseTarget(); }

    void stopForTouch() { velocityX = 0; velocityY = 0; }

    double randomUnit() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<double>(seed) / static_cast<double>(0xFFFFFFFFu);
    }

    void chooseTarget() {
        // World water area: x in [-0.28, 1.28], y in [0.60, 1.28] plus borders
        const double minX = -0.28, maxX = 1.28, minY = -0.28, maxY = 1.28;
        for (int i = 0; i < 32; ++i) {
            double tx = minX + randomUnit() * (maxX - minX);
            double ty = minY + randomUnit() * (maxY - minY);
            if (habitatAccepts(EcologyKind::fish, tx, ty)) { targetX = tx; targetY = ty; return; }
        }
        targetX = 0.5; targetY = 0.85;
    }

    // Wander using internal autonomous target.
    void step(double& px, double& py, double dt, double sunlight) {
        stepImpl(px, py, dt, sunlight, nullptr, nullptr, 0.0, 1.0);
    }

    // Directed step: overrides the wander target when extTX/TY are non-null.
    void stepToward(double& px, double& py, double dt, double sunlight,
                    double extTX, double extTY, double urgency, double speedScale) {
        stepImpl(px, py, dt, sunlight, &extTX, &extTY, urgency, speedScale);
    }

private:
    void stepImpl(double& px, double& py, double dt, double sunlight,
                  const double* extTX, const double* extTY,
                  double urgency, double speedScale) {
        if (dt <= 0 || dt > 0.2) return;
        if (!habitatAccepts(EcologyKind::fish, px, py)) return;
        double remaining = dt;
        while (remaining > 0.0) {
            const double h = std::min(remaining, 1.0 / 120.0);
            remaining -= h;
            const double tx = extTX ? *extTX : targetX;
            const double ty = extTY ? *extTY : targetY;
            double dx = tx - px, dy = ty - py;
            double dist = std::sqrt(dx*dx + dy*dy);
            if (!extTX && restRemaining <= 0 && dist < 0.022) {
                restRemaining = 0.5 + randomUnit() * 1.1;
                chooseTarget();
            }
            if (!extTX && restRemaining > 0) { restRemaining -= h; velocityX *= 0.85; velocityY *= 0.85; }
            else {
                const double baseSpeed = maximumSpeed * speedScale * (0.7 + 0.3 * sunlight);
                const double seekK = 1.8 + urgency * 1.2, dampK = 3.5;
                if (dist > 0) { velocityX += h * seekK * dx / dist * baseSpeed; velocityY += h * seekK * dy / dist * baseSpeed; }
                velocityX -= h * dampK * velocityX; velocityY -= h * dampK * velocityY;
                const double v = std::sqrt(velocityX*velocityX + velocityY*velocityY);
                if (v > baseSpeed) { velocityX *= baseSpeed/v; velocityY *= baseSpeed/v; }
            }
            double nx = px + velocityX * h, ny = py + velocityY * h;
            if (habitatAccepts(EcologyKind::fish, nx, ny)) { px = nx; py = ny; }
            else { velocityX = -velocityX * 0.5; velocityY = -velocityY * 0.5; }
        }
    }
public:
};

// ─── EcologyLife ─────────────────────────────────────────────────────────────

struct EcologyLife {
    LifeBehavior behavior = LifeBehavior::wandering;
    LifeDeath    death    = LifeDeath::none;
    double deathX = 0, deathY = 0;
    double windX = 0, windY = 0;

    int    shelterID   = -1;
    double shelterTime = 0;

    float  size        = 1.0f;
    double age         = 0;
    double lifespan    = 300.0;
    double senescence  = 90.0;   // fish-only: decrements at max size
    int    generation  = 0;
    int    species     = 0;
    uint32_t lifeRng   = 1;      // per-entity RNG for movement decisions

    double breedCooldown = 30.0;
    double depthFraction = 0.35; // fish: 0=surface, 1=deep

    // Firefly flight
    double flightX = 0.5, flightY = 0.3;
    double flightVX = 0, flightVY = 0;
    double flightWait = 0;
    double dayFade = 0;           // accumulates in daylight → dawn death

    // Wind/bending
    double bend = 0, bendVelocity = 0;
    double rootStress = 0;        // plant: 0→1 → uprooted
    bool   uprooted   = false;

    // Plant growth (used by ecologyTick for visual growth progress)
    double growth = 1.0;
    bool   juvenile = false;

    // Legacy field kept for persistence compatibility
    uint32_t speciesSeed = 0;

    EcologyLife() = default;

    static EcologyLife create(EcologyKind kind, uint32_t seed,
                              bool inJuvenile = false,
                              int inSpecies = -1, int inGeneration = 0) {
        EcologyLife life;
        uint32_t state = seed == 0 ? 1u : seed;
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        const int variant = static_cast<int>(
            double(state & 0xFFFFFFu) / double(0xFFFFFFu) * 12.0) % 12;
        life.species      = inSpecies >= 0 ? inSpecies : variant % 3;
        life.lifeRng      = state | 1u;
        life.generation   = inGeneration;
        life.juvenile     = inJuvenile;
        life.speciesSeed  = state;
        if (kind == EcologyKind::fish)
            life.size = inJuvenile ? 0.32f
                                   : 0.55f + float(state % 10001u) / 10000.f * 0.65f;
        else
            life.size = 1.0f;
        life.lifespan      = 240.0 + double((state >> 8) % 181u);
        life.breedCooldown = inJuvenile ? 65.0 : 30.0 + double(state % 31u);
        life.depthFraction = 0.28 + double((state >> 5) % 101u) / 100.0 * 0.22;
        // Initial flight target for fireflies chosen on first step
        life.flightWait = 0;
        return life;
    }

    bool canBeEaten() const { return generation == 0 || age >= 12.0; }

    double unit() {
        lifeRng ^= lifeRng << 13; lifeRng ^= lifeRng >> 17; lifeRng ^= lifeRng << 5;
        return double(lifeRng) / double(0xFFFFFFFFu);
    }

    void chooseFlightTarget() {
        const double rx = 0.10 + unit() * 0.78;
        const double ry = 0.10 + unit() * 0.48;
        flightX = std::max(0.10, std::min(0.90, rx));
        flightY = std::max(0.10, std::min(0.58, ry));
        flightWait = 1.0 + unit() * 2.0;
    }
};

// ─── EcologyMotion ────────────────────────────────────────────────────────────

struct EcologyMotion {
    double height = 0;
    double vx = 0, vy = 0, vz = 0;
    double qx = 0, qy = 0, qz = 0, qw = 1;
    double wx = 0, wy = 0, wz = 0;
    double growth = 1;
    bool natural = false;
    double escapeTime = 0;
    double jumpRemaining = 0, jumpCooldown = 0;
    bool attackPending = false;
    double dayFade = 0, recovering = 0;
    double beachedTime = -1;   // <0 = nil
    double waveCarried = -1;   // <0 = nil
    double floodDose = -1;     // <0 = nil
};

// ─── EcologyBody ──────────────────────────────────────────────────────────────

struct EcologyBody {
    EcologyKind kind;
    double cycleProgress = 0;
    uint32_t seed = 1;
    uint32_t appearanceSeed = 1;
    std::optional<FishSwimming> swimming;
    std::optional<EcologyLife> life;
    std::optional<EcologyMotion> motion;

    EcologyBody() : kind(EcologyKind::fish) {}
    explicit EcologyBody(EcologyKind k, uint32_t s, uint32_t as = 0)
        : kind(k), seed(s == 0 ? 1 : s), appearanceSeed(as == 0 ? (s == 0 ? 1 : s) : as) {
        uint32_t ns = xorshift(seed);
        cycleProgress = 0.05 + 0.60 * xorunit(ns);
        if (k == EcologyKind::fish) swimming = FishSwimming(seed);
        life = EcologyLife::create(k, seed);
    }
};

// ─── WeatherCycle ─────────────────────────────────────────────────────────────

struct WeatherCycle {
    uint32_t seed = 0x73ac9201;
    double remaining = 35;
    EcologyWeather last = EcologyWeather::clear;
    double rain = 0, cloud = 0, wind = 0;

    void step(double dt, EcologyWeather& weather, bool automatic) {
        if (last != weather) { last = weather; remaining = 45; }
        if (automatic) {
            remaining -= dt;
            if (remaining <= 0) {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                // choose different weather
                int cur = static_cast<int>(weather);
                int next = static_cast<int>(seed % 3);
                if (next >= cur) ++next;
                weather = static_cast<EcologyWeather>(next % 4);
                last = weather;
                remaining = 35.0 + (seed % 4001) / 100.0;
            }
        }
        const double sm = 1.0 - std::exp(-dt / 5.0);
        rain  += ((weather == EcologyWeather::rain    ? 1.0 : 0.0) - rain)  * sm;
        cloud += ((weather == EcologyWeather::overcast ? 1.0 : 0.0) - cloud) * sm;
        wind  += ((weather == EcologyWeather::windy   ? 1.0 : 0.0) - wind)  * sm;
    }
};

// ─── EnvironmentState ─────────────────────────────────────────────────────────

struct EnvironmentState {
    double dayPhase = 0.72;
    double dayDuration = 180;
    double wind = 0.3;
    double storm = 0.0; // Transient shake/button impulse; never saved.
    EcologyWeather weather = EcologyWeather::clear;
    std::optional<WeatherCycle> weatherCycle;
    bool automaticWeather = true;
    bool isRunning = true;
    double elapsed = 0;

    double rainAmount() const {
        if (weatherCycle) return weatherCycle->rain;
        return weather == EcologyWeather::rain ? 1.0 : 0.0;
    }
    double cloudAmount() const {
        if (weatherCycle) return weatherCycle->cloud;
        return weather == EcologyWeather::overcast ? 1.0 : 0.0;
    }
    double windyAmount() const {
        if (weatherCycle) return weatherCycle->wind;
        return weather == EcologyWeather::windy ? 1.0 : 0.0;
    }
    double effectiveWind() const {
        return std::min(1.0, wind + windyAmount() * 0.5 + rainAmount() * 0.20);
    }
    double daylight() const {
        return std::max(0.0, std::sin((dayPhase - 0.25) * 2.0 * M_PI));
    }
    double sunlight() const {
        return daylight() * (1.0 - 0.65 * rainAmount() - 0.35 * cloudAmount());
    }
};

// ─── SoundObject ──────────────────────────────────────────────────────────────

struct Departure {
    double originX, originY;
    double velX, velY;
    double started, duration;
    void positionAt(double time, double& x, double& y) const {
        double t = std::max(0.0, time - started);
        const double damping = 2.8;
        double distance = (1.0 - std::exp(-damping * t)) / damping;
        x = originX + velX * distance;
        y = originY + velY * distance;
    }
    double opacityAt(double time) const {
        return std::max(0.0, 1.0 - (time - started) / duration);
    }
};

struct SoundObject {
    int32_t id;
    SoundMaterial material;
    double x = 0.5, y = 0.5;
    double bornBeat = -1, touchTime = -1;
    float energy = 0;
    float soundingFrequency = 220.f;
    double lastOnset = -1, lastOnsetSeconds = -1;
    double holdProgress = 0;
    std::optional<EcologyBody> ecology;
    std::optional<Departure> departure;

    SoundObject() : id(0), material(SoundMaterial::sine) {}
    SoundObject(int32_t i, SoundMaterial m, double px, double py)
        : id(i), material(m), x(px), y(py) {}
};

// ─── Composition model state ──────────────────────────────────────────────────

enum class MusicalScaleType { pentatonic, minor, minorPentatonic, major };

struct CompositionSettings {
    double bpm = 120.0;
    double grid = 0.25;
    MusicalScaleType scale = MusicalScaleType::pentatonic;
    int root = 0;
    float macro = 1.0f;
    float density = 32.0f;
    float spread = 0.18f;
    float character = 0.5f;
};
