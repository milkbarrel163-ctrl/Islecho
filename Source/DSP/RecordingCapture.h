#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct PFRecordingCapture PFRecordingCapture;
// Allocation/destruction outside render; destroy only after engine and consumer stop.
PFRecordingCapture *pfr_create(uint32_t capacityFrames);
void pfr_destroy(PFRecordingCapture *);
// One control thread. Begin only after previous consumer finished, with capture stopped.
void pfr_begin(PFRecordingCapture *, uint64_t maximumFrames);
void pfr_stop(PFRecordingCapture *);
// One audio producer: bounded copy only. No allocation, lock, wait, or I/O.
void pfr_capture(PFRecordingCapture *, const float *left, const float *right, uint32_t frames);
// One background consumer, interleaved stereo output. Finished implies no later writes.
uint32_t pfr_read(PFRecordingCapture *, float *interleaved, uint32_t maximumFrames);
bool pfr_finished(const PFRecordingCapture *);
uint64_t pfr_frames(const PFRecordingCapture *);
// 0: user/lifecycle stop; 1: duration cap; 2: overflow (contiguous prefix retained).
int pfr_reason(const PFRecordingCapture *);
#ifdef __cplusplus
}
#endif
