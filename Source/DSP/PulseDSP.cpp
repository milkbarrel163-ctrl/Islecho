#include "PulseDSP.h"
#include "EnvironmentSynth.h"
#include "SongTransport.h"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int kVoices = 48, kEvents = 256, kQueue = 256, kTable = 4096;
constexpr double pi = 3.14159265358979323846;
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
float clamp(float x, float lo, float hi) { return std::max(lo, std::min(x, hi)); }
enum class Kind { define, trigger, move, pan, remove, loop, setLoop, configure, cutoff, environment, songStart };
struct Command {
    Kind kind{};
    PFEnvironmentSound environment{};
    PFSongScore song{};
    uint32_t songEpoch=0;
    int32_t id = 0;
    int material = 0, count = 0;
    float frequency = 220, pan = 0, seconds = .3f, cutoffHz = 0;
    bool appliesCutoff = false, immediate = false;
    float alarm = 0;
    double beat = 0, length = 4, bpm = 120, grid = .25;
    std::array<int32_t, PF_MAX_LOOP_NOTES> ids{};
    std::array<double, PF_MAX_LOOP_NOTES> offsets{};
};
struct Object {
    int32_t id = 0;
    int material = 0;
    float frequency = 220, pan = 0, left = .7071f, right = .7071f;
    float gain = 1, fadeStep = 0, energy = 0;
    float cutoffHz = 0, cutoffTarget = 1;
    bool removing = false;
    double scheduledBeat = -1, lastOnsetBeat = -1, lastOnsetSeconds = -1;
    uint64_t busyUntil=0;
    uint32_t onsetCount = 0;
};
struct PublishedObject {
    std::atomic<int32_t> id{0};
    std::atomic<float> energy{0}, frequency{220}, pan{0}, cutoffHz{0};
    std::atomic<double> scheduledBeat{-1}, lastOnsetBeat{-1}, lastOnsetSeconds{-1};
    std::atomic<uint32_t> onsetCount{0};
};
struct Event { bool active = false; int slot = 0; double next = 0, period = 0; float attackFrequency = 0, alarm = 0; };
struct ManagedLoop {
    int32_t id = 0;
    Command sequence{}, pending{};
    std::array<double, PF_MAX_LOOP_NOTES> next{};
    double anchor = 0, switchBeat = 0;
    bool hasPending = false;
};
struct Grain { bool active = false; float age = 0, length = 0, phase = 0, ratio = 1; };
struct Voice {
    bool active = false;
    bool followsObjectPitch = true;
    bool songVoice=false;
    int slot = 0, material = 0;
    uint64_t age = 0, length = 0;
    float phase = 0, phase2 = 0, frequency = 220, left = .7f, right = .7f;
    float decay = 1, decayStep = 1, noiseLP = 0, grainWait = 0;
    float filterCoeff = 1, filter1 = 0, filter2 = 0;
    uint32_t pulseSamples = 1, random = 1;
    std::array<Grain, 8> grains{};
    struct Mode { float a=0,b=0,y1=0,y2=0; };
    std::array<Mode,6> modes{};
    float alarm = 0;
};
float random(Voice &v) {
    v.random ^= v.random << 13; v.random ^= v.random >> 17; v.random ^= v.random << 5;
    return float(v.random & 0xFFFFFF) / 16777216.f;
}
}

struct PFCore {
    EnvironmentSynth environment;
    SongTransport song;
    std::atomic<uint32_t> songEpoch{0};
    uint32_t seenSongEpoch=0;
    std::array<std::atomic<int32_t>,PF_MAX_OBJECTS> songHeld{};
    std::atomic<bool> songActive{false};
    std::atomic<float> songBlend{0};
    std::atomic<double> songRemaining{0};
    std::atomic<int> songChord{0};
    double sr = 48000, beat = 0, bpm = 120, grid = .25;
    uint64_t frame = 0;
    std::array<float, kTable + 1> sine{};
    std::array<Object, PF_MAX_OBJECTS> objects{};
    std::array<PublishedObject, PF_MAX_OBJECTS> published{};
    std::array<Voice, kVoices> voices{};
    std::array<Event, kEvents> events{};
    std::array<ManagedLoop, PF_MAX_LOOPS> loops{};
    std::array<Command, kQueue> commands{};
    std::array<PFScheduleAck, kQueue> acknowledgements{};
    std::atomic<uint32_t> ackRead{0}, ackWrite{0};
    alignas(64) std::atomic<uint32_t> read{0};
    alignas(64) std::atomic<uint32_t> write{0};
    std::atomic<uint32_t> rejected{0}, dropped{0};
    std::atomic<double> publicBeat{0}, publicBPM{120}, publicSR{48000};
    std::atomic<uint64_t> publicFrame{0};
    std::atomic<float> macro{.72f}, density{32}, spread{.18f};
    std::atomic<float> character{.5f};
    float timbre = .5f, timbreSmoothing = 0, grainDensity = 32, grainSpread = .18f, pulseLoss = 0;
    std::atomic<float> outputTarget{1};
    float outputGain = 1, outputStep = 0;
    float toneSmoothing = 0;
    float smoothing = 0, filterCoeff = .8f, filterL1 = 0, filterL2 = 0, filterR1 = 0, filterR2 = 0;
    float dcLIn = 0, dcLOut = 0, dcRIn = 0, dcROut = 0, dcCoeff = .995f;
    double earliest = std::numeric_limits<double>::infinity();

