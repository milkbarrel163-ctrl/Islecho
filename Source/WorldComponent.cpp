#include "WorldComponent.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <algorithm>
#include <limits>

// Preferences panel (opened from the macOS menu bar).

static juce::Font makeUiFont(float height, int style = juce::Font::plain) {
#if JUCE_MAJOR_VERSION >= 8
    return juce::Font(juce::FontOptions(height, style));
#else
    return juce::Font(height, style);
#endif
}

class PreferencesComponent final : public juce::Component {
    WorldDocument& world;
    std::function<void()> onSave;
    std::function<void()> onStorm;
    juce::ComboBox scaleBox, rootBox, weatherBox;
    juce::Slider daySlider, windSlider;
    juce::ToggleButton runningToggle{"Running"}, autoWeatherToggle{"Automatic weather"};
    juce::TextButton stormBtn{"Trigger Storm"};
public:
    PreferencesComponent(WorldDocument& w, std::function<void()> save, std::function<void()> triggerStorm)
        : world(w), onSave(std::move(save)), onStorm(std::move(triggerStorm))
    {
        scaleBox.addItem("Pentatonic", 1);
        scaleBox.addItem("Minor", 2);
        scaleBox.addItem("Minor pentatonic", 3);
        scaleBox.addItem("Major", 4);
        scaleBox.setSelectedId(static_cast<int>(world.settings.scale)+1, juce::dontSendNotification);
        scaleBox.onChange = [this] {
            world.settings.scale = static_cast<MusicalScaleType>(scaleBox.getSelectedId()-1);
            onSave();
        };
        addAndMakeVisible(scaleBox);

        static const char* rootNames[] = {"C","C#","D","Eb","E","F","F#","G","Ab","A","Bb","B"};
        for (int i = 0; i < 12; ++i) rootBox.addItem(rootNames[i], i+1);
        rootBox.setSelectedId(world.settings.root+1, juce::dontSendNotification);
        rootBox.onChange = [this] { world.settings.root = rootBox.getSelectedId()-1; onSave(); };
        addAndMakeVisible(rootBox);

        for (auto* s : { &daySlider, &windSlider }) {
            s->setSliderStyle(juce::Slider::LinearHorizontal);
            s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 58, 24);
        }
        daySlider.setRange(30.0, 600.0, 1.0);
        daySlider.setTextValueSuffix(" s");
        daySlider.setValue(world.environment.dayDuration, juce::dontSendNotification);
        daySlider.onValueChange = [this] { world.environment.dayDuration = daySlider.getValue(); };
        daySlider.onDragEnd = [this] { onSave(); };
        addAndMakeVisible(daySlider);

        windSlider.setRange(0.0, 1.0, 0.01);
        windSlider.setValue(world.environment.wind, juce::dontSendNotification);
        windSlider.onValueChange = [this] { world.environment.wind = windSlider.getValue(); };
        windSlider.onDragEnd = [this] { onSave(); };
        addAndMakeVisible(windSlider);

        weatherBox.addItem("Clear", 1);
        weatherBox.addItem("Overcast", 2);
        weatherBox.addItem("Windy", 3);
        weatherBox.addItem("Rain", 4);
        weatherBox.setSelectedId(static_cast<int>(world.environment.weather)+1, juce::dontSendNotification);
        weatherBox.onChange = [this] {
            world.environment.weather = static_cast<EcologyWeather>(weatherBox.getSelectedId()-1);
            world.environment.automaticWeather = false;
            world.environment.weatherCycle.reset();
            autoWeatherToggle.setToggleState(false, juce::dontSendNotification);
            onSave();
        };
        addAndMakeVisible(weatherBox);

        autoWeatherToggle.setToggleState(world.environment.automaticWeather, juce::dontSendNotification);
        autoWeatherToggle.onClick = [this] {
            world.environment.automaticWeather = autoWeatherToggle.getToggleState();
            onSave();
        };
        addAndMakeVisible(autoWeatherToggle);

        runningToggle.setToggleState(world.environment.isRunning, juce::dontSendNotification);
        runningToggle.onClick = [this] {
            world.environment.isRunning = runningToggle.getToggleState();
            onSave();
        };
        addAndMakeVisible(runningToggle);

        stormBtn.onClick = [this] { if (onStorm) onStorm(); };
        addAndMakeVisible(stormBtn);

