#ifndef TINYALSA_LIMITS_H
#define TINYALSA_LIMITS_H

#include <tinyalsa/interval.h>

#include <limits.h>
#include <stdint.h>

#define TINYALSA_SIGNED_INTERVAL_MAX SSIZE_MAX
#define TINYALSA_SIGNED_INTERVAL_MIN SSIZE_MIN

#define TINYALSA_UNSIGNED_INTERVAL_MAX SIZE_MAX
#define TINYALSA_UNSIGNED_INTERVAL_MIN SIZE_MIN

#define TINYALSA_CHANNELS_MAX 32U
#define TINYALSA_CHANNELS_MIN 1U

#define TINYALSA_FRAMES_MAX (ULONG_MAX / (TINYALSA_CHANNELS_MAX * 4))
#define TINYALSA_FRAMES_MIN 0U

#if TINYALSA_FRAMES_MAX > TINYALSA_UNSIGNED_INTERVAL_MAX
#error "Frames max exceeds measurable value."
#endif

#if TINYALSA_FRAMES_MIN < TINYALSA_UNSIGNED_INTERVAL_MIN
#error "Frames min exceeds measurable value."
#endif

extern const struct tinyalsa_unsigned_interval tinyalsa_channels_limit;

extern const struct tinyalsa_unsigned_interval tinyalsa_frames_limit;

#endif /* TINYALSA_LIMITS_H */