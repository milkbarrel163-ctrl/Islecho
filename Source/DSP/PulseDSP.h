#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PFCore PFCore;
enum { PF_MAX_OBJECTS = 64, PF_MAX_LOOP_NOTES = 16, PF_MAX_LOOPS = 8 };
typedef struct {
    double beat, bpm, sampleRate;
    uint64_t renderedFrames;
    uint32_t rejectedCommands, droppedVoices;
} PFClockState;
typedef struct {
    int32_t id;
    bool alive;
    float energy, pan, frequency;
    double scheduledBeat, lastOnsetBeat;
    uint32_t onsetCount;
    float cutoffHz; // Per-object target; zero means bypass.
    double lastOnsetSeconds; // Sample time, independent of changing tempo.
} PFObjectState;

enum { PF_WAVE_CHANNELS = 8 };
typedef struct { float frequency, pan, cutoffHz, gain; } PFWaveSound;
typedef struct {
    PFWaveSound waves[PF_WAVE_CHANNELS];
    float wind;
    bool enabled;
} PFEnvironmentSound;
// One bounded main-thread command. All cutoffs are validated in [500,2000] Hz.
bool pf_set_environment(PFCore *, const PFEnvironmentSound *);

enum { PF_SONG_NOTES = 16 };
typedef struct { int32_t id; int midi; double offset; } PFSongNote;
typedef struct {
    PFSongNote notes[PF_SONG_NOTES];
    int count, scaleCount, root;
    int scale[7];
    double bpm, lengthBeats, durationSeconds;
} PFSongScore;
typedef struct { bool active; float blend; double bpm, remainingBeats; int chord; } PFSongState;
// Main-thread bounded score command; all playback/evolution occurs on audio time.
bool pf_song_start(PFCore *, const PFSongScore *);
bool pf_song_stop(PFCore *);
void pf_song_hold(PFCore *, int32_t id, bool held);
PFSongState pf_song_state(const PFCore *);

// Lifecycle: create/destroy/reset ONLY with the audio engine stopped.
PFCore *pf_create(double sampleRate);
void pf_destroy(PFCore *core);
void pf_reset(PFCore *core, double sampleRate);
// All command producers MUST run on the main thread (one producer).
bool pf_define(PFCore *, int32_t id, int material, float frequency, float pan);
bool pf_trigger(PFCore *, int32_t id, int material, float frequency, float pan, double beat);
// Trigger plus initial tone target are one command, so the first attack uses its environment.
// Ecological attacks start on the next audio buffer without musical quantization.
// Each immediate event latches its own pitch, including overlapping tails.
bool pf_trigger_now(PFCore *, int32_t id, int material, float frequency, float pan, double requestedBeat, float cutoffHz);
// Same bounded immediate path, with a short species-specific alarm envelope.
bool pf_trigger_alarm(PFCore *, int32_t id, int material, float frequency, float pan, double requestedBeat, float cutoffHz, float alarm);
bool pf_trigger_with_cutoff(PFCore *, int32_t id, int material, float frequency, float pan, double beat, float cutoffHz);
// Main-thread producer; caller coalesces updates. Existing voices smoothly follow the target.
bool pf_set_object_cutoff(PFCore *, int32_t id, float cutoffHz);
bool pf_move(PFCore *, int32_t id, float frequency, float pan);
// Spatial movement changes pan only; never overwrites a queued random note.
bool pf_pan(PFCore *, int32_t id, float pan);
bool pf_remove(PFCore *, int32_t id, float seconds);
bool pf_loop(PFCore *, const int32_t *ids, const double *offsets, int count,
             double anchorBeat, double lengthBeats);
// Stable identity: updates replace the sequence at its next cycle boundary.
// Multiple updates before that boundary coalesce; existing voices keep their tails.
bool pf_set_loop(PFCore *, int32_t loopID, const int32_t *ids, const double *offsets,
                 int count, double anchorBeat, double lengthBeats);
bool pf_configure(PFCore *, double bpm, double gridBeats);
// Continuous controls use atomic mailboxes, so gestures cannot flood the queue.
void pf_set_macro(PFCore *, float openness);
// Atomic target, ramped per sample over at most 20 ms for composition recall.
void pf_set_output_gain(PFCore *, float gain);
void pf_set_texture(PFCore *, float density, float spread);
// Global soft (0) to sharp (1) character; targets are smoothed on the audio thread.
void pf_set_character(PFCore *, float character);


typedef struct { int32_t id; double requestedBeat, scheduledBeat; } PFScheduleAck;
bool pf_poll_ack(PFCore *, PFScheduleAck *);
PFClockState pf_clock(const PFCore *);
PFObjectState pf_object(const PFCore *, int32_t id);
// Render: non-interleaved stereo Float32; no allocations, locks or OS calls.
void pf_render(PFCore *, float *left, float *right, uint32_t frames);
#ifdef __cplusplus
}
#endif