        setSize(480, 264);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff101c19));
        g.setColour(juce::Colour(0xffd0dbb0));
        g.setFont(makeUiFont(13.f));
        const char* labels[] = {"Scale", "Root", "Day length", "Wind", "Weather"};
        for (int i = 0; i < 5; ++i)
            g.drawText(labels[i], 16, 8+i*36, 116, 24, juce::Justification::centredLeft);
    }

    void resized() override {
        const int lx = 140, w = 316;
        scaleBox.setBounds(lx, 8, w, 25);
        rootBox.setBounds(lx, 44, w, 25);
        daySlider.setBounds(lx, 80, w, 25);
        windSlider.setBounds(lx, 116, w, 25);
        weatherBox.setBounds(lx, 152, w, 25);
        autoWeatherToggle.setBounds(16, 188, 220, 26);
        runningToggle.setBounds(240, 188, 220, 26);
        stormBtn.setBounds(16, 224, 444, 28);
    }
};

// Constants

static constexpr int MAX_OBJECTS = 64;
static constexpr uint32_t CAPTURE_FRAMES = 96000 * 600; // 10 min at up to 96 kHz

// Constructor / Destructor

WorldComponent::WorldComponent() {
    core    = pf_create(48000.0);
    capture = pfr_create(CAPTURE_FRAMES);
    setOpaque(false);  // JUCE controls are composited above the OpenGL island.
    setWantsKeyboardFocus(true);
    setSize(1200, 800);
    // JUCE 7's CoreAudio initialisation aborts after an RPC timeout in the
    // available iOS 18.3 simulator. Device builds keep the normal audio path.
#if !defined(TARGET_OS_SIMULATOR) || !TARGET_OS_SIMULATOR
    setAudioChannels(0, 2);
#endif

    island.resized(getWidth(),getHeight());
    island.attach(*this);

    loadWorld();
    if (world.objects.empty()) {
        world.environment.isRunning = true;
        world.environment.dayPhase = 0.55;
        world.ecologicalPlacement = EcologyKind::fish;
    }
    startTimerHz(30);
}

WorldComponent::~WorldComponent() {
    island.detach();
    stopTimer();
    stopRecording();
    shutdownAudio();
    pfr_destroy(capture);
    pf_destroy(core);
}

// Audio callbacks

void WorldComponent::prepareToPlay(int, double sampleRate) {
    const juce::ScopedLock lock(worldLock);
    if (recording) {
        pfr_stop(capture);
        juce::Component::SafePointer<WorldComponent> safe(this);
        juce::MessageManager::callAsync([safe] { if (safe) safe->checkpointForInterruption(); });
    }
    currentSampleRate = sampleRate;
    pf_reset(core, sampleRate);
    envAudio.reset();
    needsRestore.store(true, std::memory_order_release);
}

void WorldComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
    auto* buf = info.buffer;
    if (buf->getNumChannels() < 2) { buf->clear(info.startSample, info.numSamples); return; }
    float* L = buf->getWritePointer(0, info.startSample);
    float* R = buf->getWritePointer(1, info.startSample);
    pf_render(core, L, R, static_cast<uint32_t>(info.numSamples));
    if (recording) pfr_capture(capture, L, R, static_cast<uint32_t>(info.numSamples));
    for (int ch = 2; ch < buf->getNumChannels(); ++ch)
        buf->clear(ch, info.startSample, info.numSamples);
}

void WorldComponent::releaseResources() {
    if (recording) pfr_stop(capture);
    juce::Component::SafePointer<WorldComponent> safe(this);
    juce::MessageManager::callAsync([safe] { if (safe) safe->checkpointForInterruption(); });
}

void WorldComponent::checkpointForInterruption() {
    stopRecording();
    stopSong();
    saveWorld();
}

void WorldComponent::resumeAfterInterruption() {
    ecology.resetTime();
}

// DSP helpers

void WorldComponent::restoreVoices() {
    const juce::ScopedLock lock(worldLock);
    clearBirthWork();
    pf_configure(core, world.settings.bpm, world.settings.grid);
    pf_set_macro(core, world.settings.macro);
    pf_set_texture(core, world.settings.density, world.settings.spread);
    pf_set_character(core, world.settings.character);
    for (auto& o : world.objects) {
        if (o.departure || !o.ecology) continue;
        (void)pf_define(core, o.id, static_cast<int>(o.material),
                        world.settings.bpm > 0 ? MusicalScale::midiToHz(60 + world.settings.root) : 220.f,
                        panFor(o.x));
    }
}

float WorldComponent::panFor(double x) const {
    return juce::jlimit(-1.0f, 1.0f, static_cast<float>(2.0 * x - 1.0));
}