    explicit PFCore(double rate) {
        for (int i = 0; i <= kTable; ++i) sine[i] = float(std::sin(2 * pi * i / kTable));
        reset(rate);
    }
    void reset(double rate) {
        sr = std::isfinite(rate) && rate >= 8000 ? rate : 48000;
        beat = 0; frame = 0; bpm = 120; grid = .25;
        for(auto &id:songHeld)id.store(0);
        objects.fill({}); voices.fill({}); events.fill({}); loops.fill({});
        read.store(0); write.store(0); ackRead.store(0); ackWrite.store(0); rejected.store(0); dropped.store(0);
        environment.reset(sr);song={};songEpoch.store(0);seenSongEpoch=0;songActive.store(false);songBlend.store(0);songRemaining.store(0);songChord.store(0);
        smoothing = float(1 - std::exp(-1 / (.015 * sr)));
        timbreSmoothing = float(1 - std::exp(-1 / (.030 * sr)));
        toneSmoothing = float(1 - std::exp(-1 / (.080 * sr)));
        timbre = character.load(); grainDensity = density.load(); grainSpread = spread.load();
        pulseLoss = float(1 - std::exp(-1 / (sr * (.080 - .070 * timbre))));
        dcCoeff = float(std::exp(-2 * pi * 15 / sr));
        filterCoeff = .8f; filterL1 = filterL2 = filterR1 = filterR2 = 0;
        dcLIn = dcLOut = dcRIn = dcROut = 0;
        outputGain = 1; outputTarget.store(1); outputStep = float(1 / (.020 * sr));
        earliest = std::numeric_limits<double>::infinity();
        publicSR.store(sr); publish();
    }
    float sin(float phase) const {
        phase -= std::floor(phase);
        float f = phase * kTable; int i = int(f);
        return sine[i] + (sine[i + 1] - sine[i]) * (f - i);
    }
    float partial(float phase, float increment, float harmonic) const {
        // Fade upper partials before Nyquist, including high-pitched scattered grains.
        const float gain = clamp((.48f - increment * harmonic) / .08f, 0, 1);
        return gain * sin(phase * harmonic);
    }
    float window(float fraction) const { return .5f - .5f * sin(fraction + .25f); }
    bool push(const Command &c) {
        auto w = write.load(std::memory_order_relaxed);
        auto next = (w + 1) % kQueue;
        if (next == read.load(std::memory_order_acquire)) { rejected.fetch_add(1); return false; }
        commands[w] = c;
        write.store(next, std::memory_order_release);
        return true;
    }
    int find(int32_t id) const {
        for (int i = 0; i < PF_MAX_OBJECTS; ++i) if (objects[i].id == id) return i;
        return -1;
    }
    void position(Object &o, float frequency, float pan) {
        o.frequency = clamp(frequency, 30, float(sr * .18));
        o.pan = clamp(pan, -1, 1);
        o.left = std::cos((o.pan + 1) * float(pi / 4));
        o.right = std::sin((o.pan + 1) * float(pi / 4));
    }
    void setCutoff(Object &o, float hz) {
        o.cutoffHz = hz == 0 ? 0 : clamp(hz, 20, float(std::min(20000., sr * .4)));
        o.cutoffTarget = hz == 0 ? 1 : float(1 - std::exp(-2 * pi * o.cutoffHz / sr));

    }
    void refreshEarliest() {
        earliest = std::numeric_limits<double>::infinity();
        for (const auto &e : events) if (e.active) earliest = std::min(earliest, e.next);
        for (const auto &loop : loops) if (loop.id) {
            if (loop.hasPending) earliest = std::min(earliest, loop.switchBeat);
            for (int i = 0; i < loop.sequence.count; ++i)
                if (loop.sequence.ids[i]) earliest = std::min(earliest, loop.next[i]);
        }
    }
    void install(ManagedLoop &loop, const Command &sequence, double start) {
        loop.sequence = sequence;
        for (int i = 0; i < sequence.count; ++i) loop.next[i] = start + sequence.offsets[i];
    }
    void setLoop(const Command &c) {
        for (int i = 0; i < c.count; ++i) {
            const int slot = find(c.ids[i]);
            if (slot < 0 || objects[slot].removing) { rejected.fetch_add(1); return; }
        }
        ManagedLoop *target = nullptr;
        for (auto &loop : loops) if (loop.id == c.id) { target = &loop; break; }
        if (target) {
            // Reordering never changes period or phase, including through BPM changes.
            if (c.length != target->sequence.length) { rejected.fetch_add(1); return; }
            if (!target->hasPending) {
                const double cycles = std::max(0., std::ceil((beat + 1e-9 - target->anchor) / c.length));
                target->switchBeat = target->anchor + cycles * c.length;
            }
            target->pending = c; target->hasPending = true;
        } else {
            for (auto &loop : loops) if (!loop.id) { target = &loop; break; }
            if (!target) { rejected.fetch_add(1); return; }
            double start = c.beat;
            if (start <= beat + grid)
                start += (std::floor((beat + grid - start) / c.length) + 1) * c.length;
            target->id = c.id; target->anchor = start;
            install(*target, c, start);
        }
        refreshEarliest();
    }
    void removeFromLoops(int32_t id) {
        for (auto &loop : loops) if (loop.id) {
            bool survives = false;
            for (int i = 0; i < loop.sequence.count; ++i) {
                if (loop.sequence.ids[i] == id) loop.sequence.ids[i] = 0;
                survives |= loop.sequence.ids[i] != 0;
            }
            if (loop.hasPending) for (int i = 0; i < loop.pending.count; ++i) {
                if (loop.pending.ids[i] == id) loop.pending.ids[i] = 0;
                survives |= loop.pending.ids[i] != 0;
            }
            if (!survives) loop = {};
        }
    }
    bool schedule(int slot, double next, double period, float attackFrequency = 0, float alarm = 0) {
        for (auto &e : events) if (!e.active) {
            e = {true, slot, next, period, attackFrequency, alarm}; earliest = std::min(earliest, next); return true;
        }
        rejected.fetch_add(1); return false;
    }
    void drain() {
        const auto epoch=songEpoch.load(std::memory_order_acquire);
        if(epoch!=seenSongEpoch) {song.stop();seenSongEpoch=epoch;}
        auto r = read.load(std::memory_order_relaxed);
        // Snapshot producer index: bounded work even if UI keeps sending commands.
        const auto end = write.load(std::memory_order_acquire);
        while (r != end) {
            const auto &c = commands[r];
            int slot = find(c.id);
            if (c.kind == Kind::environment) { environment.set(c.environment,sr); }
            else if(c.kind==Kind::songStart && c.songEpoch==seenSongEpoch) {
                const bool starting=!song.running;
                song.start(c.song,beat);
                if(starting) { bpm=c.song.bpm;song.updateHarmony(0);environment.harmony(song.harmony,sr); }
            }
            else if (c.kind == Kind::configure) {
                bpm = std::max(40., std::min(c.bpm, 200.)); grid = std::max(.125, std::min(c.grid, 1.));
            } else if ((c.kind == Kind::trigger || c.kind == Kind::define)) {
                if (slot < 0) for (int i = 0; i < PF_MAX_OBJECTS; ++i) if (!objects[i].id) {
                    slot = i; objects[i] = {}; objects[i].id = c.id; break;
                }
                if (slot >= 0 && !objects[slot].removing) {
                    auto &o = objects[slot];
                    o.material = std::max(0, std::min(c.material, 4)); position(o, c.frequency, c.pan);
                    if (c.appliesCutoff) setCutoff(o, c.cutoffHz);
                    // A late command always rolls FORWARD; it never fires off-grid.
                    double target = c.beat;
                    if (c.immediate) target = song.running ? song.origin+std::ceil((beat-song.origin+0.0001)*2)/2:beat;
                    else if (target < beat + 2 * bpm / (60 * sr))
                        target = std::ceil((beat + 2 * bpm / (60 * sr)) / grid) * grid;
                    if (c.kind == Kind::trigger && schedule(slot, target, 0, c.immediate ? o.frequency : 0, c.alarm)) {
                        o.scheduledBeat = target;
                        auto w = ackWrite.load(std::memory_order_relaxed), next = (w + 1) % kQueue;
                        if (next != ackRead.load(std::memory_order_acquire)) {
                            acknowledgements[w] = {c.id, c.beat, target};
                            ackWrite.store(next, std::memory_order_release);
                        } else rejected.fetch_add(1);
                    }
                } else rejected.fetch_add(1);
            } else if (c.kind == Kind::cutoff && slot >= 0 && !objects[slot].removing) {
                setCutoff(objects[slot], c.cutoffHz);
            } else if (c.kind == Kind::move && slot >= 0 && !objects[slot].removing) {
                position(objects[slot], c.frequency, c.pan);
            } else if (c.kind == Kind::pan && slot >= 0 && !objects[slot].removing) {
                position(objects[slot], objects[slot].frequency, c.pan);
            } else if (c.kind == Kind::remove && slot >= 0) {
                auto &o = objects[slot]; o.removing = true;
                o.fadeStep = o.gain / (clamp(c.seconds, .08f, 2.f) * float(sr));
                for (auto &e : events) if (e.active && e.slot == slot) e.active = false;
                removeFromLoops(c.id);
                refreshEarliest();
            } else if (c.kind == Kind::setLoop) {
                setLoop(c);
            } else if (c.kind == Kind::loop) {
                int free = 0;
                for (const auto &e : events) if (!e.active) ++free;
                bool valid = c.count >= 1 && c.count <= PF_MAX_LOOP_NOTES && free >= c.count;
                for (int i = 0; i < c.count && valid; ++i) {
                    int s = find(c.ids[i]); valid = s >= 0 && !objects[s].removing;
                }
                if (valid) {
                    double start = c.beat;
                    if (start <= beat + grid)
                        start += (std::floor((beat + grid - start) / c.length) + 1) * c.length;
                    for (int i = 0; i < c.count; ++i) schedule(find(c.ids[i]), start + c.offsets[i], c.length);
                } else rejected.fetch_add(1);
            }
            r = (r + 1) % kQueue;
        }
        read.store(r, std::memory_order_release);
    }
    // Reserve a visible performer until its complete gesture settles. Long audio
    // tails remain in the independent voice pool and are never stolen.
    int performer(int material,int preferred=-1) {
        auto ready=[&](int slot) {
            const auto &o=objects[slot];
            return o.id && !o.removing && o.material==material && frame>=o.busyUntil && songHeld[slot].load()!=o.id;
        };
        if(preferred>=0 && ready(preferred))return preferred;
        for(int i=0;i<PF_MAX_OBJECTS;++i) {
            const int slot=song.cursor++%PF_MAX_OBJECTS;
            if(ready(slot))return slot;
        }
        return -1;
    }
    void startVoice(int slot, float attackFrequency = 0, float alarm = 0, bool inSong=false) {
        if (!objects[slot].id || objects[slot].removing) return;
        if(song.running && !inSong) {
            // Manual/birth attacks also use the allocator during a song. A held
            // requested body may audition only when its previous gesture is done.
            if(frame<objects[slot].busyUntil)slot=performer(objects[slot].material);
            if(slot<0)return;
        }
        for (auto &v : voices) if (!v.active) {
            const auto &o = objects[slot];
            v = {}; v.active = true; v.slot = slot; v.material = o.material;
            v.alarm=alarm;v.songVoice=inSong || song.running;
            v.followsObjectPitch = attackFrequency <= 0;
            v.frequency = v.followsObjectPitch ? o.frequency : attackFrequency;
            v.left = o.left; v.right = o.right;
            v.filterCoeff = o.cutoffTarget;
            v.random = uint32_t(frame + uint64_t(o.id) * 2654435761u) | 1;
            v.pulseSamples = uint32_t(sr * 60 / bpm / 4);
            double seconds = 4.2;
            if (v.material == 1) seconds = .12;
            if (v.material == 2) seconds = 1.8;
            if (v.material == 3) seconds = 3.2;
            if (v.material == 4) seconds = 4 * 60 / bpm;
            // Dense alarm events stay short and bounded; they cannot stack 1.5s
            // flower tails into the fixed 48-voice pool. Envelope still releases.
            if (alarm > .08f) {
                seconds = v.material == 0 ? .11 : v.material == 3 ? .19 : .17;
                v.pulseSamples=uint32_t(sr*.045);
            }
            v.length = uint64_t(seconds * sr);
            if(v.material==0) {
                // Impulse-excited damped resonators: a tuned bell fundamental,
                // slightly inharmonic upper modes and successively shorter tails.
                constexpr float ratios[]={1,2.01f,2.76f,4.07f,5.41f,6.79f};
                constexpr float gains[]={.52f,.23f,.14f,.075f,.04f,.025f};
                for(int i=0;i<6;++i) {
                    const double hz=v.frequency*ratios[i];
                    if(hz>sr*.42)continue;
                    const double tau=alarm>.08f ? .045:(1.55-1.10*timbre)/(1+i*.23);
                    const double radius=std::exp(-1/(sr*tau)),w=2*pi*hz/sr;
                    v.modes[i]={float(2*radius*std::cos(w)),float(-radius*radius),float(std::sin(w)*gains[i]),0};
                }
            }
            v.decay = 1; v.decayStep = float(std::exp(-1 / (sr * (v.material == 1 ? .022 : .045))));
            if(v.material==2)v.decayStep=float(std::exp(-3.0/v.length));
            auto &mutableObject = objects[slot];
            if(inSong || song.running)mutableObject.frequency=v.frequency;
            mutableObject.lastOnsetBeat = beat;mutableObject.lastOnsetSeconds=double(frame)/sr;
            const double gesture=mutableObject.material==0 ? 1.30:mutableObject.material==2 ? 1.15:1.50;
            mutableObject.busyUntil=frame+uint64_t(std::ceil(gesture*sr));
            ++mutableObject.onsetCount;
            return;
        }
        // Reject a new voice rather than abruptly stealing an audible one.
        dropped.fetch_add(1);
    }
    void songTick() {
        if(!song.running || beat+1e-10<song.nextStep)return;
        if(song.hasPending && beat+1e-10>=song.swapAt) {
            song.score=song.pending;song.hasPending=false;bpm=song.score.bpm;
            song.origin=song.swapAt;song.step=0;song.nextStep=song.origin;
            song.end=song.origin+std::ceil(song.score.durationSeconds*bpm/240)*4;
        }
        const double elapsed=song.nextStep-song.origin;
        const int oldChord=song.chord;
        if(beat+1e-10>=song.nextHarmony) {
            song.updateHarmony(song.chord+1);
            song.nextHarmony+=song.score.lengthBeats*4;
        }
        const int section=song.chord;
        if(song.chord!=oldChord)environment.harmony(song.harmony,sr);
        const int pass=int((elapsed+1e-8)/song.score.lengthBeats);
        const double phase=elapsed-pass*song.score.lengthBeats;
        auto available=[&]() {int count=0;for(const auto &v:voices)count+=!v.active;return count>3;};
        auto nextObject=[&](int excluding=-1) {
            for(int i=0;i<PF_MAX_OBJECTS;++i) {
                int slot=song.cursor++%PF_MAX_OBJECTS;
                if(slot!=excluding && objects[slot].id && !objects[slot].removing && songHeld[slot].load()!=objects[slot].id && frame>=objects[slot].busyUntil)return slot;
            }
            return -1;
        };
        int melodySlot=-1;
        for(int n=0;n<song.score.count;++n)if(std::abs(song.score.notes[n].offset-phase)<1e-6) {
            int slot=find(song.score.notes[n].id);
            if(slot>=0)slot=performer(objects[slot].material,slot);
            else slot=nextObject();
            if(slot>=0 && available()) {
                startVoice(slot,SongTransport::frequency(song.melody(n,pass,section)),0,true);melodySlot=slot;
            }
        }
        // The complete living population takes turns; denser worlds don't create
        // unbounded simultaneous attacks. Held creatures are handled by the UI.
        if(song.step%2==0) {
            int slot=nextObject(melodySlot);
            if(slot>=0 && available()) {
                int registerNote=objects[slot].material==0 ? 72:objects[slot].material==2 ? 79:60;
                startVoice(slot,SongTransport::frequency(song.nearestChord(registerNote+song.score.root,section)),0,true);
            }
        }
        ++song.step;song.nextStep=song.origin+song.step*.5;
    }
    float sample(Voice &v) {
        auto &o = objects[v.slot];
        if (v.followsObjectPitch) v.frequency += smoothing * (o.frequency - v.frequency);
        v.left += smoothing * (o.left - v.left); v.right += smoothing * (o.right - v.right);
        float increment = v.frequency / float(sr);
        v.phase += increment; v.phase -= std::floor(v.phase);
        v.phase2 += increment * (1 + .006f * timbre); v.phase2 -= std::floor(v.phase2);
        float attack = v.material == 4 ? .08f : (v.material == 3 ? .24f : v.material == 2 ? .055f : .002f);
        float release = v.material == 4 ? .3f : (v.material == 3 ? .9f : v.material == 2 ? .65f : .22f);
        if (v.alarm > .08f) { release = .025f; attack = .003f; }
        float env = 1;
        if (v.age < attack * sr) env *= window(float(v.age / (attack * sr)) * .5f);
        if (v.length - v.age < release * sr) env *= window(float((v.length - v.age) / (release * sr)) * .5f);
        float s = 0;
        switch (v.material) {
            case 0:
                for(auto &m:v.modes) { float next=m.a*m.y1+m.b*m.y2; m.y2=m.y1;m.y1=next;s+=next; }
                break;
            case 1: {
                float noise = 2 * random(v) - 1;
                v.noiseLP += .28f * (noise - v.noiseLP);
                const float shaped = (1 - timbre) * v.noiseLP + timbre * (noise - v.noiseLP);
                s = shaped * v.decay * 1.1f + sin(v.phase) * v.decay * .25f;
                v.decay *= v.decayStep; break;
            }
            case 2: {
                // High, airy glass halo; a soft slow tremolo replaces hard pulses.
                const float breath=.72f+.28f*sin(float(v.age/sr)*2.1f);
                s=(sin(v.phase)*.66f+partial(v.phase,increment,2)*(.08f+.24f*timbre)+sin(v.phase2)*.14f)*breath;
                s*=v.decay;v.decay*=v.decayStep;break;
            }
            case 3: {
                if (v.grainWait <= 0) {
                    for (auto &g : v.grains) if (!g.active) {
                        g = {true, 0, float(sr * (.12 + random(v) * .15)), random(v),
                             (random(v) < .72f ? 1.f : 2.f) * (1 + (random(v)*2-1)*.002f)}; break;
                    }
                    v.grainWait += float(sr) / grainDensity * (.65f + random(v) * .7f);
                }
                --v.grainWait;
                for (auto &g : v.grains) if (g.active) {
                    g.phase += increment * g.ratio; g.phase -= std::floor(g.phase);
                    s += (sin(g.phase) + .3f * timbre * partial(g.phase, increment * g.ratio, 2))
                         * window(g.age / g.length) * .4f;
                    if (++g.age >= g.length) g.active = false;
                }
                break;
            }
            default: s = (sin(v.phase) + .45f * sin(v.phase2)
                          + .3f * timbre * partial(v.phase, increment, 2)
                          + .12f * timbre * timbre * partial(v.phase, increment, 3)) * .55f; break;
        }
        // A voice owns its filter memory; simultaneous organisms cannot share state.
        v.filterCoeff += toneSmoothing * (o.cutoffTarget - v.filterCoeff);
        if (v.filterCoeff == 1 && o.cutoffTarget == 1) {
            v.filter1 = v.filter2 = s; // Exact historical bypass, ready for a future sweep.
        } else {
            v.filter1 += v.filterCoeff * (s - v.filter1);
            v.filter2 += v.filterCoeff * (v.filter1 - v.filter2);
            if (std::abs(v.filter1) < 1e-20f) v.filter1 = 0;
            if (std::abs(v.filter2) < 1e-20f) v.filter2 = 0;
            s = v.filter2;
        }
        float energy = env * o.gain * (v.songVoice ? song.blend : 1-song.blend);
        o.energy = std::max(o.energy, std::abs(s * energy));
        if (++v.age >= v.length) v.active = false;
        return s * energy * .14f * (1 - .25f*v.alarm);
    }
    void publish() {
        songActive.store(song.running,std::memory_order_relaxed);
        songBlend.store(song.blend,std::memory_order_relaxed);
        songRemaining.store(song.running ? std::max(0.,song.end-beat):0,std::memory_order_relaxed);
        songChord.store(std::max(0,song.chord)%4,std::memory_order_relaxed);
        publicBeat.store(beat, std::memory_order_relaxed); publicBPM.store(bpm, std::memory_order_relaxed);
        publicFrame.store(frame, std::memory_order_relaxed);
        for (int i = 0; i < PF_MAX_OBJECTS; ++i) {
            auto &p = published[i]; const auto &o = objects[i];
            p.energy.store(o.energy, std::memory_order_relaxed); p.frequency.store(o.frequency, std::memory_order_relaxed);
            p.pan.store(o.pan, std::memory_order_relaxed); p.cutoffHz.store(o.cutoffHz, std::memory_order_relaxed); p.scheduledBeat.store(o.scheduledBeat, std::memory_order_relaxed);
            p.lastOnsetBeat.store(o.lastOnsetBeat, std::memory_order_relaxed);p.lastOnsetSeconds.store(o.lastOnsetSeconds,std::memory_order_relaxed); p.onsetCount.store(o.onsetCount, std::memory_order_relaxed);
            p.id.store(o.id, std::memory_order_release);
        }
    }
    void render(float *l, float *r, uint32_t count) {
        drain();
        const float meterDecay = float(std::exp(-double(count) / (.045 * sr)));
        for (auto &o : objects) o.energy *= meterDecay;

        float cutoff = 100 * std::pow(150.f, clamp(macro.load(std::memory_order_relaxed), 0, 1));
        cutoff = std::min(cutoff, float(sr * .4));
        const float filterTarget = float(1 - std::exp(-2 * pi * cutoff / sr));
        const float d = clamp(density.load(std::memory_order_relaxed), 8, 64);
        const float sp = clamp(spread.load(std::memory_order_relaxed), 0, .75f);
        const float characterTarget = character.load(std::memory_order_relaxed);
        const float decayTarget = float(1 - std::exp(-1 / (sr * (.080 - .070 * characterTarget))));
        const float gainTarget = outputTarget.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < count; ++i) {
            timbre += timbreSmoothing * (characterTarget - timbre);
            pulseLoss += timbreSmoothing * (decayTarget - pulseLoss);
            grainDensity += timbreSmoothing * (d - grainDensity);
            grainSpread += timbreSmoothing * (sp - grainSpread);
            song.advanceBlend(beat,sr);songTick();
            if (beat + 1e-10 >= earliest) {
                for (auto &loop : loops) if (loop.id) {
                    if (loop.hasPending && beat + 1e-10 >= loop.switchBeat) {
                        install(loop, loop.pending, loop.switchBeat);
                        loop.hasPending = false;
                    }
                    for (int j = 0; j < loop.sequence.count; ++j)
                        if (loop.sequence.ids[j] && beat + 1e-10 >= loop.next[j]) {
                            const int slot = find(loop.sequence.ids[j]);
                            if (slot >= 0) startVoice(slot);
                            loop.next[j] += loop.sequence.length;
                        }
                }
                for (auto &e : events) if (e.active && beat + 1e-10 >= e.next) {
                    startVoice(e.slot, e.attackFrequency, e.alarm);
                    if (e.period > 0) e.next += e.period; else e.active = false;
                }
                refreshEarliest();
            }
            for (int j = 0; j < PF_MAX_OBJECTS; ++j) {
                auto &o = objects[j];
                if (o.id && o.removing) {
                    o.gain = std::max(0.f, o.gain - o.fadeStep);
                    if (o.gain <= 0) {
                        for (auto &v : voices) if (v.slot == j) v.active = false;
                        o = {};
                    }
                }
            }
            float left = 0, right = 0;
            environment.sample(left,right,song.blend,beat-song.origin);
            for (auto &v : voices) if (v.active) {
                float s = sample(v); left += s * v.left; right += s * v.right;
            }
            filterCoeff += smoothing * (filterTarget - filterCoeff);
            filterL1 += filterCoeff * (left - filterL1); filterL2 += filterCoeff * (filterL1 - filterL2);
            filterR1 += filterCoeff * (right - filterR1); filterR2 += filterCoeff * (filterR1 - filterR2);
            float dl = filterL2 - dcLIn + dcCoeff * dcLOut; dcLIn = filterL2; dcLOut = dl;
            float dr = filterR2 - dcRIn + dcCoeff * dcROut; dcRIn = filterR2; dcROut = dr;
            outputGain += clamp(gainTarget - outputGain, -outputStep, outputStep);
            l[i] = outputGain * .9f * std::tanh(dl); r[i] = outputGain * .9f * std::tanh(dr);
            beat += bpm / (60 * sr); ++frame;
        }
        // Avoid denormal tails in silence without changing audible material.
        if (std::abs(filterL1) < 1e-20f) filterL1 = 0;
        if (std::abs(filterL2) < 1e-20f) filterL2 = 0;
        if (std::abs(filterR1) < 1e-20f) filterR1 = 0;
        if (std::abs(filterR2) < 1e-20f) filterR2 = 0;
        if (std::abs(dcLOut) < 1e-20f) dcLOut = 0;
        if (std::abs(dcROut) < 1e-20f) dcROut = 0;
        publish();
    }
};

