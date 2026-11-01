#include "Persistence.h"
#include <juce_core/juce_core.h>
#include <cmath>

// ─── Helpers ─────────────────────────────────────────────────────────────────

static double dv(const juce::var& v, double def = 0.0) {
    return v.isVoid() ? def : static_cast<double>(v);
}
static float fv(const juce::var& v, float def = 0.f) {
    return v.isVoid() ? def : static_cast<float>(static_cast<double>(v));
}
static int iv(const juce::var& v, int def = 0) {
    return v.isVoid() ? def : static_cast<int>(v);
}
static bool bv(const juce::var& v, bool def = false) {
    return v.isVoid() ? def : static_cast<bool>(v);
}
static uint32_t u32v(const juce::var& v, uint32_t def = 1) {
    if (v.isVoid()) return def;
    auto i = static_cast<int64_t>(v);
    return i <= 0 ? def : static_cast<uint32_t>(i);
}

// ─── Key parsers ─────────────────────────────────────────────────────────────

MusicalScaleType Persistence::parseScaleKey(const juce::String& key) {
    if (key == "minor")          return MusicalScaleType::minor;
    if (key == "minorPentatonic") return MusicalScaleType::minorPentatonic;
    if (key == "major")          return MusicalScaleType::major;
    return MusicalScaleType::pentatonic;
}
juce::String Persistence::encodeScaleKey(MusicalScaleType t) {
    switch (t) {
    case MusicalScaleType::minor:          return "minor";
    case MusicalScaleType::minorPentatonic: return "minorPentatonic";
    case MusicalScaleType::major:          return "major";
    default:                               return "pentatonic";
    }
}
EcologyKind Persistence::parseKind(const juce::String& s) {
    if (s == "plant")   return EcologyKind::plant;
    if (s == "firefly") return EcologyKind::firefly;
    return EcologyKind::fish;
}
juce::String Persistence::encodeKind(EcologyKind k) {
    switch (k) {
    case EcologyKind::plant:   return "plant";
    case EcologyKind::firefly: return "firefly";
    default:                   return "fish";
    }
}
EcologyWeather Persistence::parseWeather(const juce::String& s) {
    if (s == "overcast") return EcologyWeather::overcast;
    if (s == "windy")    return EcologyWeather::windy;
    if (s == "rain")     return EcologyWeather::rain;
    return EcologyWeather::clear;
}
juce::String Persistence::encodeWeather(EcologyWeather w) {
    switch (w) {
    case EcologyWeather::overcast: return "overcast";
    case EcologyWeather::windy:    return "windy";
    case EcologyWeather::rain:     return "rain";
    default:                       return "clear";
    }
}
LifeBehavior Persistence::parseBehavior(const juce::String& s) {
    if (s == "hunting")   return LifeBehavior::hunting;
    if (s == "fleeing")   return LifeBehavior::fleeing;
    if (s == "hiding")    return LifeBehavior::hiding;
    if (s == "courting")  return LifeBehavior::courting;
    if (s == "windblown") return LifeBehavior::windblown;
    return LifeBehavior::wandering;
}
juce::String Persistence::encodeBehavior(LifeBehavior b) {
    switch (b) {
    case LifeBehavior::hunting:   return "hunting";
    case LifeBehavior::fleeing:   return "fleeing";
    case LifeBehavior::hiding:    return "hiding";
    case LifeBehavior::courting:  return "courting";
    case LifeBehavior::windblown: return "windblown";
    default:                      return "wandering";
    }
}
LifeDeath Persistence::parseDeath(const juce::String& s) {
    if (s == "stranded")  return LifeDeath::stranded;
    if (s == "eaten")     return LifeDeath::eaten;
    if (s == "oldAge")    return LifeDeath::oldAge;
    if (s == "blownAway") return LifeDeath::blownAway;
    if (s == "dawn")      return LifeDeath::dawn;
    if (s == "drowned")   return LifeDeath::drowned;
    return LifeDeath::none;
}