float WorldComponent::nextNoteHz(double alarm) {
    MusicalScale::Type t = MusicalScale::pentatonic;
    switch (world.settings.scale) {
    case MusicalScaleType::minor:          t = MusicalScale::minor; break;
    case MusicalScaleType::minorPentatonic: t = MusicalScale::minorPentatonic; break;
    case MusicalScaleType::major:          t = MusicalScale::major; break;
    default: break;
    }
    auto notes = MusicalScale::notes(t, world.settings.root);
    if (alarm > 0.08) {
        std::vector<int> high;
        for (int n : notes) if (n >= 72 + world.settings.root) high.push_back(n);
        if (!high.empty()) {
            std::uniform_int_distribution<int> dist(0, (int)high.size()-1);
            return MusicalScale::midiToHz(high[dist(pitchRng)]);
        }
    }
    std::uniform_int_distribution<int> dist(0, (int)notes.size()-1);
    return MusicalScale::midiToHz(notes[dist(pitchRng)]);
}

float WorldComponent::cutoffForKind(EcologyKind kind) const {
    const double sl = std::clamp(world.environment.sunlight(), 0.0, 1.0);
    switch (kind) {
    case EcologyKind::plant:   return static_cast<float>(320.0 * std::pow(6500.0 / 320.0, sl));
    case EcologyKind::fish:
    case EcologyKind::firefly: return 0.f;
    }
    return 0.f;
}

// Timer / ecology tick

void WorldComponent::timerCallback() {
    if (needsRestore.exchange(false, std::memory_order_acq_rel)) {
        restoreVoices();
    }

    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    world.environment.storm = std::clamp((stormUntil - now) / 2.0, 0.0, 1.0);
    if (now > noticeUntil) notice.clear();
    if (recording && pfr_reason(capture) != 0) stopRecording();

    // Poll schedule acks and update bornBeat and the ecology bridge.
    PFScheduleAck ack;
    while (pf_poll_ack(core, &ack)) {
        ecologyBridge.acknowledge(ack);
        for (auto& o : world.objects)
            if (o.id == ack.id && std::abs(o.bornBeat - ack.requestedBeat) < 1e-6) {
                o.bornBeat = ack.scheduledBeat; break;
            }
    }

    // Song status
    const auto song = pf_song_state(core);
    if (song.active)
        songStatus = juce::String::formatted("Song  %.0f BPM  %.1f beats left",
                                             song.bpm, song.remainingBeats);
    else
        songStatus.clear();

    // Update environment audio (wave voices + wind)
    envAudio.update(world.environment, world.settings, core, now, world.environment.isRunning);
    island.setDaylight(static_cast<float>(world.environment.sunlight()));
    island.setWater(static_cast<float>(world.environment.elapsed),
                    static_cast<float>(world.environment.effectiveWind()));

    ecologyTick(now);
    commitDeaths(now);
    advanceBirths();

    // Sync audio telemetry
    for (auto& o : world.objects) {
        if (!o.ecology) continue;
        const auto state = pf_object(core, o.id);
        o.energy = state.energy;
        if (state.alive) o.soundingFrequency = state.frequency;
        o.lastOnset = state.lastOnsetBeat;
        o.lastOnsetSeconds = state.lastOnsetSeconds;
    }

    // Remove departed objects
    world.objects.erase(std::remove_if(world.objects.begin(), world.objects.end(),
        [&](const SoundObject& o) {
            if (!o.departure) return false;
            return (now - o.departure->started > o.departure->duration)
                   && !pf_object(core, o.id).alive;
        }), world.objects.end());

    // Pan updates
    for (auto& o : world.objects) {
        if (o.departure) continue;
        pf_pan(core, o.id, panFor(o.x));
    }

    if (now - lastAutoSaveTime >= 5.0) {
        saveWorld();
        lastAutoSaveTime = now;
    }

    repaint();
}

void WorldComponent::clearBirthWork() {
    pendingLife.clear();
    birthRetryAfter.clear();
}

std::optional<std::pair<size_t, size_t>> WorldComponent::parentIndices(const EcologyBirth& birth) const {
    auto find = [&](int32_t id) -> std::optional<size_t> {
        for (size_t i = 0; i < world.objects.size(); ++i) {
            const auto& o = world.objects[i];
            if (o.id != id) continue;
            if (o.departure || !o.ecology || o.ecology->kind != EcologyKind::fish || !o.ecology->life) return std::nullopt;
            const auto& life = *o.ecology->life;
            if (life.death != LifeDeath::none || life.breedCooldown > 0 || heldIDs.count(id)) return std::nullopt;
            return i;
        }
        return std::nullopt;
    };
    const auto a = find(birth.parentA), b = find(birth.parentB);
    if (!a || !b) return std::nullopt;
    return std::make_pair(*a, *b);
}