PFCore *pf_create(double sr) { return new PFCore(sr); }
void pf_destroy(PFCore *p) { delete p; }
void pf_reset(PFCore *p, double sr) { p->reset(sr); }
bool pf_define(PFCore *p, int32_t id, int m, float f, float pan) {
    if (id <= 0 || !std::isfinite(f) || !std::isfinite(pan)) return false;
    Command c; c.kind = Kind::define; c.id = id; c.material = m; c.frequency = f; c.pan = pan; return p->push(c);
}
bool pf_trigger(PFCore *p, int32_t id, int m, float f, float pan, double beat) {
    if (id <= 0 || !std::isfinite(f) || !std::isfinite(pan) || !std::isfinite(beat)) return false;
    Command c; c.kind = Kind::trigger; c.id = id; c.material = m; c.frequency = f; c.pan = pan; c.beat = beat; return p->push(c);
}
bool pf_trigger_with_cutoff(PFCore *p, int32_t id, int m, float f, float pan, double beat, float hz) {
    if (id <= 0 || !std::isfinite(f) || !std::isfinite(pan) || !std::isfinite(beat) || !std::isfinite(hz) || hz < 0) return false;
    Command c; c.kind = Kind::trigger; c.id = id; c.material = m; c.frequency = f; c.pan = pan; c.beat = beat;
    c.appliesCutoff = true; c.cutoffHz = hz; return p->push(c);
}
bool pf_trigger_now(PFCore *p, int32_t id, int m, float f, float pan, double requestedBeat, float hz) {
    return pf_trigger_alarm(p,id,m,f,pan,requestedBeat,hz,0);
}
bool pf_trigger_alarm(PFCore *p, int32_t id, int m, float f, float pan, double requestedBeat, float hz, float alarm) {
    if (!std::isfinite(alarm) || alarm<0 || alarm>1) return false;
    if (id <= 0 || !std::isfinite(f) || !std::isfinite(pan) || !std::isfinite(requestedBeat) || !std::isfinite(hz) || hz < 0) return false;
    Command c; c.kind = Kind::trigger; c.id = id; c.material = m; c.frequency = f; c.pan = pan;
    c.beat = requestedBeat; c.immediate = true; c.appliesCutoff = true; c.cutoffHz = hz; c.alarm=alarm; return p->push(c);
}
bool pf_set_object_cutoff(PFCore *p, int32_t id, float hz) {
    if (id <= 0 || !std::isfinite(hz) || hz < 0) return false;
    Command c; c.kind = Kind::cutoff; c.id = id; c.cutoffHz = hz; return p->push(c);
}
bool pf_move(PFCore *p, int32_t id, float f, float pan) {
    if (!std::isfinite(f) || !std::isfinite(pan)) return false;
    Command c; c.kind = Kind::move; c.id = id; c.frequency = f; c.pan = pan; return p->push(c);
}
bool pf_pan(PFCore *p, int32_t id, float pan) {
    if (id <= 0 || !std::isfinite(pan)) return false;
    Command c; c.kind = Kind::pan; c.id = id; c.pan = pan; return p->push(c);
}
bool pf_remove(PFCore *p, int32_t id, float seconds) {
    if (!std::isfinite(seconds)) return false;
    Command c; c.kind = Kind::remove; c.id = id; c.seconds = seconds; return p->push(c);
}
bool pf_loop(PFCore *p, const int32_t *ids, const double *offsets, int count, double anchor, double length) {
    if (!ids || !offsets || count < 1 || count > PF_MAX_LOOP_NOTES || !std::isfinite(anchor) || !std::isfinite(length) || length < .25) return false;
    Command c; c.kind = Kind::loop; c.count = count; c.beat = anchor; c.length = length;
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(offsets[i]) || offsets[i] < 0 || offsets[i] >= length) return false;
        c.ids[i] = ids[i]; c.offsets[i] = offsets[i];
    }
    return p->push(c);
}
bool pf_set_loop(PFCore *p, int32_t loopID, const int32_t *ids, const double *offsets, int count, double anchor, double length) {
    if (loopID <= 0 || !ids || !offsets || count < 1 || count > PF_MAX_LOOP_NOTES ||
        !std::isfinite(anchor) || !std::isfinite(length) || length < .25) return false;
    Command c; c.kind = Kind::setLoop; c.id = loopID; c.count = count; c.beat = anchor; c.length = length;
    for (int i = 0; i < count; ++i) {
        if (ids[i] <= 0 || !std::isfinite(offsets[i]) || offsets[i] < 0 || offsets[i] >= length ||
            (i > 0 && offsets[i] < offsets[i - 1])) return false;
        c.ids[i] = ids[i]; c.offsets[i] = offsets[i];
    }
    return p->push(c);
}
bool pf_configure(PFCore *p, double bpm, double grid) {
    if (!std::isfinite(bpm) || !std::isfinite(grid)) return false;
    Command c; c.kind = Kind::configure; c.bpm = bpm; c.grid = grid; return p->push(c);
}
void pf_set_macro(PFCore *p, float x) { if (std::isfinite(x)) p->macro.store(clamp(x, 0, 1), std::memory_order_relaxed); }
void pf_set_character(PFCore *p, float x) { if (std::isfinite(x)) p->character.store(clamp(x, 0, 1), std::memory_order_relaxed); }
void pf_set_output_gain(PFCore *p, float x) { if (std::isfinite(x)) p->outputTarget.store(clamp(x, 0, 1), std::memory_order_relaxed); }
void pf_set_texture(PFCore *p, float d, float s) {
    if (std::isfinite(d)) p->density.store(clamp(d, 8, 64), std::memory_order_relaxed);
    if (std::isfinite(s)) p->spread.store(clamp(s, 0, .75f), std::memory_order_relaxed);
}
bool pf_poll_ack(PFCore *p, PFScheduleAck *out) {
    auto r = p->ackRead.load(std::memory_order_relaxed);
    if (r == p->ackWrite.load(std::memory_order_acquire)) return false;
    *out = p->acknowledgements[r];
    p->ackRead.store((r + 1) % kQueue, std::memory_order_release); return true;
}
PFClockState pf_clock(const PFCore *p) {
    return {p->publicBeat.load(), p->publicBPM.load(), p->publicSR.load(), p->publicFrame.load(), p->rejected.load(), p->dropped.load()};
}
PFObjectState pf_object(const PFCore *p, int32_t id) {
    for (const auto &o : p->published) if (o.id.load(std::memory_order_acquire) == id) {
        PFObjectState result{id, true, o.energy.load(), o.pan.load(), o.frequency.load(), o.scheduledBeat.load(), o.lastOnsetBeat.load(), o.onsetCount.load(), o.cutoffHz.load(),o.lastOnsetSeconds.load()};
        if (o.id.load(std::memory_order_acquire) == id) return result;
    }
    return {id, false, 0, 0, 0, -1, -1, 0, 0,-1};
}
void pf_render(PFCore *p, float *l, float *r, uint32_t n) { p->render(l, r, n); }