// ─── parseSettings ──────────────────────────────────────────────────────────

bool Persistence::parseSettings(const juce::var& obj, int version, CompositionSettings& out) {
    if (!obj.isObject()) return false;
    out.bpm  = dv(obj["bpm"], 120.0);
    out.grid = dv(obj["grid"], 0.25);
    out.scale = parseScaleKey(obj["scale"].toString());
    out.root = iv(obj["root"], 0);
    // Ecology worlds always use these defaults (schema >= 3)
    if (version >= 3) {
        out.macro = 1.0f; out.density = 32.0f; out.spread = 0.18f; out.character = 0.5f;
    } else {
        out.macro     = fv(obj["macro"], 1.0f);
        out.density   = fv(obj["density"], 32.0f);
        out.spread    = fv(obj["spread"], 0.18f);
        out.character = fv(obj["character"], 0.5f);
    }
    return true;
}

// ─── parseEnvironment ────────────────────────────────────────────────────────

bool Persistence::parseEnvironment(const juce::var& obj, EnvironmentState& out) {
    if (!obj.isObject()) return false;
    out.dayPhase   = dv(obj["dayPhase"],   0.72);
    out.dayDuration= dv(obj["dayDuration"],180.0);
    out.wind       = dv(obj["wind"],       0.3);
    out.weather    = parseWeather(obj["weather"].toString());
    out.isRunning  = bv(obj["isRunning"],  true);
    out.elapsed    = dv(obj["elapsed"],    0.0);
    out.automaticWeather = bv(obj["automaticWeather"], true);

    const auto& wc = obj["weatherCycle"];
    if (!wc.isVoid() && wc.isObject()) {
        WeatherCycle cycle;
        cycle.seed      = u32v(wc["seed"], 0x73ac9201);
        cycle.remaining = dv(wc["remaining"], 35.0);
        cycle.last      = parseWeather(wc["last"].toString());
        cycle.rain      = dv(wc["rain"], 0.0);
        cycle.cloud     = dv(wc["cloud"], 0.0);
        cycle.wind      = dv(wc["wind"], 0.0);
        out.weatherCycle = cycle;
    }
    return true;
}

// ─── parseLife ────────────────────────────────────────────────────────────────

bool Persistence::parseLife(const juce::var& obj, EcologyKind /*kind*/, EcologyLife& out) {
    if (!obj.isObject()) return false;
    out.age            = dv(obj["age"], 0.0);
    out.size           = fv(obj["size"], 1.0f);
    out.generation     = iv(obj["generation"], 0);
    out.juvenile       = bv(obj["juvenile"], false);
    out.breedCooldown  = dv(obj["breedCooldown"], 0.0);
    out.depthFraction  = dv(obj["depthFraction"], 0.4);
    out.flightX        = dv(obj["flightX"], 0.5);
    out.flightY        = dv(obj["flightY"], 0.3);
    out.uprooted       = bv(obj["uprooted"], false);
    out.growth         = dv(obj["growth"], 1.0);
    out.shelterID      = iv(obj["shelterID"], -1);
    out.shelterTime    = dv(obj["shelterTime"], 0.0);
    out.speciesSeed    = u32v(obj["speciesSeed"], 0);
    out.species        = iv(obj["species"], 0);
    out.lifeRng        = u32v(obj["lifeRng"], out.lifeRng);
    out.lifespan       = dv(obj["lifespan"], out.lifespan);
    out.senescence     = dv(obj["senescence"], out.senescence);
    out.flightVX       = dv(obj["flightVX"], 0.0);
    out.flightVY       = dv(obj["flightVY"], 0.0);
    out.flightWait     = dv(obj["flightWait"], 0.0);
    out.dayFade        = dv(obj["dayFade"], 0.0);
    out.bend           = dv(obj["bend"], 0.0);
    out.bendVelocity   = dv(obj["bendVelocity"], 0.0);
    out.rootStress     = dv(obj["rootStress"], 0.0);
    out.windX          = dv(obj["windX"], 0.0);
    out.windY          = dv(obj["windY"], 0.0);
    out.deathX         = dv(obj["deathX"], 0.0);
    out.deathY         = dv(obj["deathY"], 0.0);
    if (!obj["behavior"].isVoid()) out.behavior = parseBehavior(obj["behavior"].toString());
    if (!obj["death"].isVoid() && obj["death"].toString() != "null")
        out.death = parseDeath(obj["death"].toString());
    return true;
}