void WorldComponent::ecologyTick(double time) {
    if (!world.environment.isRunning) { ecology.resetTime(); clearBirthWork(); return; }
    const juce::ScopedLock lock(worldLock);
    auto intents = ecology.step(world.environment, world.objects, heldIDs, time);

    // Add emergences to pending life.
    for (auto& emg : ecology.emergences) {
        if ((int)pendingLife.size() >= 4) break;
        if ((int)world.objects.size() + (int)pendingLife.size() >= MAX_OBJECTS) break;
        EcologyBody body(emg.kind, emg.seed);
        const int32_t newID = world.nextID++;
        SoundObject o(newID, materialForKind(emg.kind), emg.x, emg.y);
        o.bornBeat = -1; o.touchTime = -1; o.ecology = body;
        // This port has no physical growth/motion integrator yet. A zero-height
        // motion placeholder would bury new meshes inside the terrain.
        pendingLife.push_back({ std::move(o), std::nullopt });
    }

    // Add births to pending life (offspring from reproduction).
    const double ecoNow = world.environment.elapsed;
    for (auto it = birthRetryAfter.begin(); it != birthRetryAfter.end();)
        it = it->second <= ecoNow ? birthRetryAfter.erase(it) : std::next(it);
    auto retrying = [&](int32_t id) {
        auto it = birthRetryAfter.find(id);
        return it != birthRetryAfter.end() && it->second > ecoNow;
    };
    auto queued = [&](int32_t id) {
        return std::any_of(pendingLife.begin(), pendingLife.end(), [&](const PendingLife& p) {
            return p.parents && (p.parents->parentA == id || p.parents->parentB == id);
        });
    };
    for (auto& birth : ecology.births) {
        if ((int)pendingLife.size() >= 4) break;
        if ((int)world.objects.size() + (int)pendingLife.size() >= MAX_OBJECTS) break;
        if (retrying(birth.parentA) || retrying(birth.parentB)) continue;
        if (queued(birth.parentA) || queued(birth.parentB)) continue;
        if (!parentIndices(birth)) continue;
        EcologyBody body(EcologyKind::fish, birth.seed);
        body.life = EcologyLife::create(EcologyKind::fish, birth.seed,
                                         /*juvenile=*/true,
                                         static_cast<int>(birth.species),
                                         birth.generation);
        const int32_t newID = world.nextID++;
        SoundObject o(newID, materialForKind(EcologyKind::fish), birth.x, birth.y);
        o.bornBeat = -1; o.touchTime = -1; o.ecology = body;
        pendingLife.push_back({ std::move(o), birth });
    }

    // Stage B terrain grounding: clamp terrestrial organism heights above the island surface.
    for (auto& o : world.objects) {
        if (!o.ecology || o.departure) continue;
        if (o.ecology->kind == EcologyKind::fish) continue;
        if (!o.ecology->motion) o.ecology->motion = EcologyMotion{};
        const float terrainH = IslandScene::heightAt(float(o.x), float(o.y));
        if (o.ecology->motion->height < double(terrainH))
            o.ecology->motion->height = double(terrainH);
    }

    // Schedule ecology intents through bridge (deduplicates, uses pf_trigger_alarm)
    const auto songState = pf_song_state(core);
    if (!songState.active)
        ecologyBridge.schedule(intents, world.objects, world.environment,
                               core, world.settings, pitchRng);
}

void WorldComponent::stopSong() {
    rhythm.reset();
    pf_song_stop(core);
    songStatus.clear();
}

void WorldComponent::commitDeaths(double time) {
    bool changed = false;
    for (auto& o : world.objects) {
        if (o.departure) continue;
        if (!o.ecology || !o.ecology->life) continue;
        if (o.ecology->life->death == LifeDeath::none) continue;
        const auto& cause = o.ecology->life->death;
        const double duration = (cause == LifeDeath::eaten) ? 0.25 : 0.8;
        if (!pf_remove(core, o.id, static_cast<float>(duration))) continue;
        o.departure = Departure{
            o.x, o.y,
            o.ecology->life->windX, o.ecology->life->windY,
            time, duration
        };
        changed = true;
    }
    if (changed) saveWorld();
}

