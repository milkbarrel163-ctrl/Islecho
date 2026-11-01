#pragma once
#include "IslechoTypes.h"
#include <juce_core/juce_core.h>
#include <vector>
#include <optional>
#include <string>

struct WorldDocument {
    int schemaVersion = 11;
    CompositionSettings settings;
    EnvironmentState environment;
    std::vector<SoundObject> objects;
    std::optional<EcologyKind> ecologicalPlacement;
    int32_t nextID = 1;
};

class Persistence {
public:
    // Returns true and populates doc on success; sets errorMessage on failure.
    static bool load(const juce::File& file, WorldDocument& doc, juce::String& errorMessage);
    static bool save(const juce::File& file, const WorldDocument& doc, juce::String& errorMessage);

    // Returns the default world save location for macOS.
    static juce::File defaultSaveFile();

private:
    static bool parseDocument(const juce::var& root, WorldDocument& doc, juce::String& err);
    static bool parseSettings(const juce::var& obj, int version, CompositionSettings& out);
    static bool parseEnvironment(const juce::var& obj, EnvironmentState& out);
    static bool parseSoundObject(const juce::var& obj, int version, SoundObject& out);
    static bool parseEcologyBody(const juce::var& obj, int version, EcologyBody& out);
    static bool parseMotion(const juce::var& obj, EcologyMotion& out);
    static bool parseLife(const juce::var& obj, EcologyKind kind, EcologyLife& out);
    static bool parseFishSwimming(const juce::var& obj, FishSwimming& out);

    static juce::var encodeDocument(const WorldDocument& doc);
    static juce::var encodeObject(const SoundObject& o);
    static juce::var encodeEcologyBody(const EcologyBody& b);
    static juce::var encodeEnvironment(const EnvironmentState& e);

    static MusicalScaleType parseScaleKey(const juce::String& key);
    static juce::String encodeScaleKey(MusicalScaleType t);
    static EcologyKind parseKind(const juce::String& s);
    static juce::String encodeKind(EcologyKind k);
    static EcologyWeather parseWeather(const juce::String& s);
    static juce::String encodeWeather(EcologyWeather w);
    static LifeBehavior parseBehavior(const juce::String& s);
    static juce::String encodeBehavior(LifeBehavior b);
    static LifeDeath parseDeath(const juce::String& s);
};