// ─── parseFishSwimming ────────────────────────────────────────────────────────

bool Persistence::parseFishSwimming(const juce::var& obj, FishSwimming& out) {
    if (!obj.isObject()) return false;
    out.targetX       = dv(obj["targetX"], 0.5);
    out.targetY       = dv(obj["targetY"], 0.85);
    out.velocityX     = dv(obj["velocityX"], 0.0);
    out.velocityY     = dv(obj["velocityY"], 0.0);
    out.restRemaining = dv(obj["restRemaining"], 0.0);
    out.heading       = dv(obj["heading"], 0.0);
    out.seed          = u32v(obj["seed"], 1);
    return true;
}

// ─── parseMotion ─────────────────────────────────────────────────────────────

bool Persistence::parseMotion(const juce::var& obj, EcologyMotion& out) {
    if (!obj.isObject()) return false;
    out.height        = dv(obj["height"], 0.0);
    out.vx = dv(obj["vx"],0); out.vy = dv(obj["vy"],0); out.vz = dv(obj["vz"],0);
    out.qx = dv(obj["qx"],0); out.qy = dv(obj["qy"],0); out.qz = dv(obj["qz"],0); out.qw = dv(obj["qw"],1);
    out.wx = dv(obj["wx"],0); out.wy = dv(obj["wy"],0); out.wz = dv(obj["wz"],0);
    out.growth        = dv(obj["growth"], 1.0);
    out.natural       = bv(obj["natural"], false);
    out.escapeTime    = dv(obj["escapeTime"], 0.0);
    out.jumpRemaining = dv(obj["jumpRemaining"], 0.0);
    out.jumpCooldown  = dv(obj["jumpCooldown"], 0.0);
    out.attackPending = bv(obj["attackPending"], false);
    out.dayFade       = dv(obj["dayFade"], 0.0);
    out.recovering    = dv(obj["recovering"], 0.0);
    if (!obj["beachedTime"].isVoid()) out.beachedTime = dv(obj["beachedTime"], -1.0);
    if (!obj["waveCarried"].isVoid()) out.waveCarried = dv(obj["waveCarried"], -1.0);
    if (!obj["floodDose"].isVoid())   out.floodDose   = dv(obj["floodDose"], -1.0);
    return true;
}

// ─── parseEcologyBody ─────────────────────────────────────────────────────────

bool Persistence::parseEcologyBody(const juce::var& obj, int version, EcologyBody& out) {
    if (!obj.isObject()) return false;
    out.kind          = parseKind(obj["kind"].toString());
    out.cycleProgress = dv(obj["cycleProgress"], 0.05);
    out.seed          = u32v(obj["seed"], 1);
    out.appearanceSeed = u32v(obj["appearanceSeed"], out.seed);

    const auto& sw = obj["swimming"];
    if (!sw.isVoid() && sw.isObject()) {
        FishSwimming swim(out.seed);
        if (parseFishSwimming(sw, swim)) out.swimming = swim;
    } else if (out.kind == EcologyKind::fish && version >= 4) {
        out.swimming = FishSwimming(out.seed);
    }

    const auto& li = obj["life"];
    if (!li.isVoid() && li.isObject()) {
        EcologyLife life;
        if (parseLife(li, out.kind, life)) out.life = life;
    } else if (version >= 5) {
        out.life = EcologyLife{};
    }

    const auto& mo = obj["motion"];
    if (!mo.isVoid() && mo.isObject()) {
        EcologyMotion mot;
        if (parseMotion(mo, mot)) out.motion = mot;
    }
    return true;
}

