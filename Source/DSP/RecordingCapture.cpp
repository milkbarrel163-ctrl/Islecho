#include "RecordingCapture.h"
#include <algorithm>
#include <atomic>
#include <memory>

static_assert(std::atomic<uint64_t>::is_always_lock_free, "Capture counters must be lock-free");
static_assert(std::atomic<bool>::is_always_lock_free, "Capture flags must be lock-free");
struct PFRecordingCapture {
    const uint32_t capacity;
    std::unique_ptr<float[]> samples;
    alignas(64) std::atomic<uint64_t> written{0};
    alignas(64) std::atomic<uint64_t> consumed{0};
    std::atomic<bool> enabled{false}, inFlight{false};
    std::atomic<int> reason{0};
    uint64_t limit = 0;
    explicit PFRecordingCapture(uint32_t frames) : capacity(std::max(1u, frames)), samples(new float[size_t(capacity) * 2]{}) {}
};
PFRecordingCapture *pfr_create(uint32_t frames) { return new PFRecordingCapture(frames); }
void pfr_destroy(PFRecordingCapture *p) { delete p; }
void pfr_begin(PFRecordingCapture *p, uint64_t maximum) {
    p->written.store(0); p->consumed.store(0); p->reason.store(0);
    p->limit = maximum;
    p->enabled.store(maximum > 0);
}
void pfr_stop(PFRecordingCapture *p) { p->enabled.store(false); }
void pfr_capture(PFRecordingCapture *p, const float *l, const float *r, uint32_t frames) {
    // Sequentially consistent gate: a consumer observing stopped + !inFlight
    // cannot miss a producer that already observed enabled. Later producers do nothing.
    p->inFlight.store(true);
    if (p->enabled.load()) {
        const auto w = p->written.load(std::memory_order_relaxed);
        const auto count = uint32_t(std::min(uint64_t(frames), p->limit - w));
        const auto available = p->capacity - (w - p->consumed.load(std::memory_order_acquire));
        if (count > available) {
            p->reason.store(2); p->enabled.store(false); // Never skip a block and append later audio.
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                const auto slot = (w + i) % p->capacity;
                p->samples[slot * 2] = l[i]; p->samples[slot * 2 + 1] = r[i];
            }
            p->written.store(w + count, std::memory_order_release);
            if (w + count >= p->limit) { p->reason.store(1); p->enabled.store(false); }
        }
    }
    p->inFlight.store(false);
}
uint32_t pfr_read(PFRecordingCapture *p, float *out, uint32_t maximum) {
    const auto r = p->consumed.load(std::memory_order_relaxed);
    const auto count = uint32_t(std::min(uint64_t(maximum), p->written.load(std::memory_order_acquire) - r));
    for (uint32_t i = 0; i < count; ++i) {
        const auto slot = (r + i) % p->capacity;
        out[i * 2] = p->samples[slot * 2]; out[i * 2 + 1] = p->samples[slot * 2 + 1];
    }
    p->consumed.store(r + count, std::memory_order_release);
    return count;
}
bool pfr_finished(const PFRecordingCapture *p) { return !p->enabled.load() && !p->inFlight.load(); }
uint64_t pfr_frames(const PFRecordingCapture *p) { return p->written.load(std::memory_order_acquire); }
int pfr_reason(const PFRecordingCapture *p) { return p->reason.load(); }
