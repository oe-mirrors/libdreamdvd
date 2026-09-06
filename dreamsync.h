#ifndef __DREAMSYNC_H__
#define __DREAMSYNC_H__

#include <stdint.h>

/* The decoder keeps its private, continuous 90 kHz timeline across DVD cells.
 * Lock its elapsed presentation time to played PCM, not to the producer or
 * the amount of audio waiting in the userspace queue. */
struct ddvd_dream_sync {
	int valid;
	unsigned int generation;
	uint32_t video_anchor;
	int64_t audio_anchor;
	int64_t filtered_error;
};

static inline int64_t ddvd_dream_sync_error(struct ddvd_dream_sync *sync,
	uint32_t video_pts, int64_t audio_pts, unsigned int generation)
{
	int64_t error;

	if (!sync->valid || sync->generation != generation) {
		sync->valid = 1;
		sync->generation = generation;
		sync->video_anchor = video_pts;
		sync->audio_anchor = audio_pts;
		sync->filtered_error = 0;
		return 0;
	}
	/* Both counters can cross the driver's 32-bit PTS wrap. */
	error = (int32_t)((video_pts - sync->video_anchor) -
		(uint32_t)(audio_pts - sync->audio_anchor));
	if (error > 10 * 90000 || error < -10 * 90000) {
		/* Seek/discontinuity, not oscillator drift. Never try to catch up a
		 * different title by feeding the decoder in a burst. */
		sync->valid = 0;
		return 0;
	}
	sync->filtered_error += (error - sync->filtered_error) / 8;
	return sync->filtered_error;
}

static inline int64_t ddvd_dream_sync_adjustment(int64_t error_pts,
	int64_t frame_us)
{
	int64_t adjustment;
	int64_t limit = frame_us / 20; /* At most 5%, no jumps or audio drops. */

	/* VPTS is quantized to pictures/fields; do not chase that jitter. */
	if (error_pts >= -3600 && error_pts <= 3600)
		return 0;
	error_pts += error_pts > 0 ? -3600 : 3600;
	adjustment = error_pts * frame_us / (90000 * 4);
	if (adjustment > limit)
		adjustment = limit;
	else if (adjustment < -limit)
		adjustment = -limit;
	return adjustment;
}

#endif