// ─── parseSoundObject ────────────────────────────────────────────────────────

bool Persistence::parseSoundObject(const juce::var& obj, int version, SoundObject& out) {
    if (!obj.isObject()) return false;
    out.id       = static_cast<int32_t>(iv(obj["id"], 0));
    out.material = static_cast<SoundMaterial>(std::max(0, std::min(4, iv(obj["material"], 0))));
    out.x        = dv(obj["x"], 0.5);
    out.y        = dv(obj["y"], 0.5);

    const auto& eco = obj["ecology"];
    if (!eco.isVoid() && eco.isObject()) {
        EcologyBody body;
        if (parseEcologyBody(eco, version, body)) {
            out.ecology = body;
            // In ecology worlds, material must match the kind
            if (version >= 3) out.material = materialForKind(body.kind);
        }
    }
    return out.id > 0;
}

// ─── parseDocument ────────────────────────────────────────────────────────────

bool Persistence::parseDocument(const juce::var& root, WorldDocument& doc, juce::String& err) {
    if (!root.isObject()) { err = "Root is not a JSON object"; return false; }
    const int version = iv(root["schemaVersion"], 0);
    if (version < 1 || version > 11) {
        err = "Unsupported schema version: " + juce::String(version); return false;
    }
    doc.schemaVersion = 11;

    if (!parseSettings(root["settings"], version, doc.settings)) {
        err = "Failed to parse settings"; return false;
    }
    if (version >= 2) {
        if (!parseEnvironment(root["environment"], doc.environment)) {
            err = "Failed to parse environment"; return false;
        }
    }
    const auto& kindStr = root["ecologicalPlacement"];
    if (!kindStr.isVoid()) {
        if (kindStr.toString() == "null" || kindStr.isVoid()) doc.ecologicalPlacement = std::nullopt;
        else doc.ecologicalPlacement = parseKind(kindStr.toString());
    } else {
        doc.ecologicalPlacement = EcologyKind::fish;
    }

    const auto& objects = root["objects"];
    if (objects.isArray()) {
        for (int i = 0; i < objects.size(); ++i) {
            SoundObject o;
            if (parseSoundObject(objects[i], version, o)) {
                // Schema migration: pre-v3 only keep ecological objects
                if (version < 3 && !o.ecology) continue;
                // Ensure habitat for schema < 10
                if (o.ecology && version < 10) {
                    o.ecology->life = o.ecology->life.value_or(EcologyLife{});
                    if (o.ecology->kind == EcologyKind::fish && !o.ecology->swimming)
                        o.ecology->swimming = FishSwimming(o.ecology->seed);
                    o.ecology->motion.reset();
                }
                doc.objects.push_back(std::move(o));
            }
        }
    }

    // Assign fresh IDs for pre-v3
    if (version < 3) {
        for (int i = 0; i < (int)doc.objects.size(); ++i)
            doc.objects[i].id = static_cast<int32_t>(i + 1);
    }

    // Compute nextID
    int32_t maxID = 0;
    for (auto& o : doc.objects) maxID = std::max(maxID, o.id);
    doc.nextID = maxID + 1;

    return true;
}

// ─── load ─────────────────────────────────────────────────────────────────────

bool Persistence::load(const juce::File& file, WorldDocument& doc, juce::String& errorMessage) {
    if (!file.existsAsFile()) { errorMessage = "File not found: " + file.getFullPathName(); return false; }
    const auto json = juce::JSON::parse(file.loadFileAsString());
    if (!json.isObject()) { errorMessage = "Not valid JSON"; return false; }
    doc = {};
    return parseDocument(json, doc, errorMessage);
}