bool pf_set_environment(PFCore *p,const PFEnvironmentSound *frame) {
    if(!p || !frame || !std::isfinite(frame->wind) || frame->wind<0 || frame->wind>1)return false;
    for(const auto &w:frame->waves) {
        if(!std::isfinite(w.frequency) || w.frequency<20 || w.frequency>4000 ||
           !std::isfinite(w.pan) || std::abs(w.pan)>1 || !std::isfinite(w.cutoffHz) || w.cutoffHz<500 || w.cutoffHz>2000 ||
           !std::isfinite(w.gain) || w.gain<0 || w.gain>1)return false;
    }
    Command c;c.kind=Kind::environment;c.environment=*frame;return p->push(c);
}

bool pf_song_start(PFCore *p,const PFSongScore *s) {
    if(!p || !s || s->count<5 || s->count>PF_SONG_NOTES || (s->scaleCount!=5 && s->scaleCount!=7) || s->root<0 || s->root>11 ||
       !std::isfinite(s->bpm) || s->bpm<60 || s->bpm>160 || !std::isfinite(s->lengthBeats) || s->lengthBeats<4 || s->lengthBeats>32 || std::fmod(s->lengthBeats,4)!=0 || s->scale[0]!=0 || s->notes[0].offset!=0 ||
       !std::isfinite(s->durationSeconds) || s->durationSeconds<8 || s->durationSeconds>180)return false;
    for(int i=0;i<s->scaleCount;++i)if(s->scale[i]<0 || s->scale[i]>11 || (i && s->scale[i]<=s->scale[i-1]))return false;
    for(int i=0;i<s->count;++i) {
        const auto &n=s->notes[i];
        if(n.id<=0 || n.midi<36 || n.midi>96)return false;
        bool inKey=false;
        for(int j=0;j<s->scaleCount;++j)inKey|=(n.midi-s->root+120)%12==s->scale[j];
        if(!inKey || !std::isfinite(n.offset) || n.offset<0 || n.offset>=s->lengthBeats ||
           std::abs(n.offset*2-std::round(n.offset*2))>1e-6 || (i && n.offset<=s->notes[i-1].offset))return false;
    }
    Command c;c.kind=Kind::songStart;c.song=*s;c.songEpoch=p->songEpoch.load();return p->push(c);
}
bool pf_song_stop(PFCore *p) { if(!p)return false;p->songEpoch.fetch_add(1,std::memory_order_release);return true; }
PFSongState pf_song_state(const PFCore *p) {
    return {p->songActive.load(),p->songBlend.load(),p->publicBPM.load(),p->songRemaining.load(),p->songChord.load()};
}

void pf_song_hold(PFCore *p,int32_t id,bool held) {
    if(!p || id<=0)return;
    for(int i=0;i<PF_MAX_OBJECTS;++i) {
        if(!held && p->songHeld[i].load()==id)p->songHeld[i].store(0);
        else if(held && p->published[i].id.load()==id)p->songHeld[i].store(id);
    }
}
