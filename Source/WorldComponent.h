#pragma once
#include "EcologySimulation.h"
#include "Persistence.h"
#include "SongMode.h"
#include "EnvironmentAudio.h"
#include "PulseDSP.h"
#include "RecordingCapture.h"
#include "IslandScene.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <set>
#include <random>

// Full JUCE macOS world: ecology simulation + audio + 2D rendering + persistence.
class WorldComponent final : public juce::AudioAppComponent,
                              private juce::Timer {
public:
    WorldComponent();
    ~WorldComponent() override;

    // AudioAppComponent
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;
    void checkpointForInterruption();
    void resumeAfterInterruption();

    // Component
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&,const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&,float scaleFactor) override;
    bool keyPressed(const juce::KeyPress&) override;

    void showPreferences();
    bool isRecording() const { return recording.load(); }
    void toggleRecording();

private:
    // ── Audio ─────────────────────────────────────────────────────────────
    PFCore* core = nullptr;
    PFRecordingCapture* capture = nullptr;
    std::atomic<bool> needsRestore{false};
    std::atomic<bool> recording{false};
    double currentSampleRate = 48000.0;
    double recordingSampleRate = 48000.0;
    double lastAutoSaveTime = 0.0;
    double stormUntil = 0.0;
    bool saveAllowed = true;

    // ── World state ──────────────────────────────────────────────────────
    WorldDocument world;
    EcologySimulation ecology;
    juce::CriticalSection worldLock;

    // ── Interaction ──────────────────────────────────────────────────────
    struct Contact {
        int32_t id;
        juce::Point<float> origin, previous, grabOffset;
        float frequency;
        double beganAt;
        bool moved = false;
        bool musicalTap = true;
        bool voiceHeld = false;
        double lastDragAt = 0.0;
        double dragVX = 0.0, dragVY = 0.0;
    };
    std::map<juce::uint64, Contact> contacts;
    std::map<juce::uint64, juce::Point<float>> emptyContacts;
    std::set<int32_t> heldIDs;
    int32_t highlightedID = 0;
    double highlightUntil = 0;
    // Note randomizer
    std::mt19937 pitchRng{std::random_device{}()};

    // ── Song mode ──────────────────────────────────────────────────────────
    RhythmLearning rhythm;
    EcologyMusicBridge ecologyBridge;
    juce::String songStatus;
    void stopSong();

    // ── Environment audio ─────────────────────────────────────────────────
    EnvironmentAudioBridge envAudio;

    // ── Pending life queue ────────────────────────────────────────────────
    struct PendingLife {
        SoundObject object;
        std::optional<EcologyBirth> parents;
    };
    std::vector<PendingLife> pendingLife;
    std::map<int32_t, double> birthRetryAfter;
    void clearBirthWork();
    std::optional<std::pair<size_t, size_t>> parentIndices(const EcologyBirth&) const;

    // ── Notice ────────────────────────────────────────────────────────────
    juce::String notice;
    double noticeUntil = 0;

    // ── Timer callback ────────────────────────────────────────────────────
    void timerCallback() override;
    void ecologyTick(double time);
    void commitDeaths(double time);
    void advanceBirths();

    // ── DSP helpers ────────────────────────────────────────────────────────
    void restoreVoices();
    float panFor(double x) const;
    float nextNoteHz(double alarm = 0.0);
    float cutoffForKind(EcologyKind kind) const;

    // ── Three-dimensional scenery (presentation only) ──────────────────
    IslandScene island;

    // ── Rendering ─────────────────────────────────────────────────────────
    juce::Rectangle<float> worldBounds() const;
    juce::Point<float> screenPoint(double wx, double wy) const;
    juce::Point<double> worldPoint(juce::Point<float> screen) const;
    SoundObject* hitTest(juce::Point<float> screen);

    void paintBackground(juce::Graphics&);
    void paintOrganisms(juce::Graphics&);
    void paintControls(juce::Graphics&);

    // ── Preferences window (opened via macOS Menu Bar → Preferences…) ────
    std::unique_ptr<juce::DocumentWindow> prefsWindow;
    static juce::File recordingsDirectory();

    // ── Persistence ────────────────────────────────────────────────────────
    void loadWorld();
    void saveWorld();

    // ── Recording ─────────────────────────────────────────────────────────
    void startRecording();
    void stopRecording();
    void finalizeRecording();
    std::unique_ptr<juce::AudioFormatWriter> wavWriter;
    juce::File recordingFile;
    juce::Thread::ThreadID recordingThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WorldComponent)
};