// ─── Encoding ────────────────────────────────────────────────────────────────

juce::var Persistence::encodeEnvironment(const EnvironmentState& e) {
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty("dayPhase",    e.dayPhase);
    obj->setProperty("dayDuration", e.dayDuration);
    obj->setProperty("wind",        e.wind);
    obj->setProperty("weather",     encodeWeather(e.weather));
    obj->setProperty("isRunning",   e.isRunning);
    obj->setProperty("elapsed",     e.elapsed);
    obj->setProperty("automaticWeather", e.automaticWeather);
    if (e.weatherCycle) {
        auto wc = std::make_unique<juce::DynamicObject>();
        wc->setProperty("seed",      static_cast<int64_t>(e.weatherCycle->seed));
        wc->setProperty("remaining", e.weatherCycle->remaining);
        wc->setProperty("last",      encodeWeather(e.weatherCycle->last));
        wc->setProperty("rain",      e.weatherCycle->rain);
        wc->setProperty("cloud",     e.weatherCycle->cloud);
        wc->setProperty("wind",      e.weatherCycle->wind);
        obj->setProperty("weatherCycle", wc.release());
    }
    return obj.release();
}

juce::var Persistence::encodeEcologyBody(const EcologyBody& b) {
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty("kind",          encodeKind(b.kind));
    obj->setProperty("cycleProgress", b.cycleProgress);
    obj->setProperty("seed",          static_cast<int64_t>(b.seed));
    obj->setProperty("appearanceSeed",static_cast<int64_t>(b.appearanceSeed));
    if (b.swimming) {
        auto sw = std::make_unique<juce::DynamicObject>();
        sw->setProperty("targetX",       b.swimming->targetX);
        sw->setProperty("targetY",       b.swimming->targetY);
        sw->setProperty("velocityX",     b.swimming->velocityX);
        sw->setProperty("velocityY",     b.swimming->velocityY);
        sw->setProperty("restRemaining", b.swimming->restRemaining);
        sw->setProperty("heading",       b.swimming->heading);
        sw->setProperty("seed",          static_cast<int64_t>(b.swimming->seed));
        obj->setProperty("swimming", sw.release());
    }
    if (b.life) {
        auto li = std::make_unique<juce::DynamicObject>();
        li->setProperty("age",           b.life->age);
        li->setProperty("size",          (double)b.life->size);
        li->setProperty("generation",    b.life->generation);
        li->setProperty("juvenile",      b.life->juvenile);
        li->setProperty("breedCooldown", b.life->breedCooldown);
        li->setProperty("depthFraction", b.life->depthFraction);
        li->setProperty("flightX",       b.life->flightX);
        li->setProperty("flightY",       b.life->flightY);
        li->setProperty("uprooted",      b.life->uprooted);
        li->setProperty("growth",        b.life->growth);
        li->setProperty("shelterTime",   b.life->shelterTime);
        li->setProperty("shelterID",     b.life->shelterID);
        li->setProperty("behavior",      encodeBehavior(b.life->behavior));
        li->setProperty("species",       b.life->species);
        li->setProperty("speciesSeed",   static_cast<int64_t>(b.life->speciesSeed));
        li->setProperty("lifeRng",       static_cast<int64_t>(b.life->lifeRng));
        li->setProperty("lifespan",      b.life->lifespan);
        li->setProperty("senescence",    b.life->senescence);
        li->setProperty("flightVX",      b.life->flightVX);
        li->setProperty("flightVY",      b.life->flightVY);
        li->setProperty("flightWait",    b.life->flightWait);
        li->setProperty("dayFade",       b.life->dayFade);
        li->setProperty("bend",          b.life->bend);
        li->setProperty("bendVelocity",  b.life->bendVelocity);
        li->setProperty("rootStress",    b.life->rootStress);
        li->setProperty("windX",         b.life->windX);
        li->setProperty("windY",         b.life->windY);
        // Only encode death if present
        if (b.life->death != LifeDeath::none) {
            static const char* deathNames[] = {"stranded","eaten","oldAge","blownAway","dawn","drowned"};
            int di = static_cast<int>(b.life->death);
            if (di >= 0 && di < 6) li->setProperty("death", juce::String(deathNames[di]));
            li->setProperty("deathX", b.life->deathX);
            li->setProperty("deathY", b.life->deathY);
        }
        obj->setProperty("life", li.release());
    }
    if (b.motion) {
        auto mo = std::make_unique<juce::DynamicObject>();
        mo->setProperty("height", b.motion->height);
        mo->setProperty("vx", b.motion->vx); mo->setProperty("vy", b.motion->vy); mo->setProperty("vz", b.motion->vz);
        mo->setProperty("qx", b.motion->qx); mo->setProperty("qy", b.motion->qy);
        mo->setProperty("qz", b.motion->qz); mo->setProperty("qw", b.motion->qw);
        mo->setProperty("wx", b.motion->wx); mo->setProperty("wy", b.motion->wy); mo->setProperty("wz", b.motion->wz);
        mo->setProperty("growth", b.motion->growth);
        mo->setProperty("natural", b.motion->natural);
        mo->setProperty("escapeTime", b.motion->escapeTime);
        mo->setProperty("jumpRemaining", b.motion->jumpRemaining);
        mo->setProperty("jumpCooldown", b.motion->jumpCooldown);
        mo->setProperty("attackPending", b.motion->attackPending);
        mo->setProperty("dayFade", b.motion->dayFade);
        mo->setProperty("recovering", b.motion->recovering);
        obj->setProperty("motion", mo.release());
    }
    return obj.release();
}