void WorldComponent::advanceBirths() {
    if (!world.environment.isRunning || pendingLife.empty() || (int)world.objects.size() >= MAX_OBJECTS) return;
    auto job = std::move(pendingLife.front());
    pendingLife.erase(pendingLife.begin());
    if (job.parents && !parentIndices(*job.parents)) return;

    // Simple placement: try the stored position; if not valid, search nearby
    double bx = job.object.x, by = job.object.y;
    const EcologyKind kind = job.object.ecology ? job.object.ecology->kind : EcologyKind::fish;
    if (!habitatAccepts(kind, bx, by)) {
        // Spiral search
        bool placed = false;
        for (int r = 1; r <= 20 && !placed; ++r) {
            for (int dx = -r; dx <= r && !placed; ++dx) {
                for (int dy = -r; dy <= r && !placed; ++dy) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    double cx = bx + dx * 0.03, cy = by + dy * 0.03;
                    if (habitatAccepts(kind, cx, cy)) { bx = cx; by = cy; placed = true; }
                }
            }
        }
        if (!placed) {
            if (job.parents) {
                birthRetryAfter[job.parents->parentA] = world.environment.elapsed + 1.0;
                birthRetryAfter[job.parents->parentB] = world.environment.elapsed + 1.0;
            }
            return;
        }
    }
    job.object.x = bx; job.object.y = by;

    const double beat = pf_clock(core).beat;
    if (job.parents) {
        const auto parents = parentIndices(*job.parents);
        if (!parents) return;
        const float freq = nextNoteHz();
        const float cutoff = cutoffForKind(kind);
        if (!pf_trigger_now(core, job.object.id, static_cast<int>(job.object.material),
                            freq, panFor(bx), beat, cutoff)) return;
        job.object.bornBeat = beat;
        for (size_t i : { parents->first, parents->second }) {
            auto& life = *world.objects[i].ecology->life;
            life.breedCooldown = 75.0;
            (void)life.unit();
        }
    } else {
        const float restFreq = MusicalScale::midiToHz(60 + world.settings.root);
        if (!pf_define(core, job.object.id, static_cast<int>(job.object.material),
                       restFreq, panFor(bx)))
            return;
    }
    world.objects.push_back(std::move(job.object));
    saveWorld();
}

// Interaction

SoundObject* WorldComponent::hitTest(juce::Point<float> screen) {
    const auto wb = worldBounds();
    const float hitRadius = std::max(24.f, wb.getWidth() * 0.025f);
    SoundObject* result = nullptr;
    float nearest=hitRadius;
    for (auto& o : world.objects) {
        if (o.departure || !o.ecology) continue;
        if (o.ecology->life && o.ecology->life->death != LifeDeath::none) continue;
        const auto foot=screenPoint(o.x,o.y);
        float lift=0;
        if(o.ecology->kind==EcologyKind::plant)
            lift=.87f*1.70f*OrganismMesh::appearance(o.ecology->appearanceSeed).proportions.y;
        else if(o.ecology->kind==EcologyKind::firefly)lift=.95f;
        else lift=.12f;
        const auto handle=island.projectLift(o.x,o.y,lift);
        const auto segment=handle-foot;
        const float squared=segment.x*segment.x+segment.y*segment.y;
        const float t=squared>1.e-6f?juce::jlimit(0.f,1.f,((screen.x-foot.x)*segment.x+(screen.y-foot.y)*segment.y)/squared):0.f;
        const float distance=screen.getDistanceFrom(foot+segment*t);
        if(distance<nearest){nearest=distance;result=&o;}
    }
    return result;
}

void WorldComponent::mouseDown(const juce::MouseEvent& event) {
    const juce::uint64 key = static_cast<juce::uint64>(event.source.getIndex());
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    grabKeyboardFocus();

    // Multitouch: mark existing contacts non-musical (rhythm tap only valid for clean single tap)
    if (!contacts.empty())
        for (auto& [k, c] : contacts)
            c.musicalTap = false;

    auto* hit = hitTest(event.position);
    if (!hit) {
        emptyContacts[key] = event.position;
        return;
    }

    highlightedID=hit->id;
    highlightUntil=now+.28;
    const float freq = nextNoteHz();
    const float cutoff = cutoffForKind(hit->ecology->kind);
    const auto beat = pf_clock(core).beat;
    const bool triggered=pf_trigger_now(core, hit->id, static_cast<int>(hit->material), freq,
                        panFor(hit->x), beat, cutoff);
    if (!triggered) {
        notice = "Voice busy - please wait"; noticeUntil = now + 2.4;
    } else {
        hit->bornBeat = beat;
        pf_song_hold(core, hit->id, true);
        heldIDs.insert(hit->id);
    }
    // musicalTap=true only if this is the only contact (no prior contacts)
    const bool solo = contacts.empty();
    contacts[key] = { hit->id, event.position, event.position,
                      screenPoint(hit->x,hit->y)-event.position,
                      freq, now, false, solo && triggered, triggered, now };
}

void WorldComponent::mouseDrag(const juce::MouseEvent& event) {
    const juce::uint64 key = static_cast<juce::uint64>(event.source.getIndex());
    auto it = contacts.find(key);
    if (it == contacts.end()) {
        auto empty = emptyContacts.find(key);
        if (empty == emptyContacts.end()) return;
        const auto previous=empty->second;
        if (!contacts.empty()) { empty->second=event.position; return; }
        if (emptyContacts.size() == 1) island.orbit(previous,event.position);
        else if (emptyContacts.size() == 2) {
            auto other=emptyContacts.begin();
            if (other->first==key) ++other;
            island.navigate(previous,other->second,event.position,other->second);
        }
        empty->second=event.position;
        return;
    }
    auto& contact = it->second;
    if (event.position.getDistanceFrom(contact.origin) > 10) contact.moved = true;
    if (contact.moved) {
        const auto wp = worldPoint(event.position+contact.grabOffset);
        const auto previousWorld = worldPoint(contact.previous+contact.grabOffset);
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        const double dt = now - contact.lastDragAt;
        if (dt > 0.001 && dt < 0.2) {
            contact.dragVX = std::clamp((wp.x - previousWorld.x) / dt, -0.12, 0.12);
            contact.dragVY = std::clamp((wp.y - previousWorld.y) / dt, -0.12, 0.12);
        }
        contact.lastDragAt = now;
        auto objIt = std::find_if(world.objects.begin(), world.objects.end(),
                                  [&](const SoundObject& o) { return o.id == contact.id; });
        if (objIt != world.objects.end() && !objIt->departure) {
            auto* obj = &(*objIt);
            double nx = wp.x, ny = wp.y;
            boundedPosition(nx, ny);
            obj->x = nx; obj->y = ny;
            pf_pan(core, obj->id, panFor(nx));
        }
    }
    contact.previous = event.position;
}

void WorldComponent::mouseUp(const juce::MouseEvent& event) {
    const juce::uint64 key = static_cast<juce::uint64>(event.source.getIndex());
    emptyContacts.erase(key);
    auto it = contacts.find(key);
    if (it == contacts.end()) return;
    const Contact c = it->second;
    if(c.voiceHeld){pf_song_hold(core, c.id, false);heldIDs.erase(c.id);}
    contacts.erase(it);
    if (c.moved) {
        auto obj = std::find_if(world.objects.begin(), world.objects.end(),
                                [&](const SoundObject& o) { return o.id == c.id; });
        if (obj != world.objects.end() && obj->ecology) {
            if (obj->ecology->swimming) {
                obj->ecology->swimming->velocityX = c.dragVX;
                obj->ecology->swimming->velocityY = c.dragVY;
            }
            if (obj->ecology->life) {
                if (obj->ecology->kind == EcologyKind::firefly) {
                    obj->ecology->life->flightVX = c.dragVX;
                    obj->ecology->life->flightVY = c.dragVY;
                } else if (obj->ecology->kind == EcologyKind::plant) {
                    obj->ecology->life->windX = c.dragVX;
                    obj->ecology->life->windY = c.dragVY;
                }
            }
        }
    }

    // Rhythm tap: short, unmoved, sole musical contact
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if(highlightedID==c.id)highlightUntil=now+.28;
    const double duration = now - c.beganAt;
    const bool isTap = c.musicalTap && !c.moved && duration < 0.25;
    if (isTap) {
        auto score = rhythm.record(c.id, c.frequency, c.beganAt,
                                   world.settings.scale, world.settings.root);
        if (score.has_value()) {
            if (pf_song_start(core, &score.value())) {
                const int bpm = static_cast<int>(std::round(score->bpm));
                notice = juce::String::formatted("Song  %d BPM", bpm);
                noticeUntil = now + 3.0;
            } else {
                stopSong();
            }
        }
    }

    saveWorld();
}

void WorldComponent::mouseWheelMove(const juce::MouseEvent& event,
                                    const juce::MouseWheelDetails& wheel) {
    if(!std::isfinite(wheel.deltaX)||!std::isfinite(wheel.deltaY))return;
    // JUCE delivers two-finger trackpad scrolling here, separately from touch
    // contacts. Match the iOS orbit/pitch sensitivity in logical pixels.
    const auto to=event.position+juce::Point<float>(wheel.deltaX*300.f,-wheel.deltaY*300.f);
    island.orbit(event.position,to);
}

void WorldComponent::mouseMagnify(const juce::MouseEvent& event,float scaleFactor) {
    island.zoomAt(event.position,scaleFactor);
}

bool WorldComponent::keyPressed(const juce::KeyPress& key) {
    if (key.getTextCharacter() == 's' || key.getTextCharacter() == 'S') {
        stormUntil = juce::Time::getMillisecondCounterHiRes() / 1000.0 + 2.0;
        return true;
    }
    return false;
}

// Rendering

juce::Rectangle<float> WorldComponent::worldBounds() const {
    return getLocalBounds().toFloat().reduced(20.f).withTrimmedTop(44.f).withTrimmedBottom(48.f);
}

juce::Point<float> WorldComponent::screenPoint(double wx, double wy) const {
    return island.project(wx,wy);
}

juce::Point<double> WorldComponent::worldPoint(juce::Point<float> screen) const {
    return island.unproject(screen);
}