juce::var Persistence::encodeObject(const SoundObject& o) {
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty("id",       (int)o.id);
    obj->setProperty("material", (int)o.material);
    obj->setProperty("x",        o.x);
    obj->setProperty("y",        o.y);
    if (o.ecology) obj->setProperty("ecology", encodeEcologyBody(*o.ecology));
    return obj.release();
}

juce::var Persistence::encodeDocument(const WorldDocument& doc) {
    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("schemaVersion", 11);

    // Settings (schema >= 3 only encodes music settings)
    auto settings = std::make_unique<juce::DynamicObject>();
    settings->setProperty("bpm",  doc.settings.bpm);
    settings->setProperty("grid", doc.settings.grid);
    settings->setProperty("scale", encodeScaleKey(doc.settings.scale));
    settings->setProperty("root", doc.settings.root);
    root->setProperty("settings", settings.release());

    root->setProperty("environment", encodeEnvironment(doc.environment));

    juce::Array<juce::var> objsArray;
    // Re-number IDs for file storage
    int newID = 1;
    for (auto& o : doc.objects) {
        if (!o.ecology) continue;
        if (o.departure) continue;
        if (o.ecology->life && o.ecology->life->death != LifeDeath::none) continue;
        SoundObject copy = o;
        copy.id = static_cast<int32_t>(newID++);
        objsArray.add(encodeObject(copy));
    }
    root->setProperty("objects", objsArray);

    if (doc.ecologicalPlacement)
        root->setProperty("ecologicalPlacement", encodeKind(*doc.ecologicalPlacement));
    else
        root->setProperty("ecologicalPlacement", juce::var{});

    return root.release();
}

bool Persistence::save(const juce::File& file, const WorldDocument& doc, juce::String& errorMessage) {
    const juce::var jsonDoc = encodeDocument(doc);
    const juce::String text = juce::JSON::toString(jsonDoc, false);
    if (!file.replaceWithText(text)) {
        errorMessage = "Could not write to " + file.getFullPathName();
        return false;
    }
    return true;
}

juce::File Persistence::defaultSaveFile() {
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Islecho").getChildFile("world.json");
}