void WorldComponent::paint(juce::Graphics& g) {
    const juce::ScopedLock lock(worldLock);
    paintBackground(g);
    paintOrganisms(g);
    paintControls(g);
}

void WorldComponent::paintBackground(juce::Graphics& g) {
    juce::ignoreUnused(g); // The opaque terrain, sea, and dark backdrop are 3D.
}

void WorldComponent::paintOrganisms(juce::Graphics& g) {
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    std::vector<IslandScene::Organism> visible;
    visible.reserve(world.objects.size());
    for (auto& o : world.objects) {
        if (!o.ecology) continue;
        double ox = o.x, oy = o.y;
        float alpha = 1.f;
        if (o.departure) {
            o.departure->positionAt(now, ox, oy);
            alpha = static_cast<float>(o.departure->opacityAt(now));
        }
        if (alpha <= 0.01f) continue;

        IslandScene::Organism item;
        item.kind=int(o.ecology->kind);item.u=ox;item.v=oy;
        item.seed=o.ecology->appearanceSeed;item.energy=o.energy;item.opacity=alpha;
        const bool selected=highlightedID==o.id &&
            (now<highlightUntil || std::any_of(contacts.begin(),contacts.end(),
                [&](const auto& pair){return pair.second.id==o.id;}));
        item.highlight=selected?1.f:0.f;
        if(const auto& life=o.ecology->life){
            item.species=life->species;item.size=life->size;item.bend=float(life->bend);
        }
        if(const auto& swim=o.ecology->swimming)item.heading=float(swim->heading);
        if(item.kind==2)item.heading=float(world.environment.elapsed*.35);
        if(const auto& motion=o.ecology->motion){
            item.hasMotion=std::isfinite(motion->height) && std::abs(motion->height)>1.e-4;
            item.motionHeight=float(motion->height);
            item.motionGrowth=motion->natural && !item.hasMotion?1.f:float(motion->growth);
            item.qx=float(motion->qx);item.qy=float(motion->qy);
            item.qz=float(motion->qz);item.qw=float(motion->qw);
        }
        visible.push_back(item);
        if(selected){
            const auto foot=screenPoint(ox,oy);
            const auto tint=item.kind==0?juce::Colour(0xff3fe8db)
                           :item.kind==1?juce::Colour(0xffc9ed66):juce::Colour(0xffc199f0);
            g.setColour(tint.withAlpha(.78f*alpha));
            g.drawEllipse(foot.x-17,foot.y-8,34,16,1.5f);
        }
    }
    island.setOrganisms(std::move(visible));
}

void WorldComponent::paintControls(juce::Graphics& g) {
    // Title bar
    g.setColour(juce::Colour(0xffd0dbb0));
    g.setFont(makeUiFont(22.f));
    g.drawText("Islecho", 20, 8, getWidth() - 40, 32, juce::Justification::centredLeft);

    // Weather / day indicator
    const double phase = world.environment.dayPhase;
    const char* timeRaw = phase < 0.25 ? "\xe5\xa4\x9c"   // Night
                        : phase < 0.5  ? "\xe6\x99\xa8"   // Morning
                        : phase < 0.75 ? "\xe6\x98\xbc"   // Day
                        :                "\xe6\x9a\xae";  // Dusk
    const char* weatherRaw = "";
    switch (world.environment.weather) {
    case EcologyWeather::rain:     weatherRaw = "  \xe9\x9b\xa8"; break;   // Rain
    case EcologyWeather::windy:    weatherRaw = "  \xe9\xa3\x8e"; break;   // Wind
    case EcologyWeather::overcast: weatherRaw = "  \xe9\x98\xb4"; break;   // Overcast
    default: break;
    }
    g.setFont(makeUiFont(14.f));
    g.setColour(juce::Colour(0xff8aab8a));
    auto timeAndWeather = juce::String::fromUTF8(timeRaw);
    timeAndWeather += juce::String::fromUTF8(weatherRaw);
    g.drawText(timeAndWeather,
               20, 8, getWidth() - 40, 32, juce::Justification::centredRight);

    // Song / notice status
    juce::String statusLine = songStatus.isEmpty() ? notice : songStatus;
    if (!statusLine.isEmpty()) {
        g.setColour(juce::Colour(0xffeecc88).withAlpha(0.9f));
        g.setFont(makeUiFont(15.f));
        g.drawText(statusLine, 0, getHeight() - 44, getWidth(), 28, juce::Justification::centred);
    }

    // Organism count
    int nOrgs = 0;
    for (auto& o : world.objects)
        if (!o.departure && o.ecology &&
            o.ecology->life && o.ecology->life->death == LifeDeath::none) ++nOrgs;
    g.setColour(juce::Colour(0xff667766));
    g.setFont(makeUiFont(12.f));
    g.drawText(juce::String(nOrgs) + " organisms", 20, getHeight() - 42, 120, 28, juce::Justification::centredLeft);
}

void WorldComponent::resized() {
    island.resized(getWidth(), getHeight());
    repaint();
}

void WorldComponent::showPreferences() {
    if (prefsWindow && prefsWindow->isVisible()) {
        prefsWindow->toFront(true);
        return;
    }
    // Inline subclass so closeButtonPressed hides rather than asserts.
    class PrefsWindow : public juce::DocumentWindow {
    public:
        PrefsWindow() : DocumentWindow("Preferences",
                                       juce::Colour(0xff101c19),
                                       juce::DocumentWindow::closeButton) {}
        void closeButtonPressed() override { setVisible(false); }
    };
    auto* win  = new PrefsWindow();
    auto* comp = new PreferencesComponent(world, [this] { saveWorld(); }, [this] {
        stormUntil = juce::Time::getMillisecondCounterHiRes() / 1000.0 + 2.0;
    });
    win->setContentOwned(comp, true);
    win->setUsingNativeTitleBar(true);
    win->setResizable(false, false);
    win->centreAroundComponent(this, comp->getWidth(), comp->getHeight());
    win->setVisible(true);
    prefsWindow.reset(win);
}

juce::File WorldComponent::recordingsDirectory() {
    return juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
        .getChildFile("Islecho Recordings");
}


// Persistence

void WorldComponent::loadWorld() {
    const auto file = Persistence::defaultSaveFile();
    if (!file.existsAsFile()) return;
    juce::String err;
    WorldDocument loaded;
    if (Persistence::load(file, loaded, err)) {
        const juce::ScopedLock lock(worldLock);
        world = std::move(loaded);
    } else {
        saveAllowed = false;
        notice = "World could not be loaded: " + err;
        noticeUntil = std::numeric_limits<double>::infinity();
    }
}

void WorldComponent::saveWorld() {
    if (!saveAllowed) return; // Preserve an unreadable or unsupported save for recovery.
    const auto file = Persistence::defaultSaveFile();
    file.getParentDirectory().createDirectory();
    juce::String err;
    if (!Persistence::save(file, world, err)) {
        notice = "Save failed: " + err;
        noticeUntil = juce::Time::getMillisecondCounterHiRes() / 1000.0 + 3.0;
    }
}

// Recording

void WorldComponent::startRecording() {
    if (recording) return;
    recordingSampleRate = currentSampleRate;
    pfr_begin(capture, static_cast<uint64_t>(currentSampleRate) * 600);
    recording = true;

    const auto dir = recordingsDirectory();
    dir.createDirectory();
    recordingFile = dir.getChildFile("islecho_" +
        juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".wav");
    notice = "Recording...";
    noticeUntil = juce::Time::getMillisecondCounterHiRes() / 1000.0 + 2.0;
}

void WorldComponent::stopRecording() {
    if (!recording) return;
    pfr_stop(capture);
    recording = false;
    while (!pfr_finished(capture)) juce::Thread::yield();
    finalizeRecording();
}

void WorldComponent::toggleRecording() {
    if (recording) stopRecording();
    else startRecording();
}

void WorldComponent::finalizeRecording() {
    const int stopReason = pfr_reason(capture);
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream{recordingFile.createOutputStream()};
    if (stream) {
#if JUCE_MAJOR_VERSION >= 8
        auto writer = fmt.createWriterFor(stream, juce::AudioFormatWriterOptions{}
            .withSampleRate(recordingSampleRate).withNumChannels(2).withBitsPerSample(24));
#else
        auto writer = std::unique_ptr<juce::AudioFormatWriter>(
            fmt.createWriterFor(stream.get(), recordingSampleRate, 2, 24, {}, 0));
        if (writer) stream.release(); // JUCE 7 transfers ownership on success.
#endif
        if (writer) {
            static constexpr int blockSize = 4096;
            std::vector<float> interleaved(blockSize * 2);
            juce::AudioBuffer<float> buf(2, blockSize);
            while (!pfr_finished(capture)) {
                uint32_t got = pfr_read(capture, interleaved.data(), blockSize);
                if (got == 0) break;
                for (uint32_t i = 0; i < got; ++i) {
                    buf.setSample(0, (int)i, interleaved[2*i]);
                    buf.setSample(1, (int)i, interleaved[2*i+1]);
                }
                writer->writeFromAudioSampleBuffer(buf, 0, (int)got);
            }
            notice = stopReason == 2 ? "Recording full; saved prefix: " + recordingFile.getFileName()
                     : "Saved: " + recordingFile.getFileName();
            noticeUntil = juce::Time::getMillisecondCounterHiRes() / 1000.0 + 4.0;
        }
    }
}
