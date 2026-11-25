/* pcm.c
**
** Copyright 2011, The Android Open Source Project
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are met:
**     * Redistributions of source code must retain the above copyright
**       notice, this list of conditions and the following disclaimer.
**     * Redistributions in binary form must reproduce the above copyright
**       notice, this list of conditions and the following disclaimer in the
**       documentation and/or other materials provided with the distribution.
**     * Neither the name of The Android Open Source Project nor the names of
**       its contributors may be used to endorse or promote products derived
**       from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY The Android Open Source Project ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED. IN NO EVENT SHALL The Android Open Source Project BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
** SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
** CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
** DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h>

#include <linux/ioctl.h>

#ifndef __force
#define __force
#endif

#ifndef __bitwise
#define __bitwise
#endif

#ifndef __user
#define __user
#endif

#include <sound/asound.h>

#include <tinyalsa/pcm.h>
#include <tinyalsa/limits.h>
#include "pcm_io.h"

#ifndef PARAM_MAX
#define PARAM_MAX SNDRV_PCM_HW_PARAM_LAST_INTERVAL
#endif /* PARAM_MAX */

#ifndef SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP
#define SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP (1<<2)
#endif /* SNDRV_PCM_HW_PARAMS_NO_PERIOD_WAKEUP */

/* Logs information into a string; follows snprintf() in that
 * offset may be greater than size, and though no characters are copied
 * into string, characters are still counted into offset. */
#define STRLOG(string, offset, size, ...) \
    do { int temp, clipoffset = offset > size ? size : offset; \
         temp = snprintf(string + clipoffset, size - clipoffset, __VA_ARGS__); \
         if (temp > 0) offset += temp; } while (0)

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* refer to SNDRV_PCM_ACCESS_##index in sound/asound.h. */
static const char * const access_lookup[] = {
        "MMAP_INTERLEAVED",
        "MMAP_NONINTERLEAVED",
        "MMAP_COMPLEX",
        "RW_INTERLEAVED",
        "RW_NONINTERLEAVED",
};

/* refer to SNDRV_PCM_FORMAT_##index in sound/asound.h. */
static const char * const format_lookup[] = {
        /*[0] =*/ "S8",
        "U8",
        "S16_LE",
        "S16_BE",
        "U16_LE",
        "U16_BE",
        "S24_LE",
        "S24_BE",
        "U24_LE",
        "U24_BE",
        "S32_LE",
        "S32_BE",
        "U32_LE",
        "U32_BE",
        "FLOAT_LE",
        "FLOAT_BE",
        "FLOAT64_LE",
        "FLOAT64_BE",
        "IEC958_SUBFRAME_LE",
        "IEC958_SUBFRAME_BE",
        "MU_LAW",
        "A_LAW",
        "IMA_ADPCM",
        "MPEG",
        /*[24] =*/ "GSM",
        /* gap */
        [31] = "SPECIAL",
        "S24_3LE",
        "S24_3BE",
};

struct pcm {
    struct pcm_config config;
    const struct pcm_ops *ops;
    void *data;
    unsigned int flags;
    unsigned int subdevice;
    unsigned char mmap_status[4096];
    struct snd_pcm_mmap_status *mmap_status_ptr;
    unsigned int mmap_buffer_size;
    void *mmap_buffer;
    unsigned int buffer_size;
    unsigned int xruns;
    long pcm_delay;
    char error[256];
    int fd;
};

static struct pcm bad_pcm = {0};

static inline int pcm_state(const struct pcm *pcm)
{
    if (!pcm->mmap_status_ptr)
        return PCM_STATE_OPEN;
    return pcm->mmap_status_ptr->state;
}

static int oops(struct pcm *pcm, int err, const char *format, ...)
{
    va_list ap;
    int len;

    va_start(ap, format);
    len = vsnprintf(pcm->error, sizeof(pcm->error), format, ap);
    va_end(ap);

    if (len < 0 || len >= (int) sizeof(pcm->error))
        len = sizeof(pcm->error) - 1;
    pcm->error[len] = '\0';

    errno = err;
    return -1;
}

unsigned int pcm_frames_to_bytes(const struct pcm *pcm, unsigned int frames)
{
    return frames * pcm->config.channels * pcm_format_to_bits(pcm->config.format) / 8;
}

unsigned int pcm_bytes_to_frames(const struct pcm *pcm, unsigned int bytes)
{
    return bytes / (pcm->config.channels * pcm_format_to_bits(pcm->config.format) / 8);
}

unsigned int pcm_format_to_bits(enum pcm_format format)
{
    switch (format) {
    case PCM_FORMAT_S8:
    case PCM_FORMAT_U8:
        return 8;
    case PCM_FORMAT_S16_LE:
    case PCM_FORMAT_S16_BE:
    case PCM_FORMAT_U16_LE:
    case PCM_FORMAT_U16_BE:
        return 16;
    case PCM_FORMAT_S24_LE:
    case PCM_FORMAT_S24_BE:
    case PCM_FORMAT_U24_LE:
    case PCM_FORMAT_U24_BE:
    case PCM_FORMAT_S24_3LE:
    case PCM_FORMAT_S24_3BE:
        return 24;
    case PCM_FORMAT_S32_LE:
    case PCM_FORMAT_S32_BE:
    case PCM_FORMAT_U32_LE:
    case PCM_FORMAT_U32_BE:
    case PCM_FORMAT_FLOAT_LE:
    case PCM_FORMAT_FLOAT_BE:
        return 32;
    case PCM_FORMAT_FLOAT64_LE:
    case PCM_FORMAT_FLOAT64_BE:
        return 64;
    default:
        return 16;
    }
}

const char *pcm_get_error(const struct pcm *pcm)
{
    return pcm->error;
}

unsigned int pcm_get_channels(const struct pcm *pcm)
{
    return pcm->config.channels;
}

const struct pcm_config *pcm_get_config(const struct pcm *pcm)
{
    return &pcm->config;
}

unsigned int pcm_get_rate(const struct pcm *pcm)
{
    return pcm->config.rate;
}

enum pcm_format pcm_get_format(const struct pcm *pcm)
{
    return pcm->config.format;
}

int pcm_get_file_descriptor(const struct pcm *pcm)
{
    return pcm->fd;
}

int pcm_is_ready(const struct pcm *pcm)
{
    return pcm != &bad_pcm && pcm->fd >= 0;
}

static int pcm_hw_mmap_status(struct pcm *pcm)
{
    /* mmap not used; just initialise the status pointer so state checks work */
    if (!pcm->mmap_status_ptr) {
        pcm->mmap_status_ptr = (struct snd_pcm_mmap_status *)pcm->mmap_status;
        memset(pcm->mmap_status_ptr, 0, sizeof(struct snd_pcm_mmap_status));
        pcm->mmap_status_ptr->state = PCM_STATE_RUNNING;
    }
    return 0;
}

/* --- hw_params manipulation using kernel UAPI masks/intervals ---
 *
 * The kernel's snd_pcm_hw_params uses struct snd_mask (for enumerated
 * params like access/format) and struct snd_interval (for range params
 * like channels/rate/period_size).  We manipulate these directly via
 * ioctl(SNDRV_PCM_IOCTL_HW_PARAMS) instead of relying on alsa-lib.
 */

static int pcm_set_mask(struct snd_pcm_hw_params *params, unsigned int param,
                        unsigned int val)
{
    unsigned int idx = param - SNDRV_PCM_HW_PARAM_FIRST_MASK;
    if (idx >= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK + 1)
        return -EINVAL;
    /* clear all bits, then set the one we want */
    memset(params->masks[idx].bits, 0, sizeof(params->masks[idx].bits));
    params->masks[idx].bits[val / 32] |= (1U << (val % 32));
    return 0;
}

static int pcm_set_interval(struct snd_pcm_hw_params *params, unsigned int param,
                            unsigned int min, unsigned int max)
{
    unsigned int idx = param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
    if (idx >= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1)
        return -EINVAL;
    params->intervals[idx].min = min;
    params->intervals[idx].max = max;
    return 0;
}

static int pcm_get_interval(struct snd_pcm_hw_params *params, unsigned int param,
                            unsigned int *min, unsigned int *max)
{
    unsigned int idx = param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL;
    if (idx >= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL + 1)
        return -EINVAL;
    *min = params->intervals[idx].min;
    *max = params->intervals[idx].max;
    return 0;
}

/* Initialize hw params with full hardware ranges (like snd_pcm_hw_params_any()).
 * Sets all masks to all-bits and all intervals to min=0, max=~0U.
 * Sets rmask/cmask for the params we're about to constrain, so the kernel
 * knows which ones we care about. */
static unsigned int pcm_format_to_alsa(enum pcm_format format)
{
    switch (format) {
    case PCM_FORMAT_S16_LE: return SNDRV_PCM_FORMAT_S16_LE;
    case PCM_FORMAT_S32_LE: return SNDRV_PCM_FORMAT_S32_LE;
    case PCM_FORMAT_S8: return SNDRV_PCM_FORMAT_S8;
    case PCM_FORMAT_S24_LE: return SNDRV_PCM_FORMAT_S24_LE;
    case PCM_FORMAT_S24_3LE: return SNDRV_PCM_FORMAT_S24_3LE;
    default: return SNDRV_PCM_FORMAT_S16_LE;
    }
}

static void pcm_params_any(struct snd_pcm_hw_params *p)
{
    unsigned int n;

    memset(p, 0, sizeof(*p));
    p->flags = 0;
    p->rmask = ~0U;

    for (n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; n++) {
        p->masks[n].bits[0] = ~0U;
        p->masks[n].bits[1] = ~0U;
    }

    for (n = 0; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n++) {
        p->intervals[n].min = 0;
        p->intervals[n].max = ~0U;
    }
}

int pcm_set_config(struct pcm *pcm, const struct pcm_config *config)
{
    struct snd_pcm_hw_params *params;
    struct snd_pcm_sw_params *sparams;
    unsigned int min, max;
    int rc;

    if (!pcm || !config)
        return oops((struct pcm *)pcm, EINVAL, "invalid argument");

    memcpy(&pcm->config, config, sizeof(pcm->config));

    params = calloc(1, sizeof(*params));
    if (!params)
        return oops((struct pcm *)pcm, ENOMEM, "can't allocate hw params");

    /* Initialize params with full hardware ranges, then set our constraints */
    pcm_params_any(params);

    /* Set access type: interleaved read/write */
    pcm_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);

    /* Set format */
    pcm_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, pcm_format_to_alsa(config->format));

    /* Set channels */
    pcm_set_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS,
                     config->channels, config->channels);

    /* Set rate */
    pcm_set_interval(params, SNDRV_PCM_HW_PARAM_RATE,
                     config->rate, config->rate);

    /* Set period size */
    pcm_set_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
                     config->period_size, config->period_size);

    /* Set period count */
    pcm_set_interval(params, SNDRV_PCM_HW_PARAM_PERIODS,
                     config->period_count, config->period_count);

    /* Apply hw params (single call) */
    rc = pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_HW_PARAMS, params);
    if (rc < 0) {
        rc = errno;
        free(params);
        return oops((struct pcm *)pcm, rc, "can't set hw params");
    }

    /* Read back the actual configuration the driver settled on */
    pcm_get_interval(params, SNDRV_PCM_HW_PARAM_RATE, &min, &max);
    pcm->config.rate = min;
    pcm_get_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS, &min, &max);
    pcm->config.channels = min;
    pcm_get_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, &min, &max);
    pcm->config.period_size = min;
    pcm_get_interval(params, SNDRV_PCM_HW_PARAM_PERIODS, &min, &max);
    pcm->config.period_count = min;
    /* Format is a mask; read back the first set bit */
    {
        unsigned int fmt_idx = SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK;
        unsigned int f;
        for (f = 0; f < 32; f++) {
            if (params->masks[fmt_idx].bits[f / 32] & (1U << (f % 32))) {
                pcm->config.format = (enum pcm_format)f;
                break;
            }
        }
    }
    free(params);

    /* Buffer size = period_size * period_count frames */
    pcm->buffer_size = pcm->config.period_size * pcm->config.period_count;

    /* Set software params */
    sparams = calloc(1, sizeof(*sparams));
    if (!sparams)
        return oops((struct pcm *)pcm, ENOMEM, "can't allocate sw params");

    /* Set software params directly (some drivers reject all-zero "get") */
    sparams->tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
    sparams->period_step = 1;
    sparams->sleep_min = 0;
    sparams->avail_min = pcm->config.period_size;
    if (pcm->config.start_threshold)
        sparams->start_threshold = pcm->config.start_threshold;
    else
        sparams->start_threshold = pcm->config.period_size;
    if (pcm->config.stop_threshold)
        sparams->stop_threshold = pcm->config.stop_threshold;
    else
        sparams->stop_threshold = pcm->buffer_size;
    sparams->silence_threshold = 0;
    sparams->silence_size = 0;
    sparams->boundary = pcm->buffer_size;

    rc = pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_SW_PARAMS, sparams);
    free(sparams);
    if (rc < 0)
        return oops((struct pcm *)pcm, errno, "can't set sw params");

    return 0;
}

struct pcm *pcm_open(unsigned int card, unsigned int device,
                     unsigned int flags, const struct pcm_config *config)
{
    struct pcm *pcm;
    struct snd_pcm_info info;
    int rc;

    pcm = calloc(1, sizeof(struct pcm));
    if (!pcm) {
        oops((struct pcm *)&bad_pcm, ENOMEM, "can't allocate PCM object");
        return &bad_pcm;
    }

    /* Open the hardware PCM device directly via pcm_hw_ops */
    pcm->ops = &hw_ops;
    pcm->fd = pcm->ops->open(card, device, flags, &pcm->data, NULL);

    if (pcm->fd < 0) {
        oops((struct pcm *)&bad_pcm, errno, "cannot open device (%u) for card (%u)",
             device, card);
        free(pcm);
        return &bad_pcm;
    }

    pcm->flags = flags;

    if (pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_INFO, &info)) {
        oops((struct pcm *)pcm, errno, "cannot get info");
        pcm->ops->close(pcm->data);
        free(pcm);
        return &bad_pcm;
    }
    pcm->subdevice = info.subdevice;

    if (pcm_set_config(pcm, config) != 0) {
        memcpy(bad_pcm.error, pcm->error, sizeof(pcm->error));
        pcm->ops->close(pcm->data);
        free(pcm);
        return &bad_pcm;
    }

    rc = pcm_hw_mmap_status(pcm);
    if (rc < 0) {
        oops((struct pcm *)pcm, errno, "mmap status failed");
        pcm->ops->close(pcm->data);
        free(pcm);
        return &bad_pcm;
    }

    pcm->xruns = 0;
    return pcm;
}

int pcm_close(struct pcm *pcm)
{
    if (pcm == &bad_pcm || pcm == NULL)
        return -EINVAL;

    if (pcm->fd >= 0)
        pcm->ops->close(pcm->data);

    free(pcm);
    return 0;
}

static int pcm_rw_transfer(struct pcm *pcm, void *data, unsigned int frames)
{
    int is_playback;

    struct snd_xferi transfer;
    int res;

    is_playback = !(pcm->flags & PCM_IN);

    transfer.buf = data;
    transfer.frames = frames;
    transfer.result = 0;

    res = pcm->ops->ioctl(pcm->data, is_playback
                          ? SNDRV_PCM_IOCTL_WRITEI_FRAMES
                          : SNDRV_PCM_IOCTL_READI_FRAMES, &transfer);

    return res == 0 ? (int) transfer.result : -1;
}

static int pcm_generic_transfer(struct pcm *pcm, void *data,
                                unsigned int frames)
{
    int res;

#if UINT_MAX > TINYALSA_FRAMES_MAX
    if (frames > TINYALSA_FRAMES_MAX)
        return -EINVAL;
#endif
    if (frames > INT_MAX)
        return -EINVAL;

    if (pcm_state(pcm) == PCM_STATE_SETUP && pcm_prepare(pcm) != 0) {
        return -1;
    }

again:

    if (pcm->flags & PCM_MMAP)
        res = -1; /* mmap not supported in this build */
    else
        res = pcm_rw_transfer(pcm, data, frames);

    if (res < 0) {
        switch (errno) {
        case EPIPE:
            pcm->xruns++;
            /* fallthrough */
        case ESTRPIPE:
            /*
             * Try to restart if we are allowed to do so.
             * Otherwise, return error.
             */
            if (pcm->flags & PCM_NORESTART || pcm_prepare(pcm))
                return -1;
            goto again;
        case EAGAIN:
            if (pcm->flags & PCM_NONBLOCK)
                return -1;
            /* fallthrough */
        default:
            return oops(pcm, errno, "cannot read/write stream data");
        }
    }

    return res;
}

int pcm_writei(struct pcm *pcm, const void *data, unsigned int frame_count)
{
    if (pcm->flags & PCM_IN)
        return -EINVAL;

    return pcm_generic_transfer(pcm, (void*) data, frame_count);
}

int pcm_readi(struct pcm *pcm, void *data, unsigned int frame_count)
{
    if (!(pcm->flags & PCM_IN))
        return -EINVAL;

    return pcm_generic_transfer(pcm, data, frame_count);
}

int pcm_write(struct pcm *pcm, const void *data, unsigned int count)
{
    unsigned int requested_frames = pcm_bytes_to_frames(pcm, count);
    int ret = pcm_writei(pcm, data, requested_frames);

    if (ret < 0)
        return ret;

    return ((unsigned int )ret == requested_frames) ? 0 : -EIO;
}

int pcm_read(struct pcm *pcm, void *data, unsigned int count)
{
    unsigned int requested_frames = pcm_bytes_to_frames(pcm, count);
    int ret = pcm_readi(pcm, data, requested_frames);

    if (ret < 0)
        return ret;

    return ((unsigned int )ret == requested_frames) ? 0 : -EIO;
}

long pcm_get_delay(struct pcm *pcm)
{
    if (pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_DELAY, &pcm->pcm_delay) < 0)
        return -1;

    return pcm->pcm_delay;
}

int pcm_ioctl(struct pcm *pcm, int request, ...)
{
    va_list ap;
    void * arg;

    if (!pcm_is_ready(pcm))
        return -1;

    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    return pcm->ops->ioctl(pcm->data, request, arg);
}

int pcm_prepare(struct pcm *pcm)
{
    if (!pcm_is_ready(pcm))
        return oops(pcm, EINVAL, "pcm is not ready");

    return pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_PREPARE, 0);
}

int pcm_start(struct pcm *pcm)
{
    if (!pcm_is_ready(pcm))
        return oops(pcm, EINVAL, "pcm is not ready");

    return pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_START, 0);
}

int pcm_drain(struct pcm *pcm)
{
    if (!pcm_is_ready(pcm))
        return oops(pcm, EINVAL, "pcm is not ready");

    return pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_DRAIN, 0);
}

int pcm_stop(struct pcm *pcm)
{
    if (!pcm_is_ready(pcm))
        return oops(pcm, EINVAL, "pcm is not ready");

    return pcm->ops->ioctl(pcm->data, SNDRV_PCM_IOCTL_DROP, 0);
}

int pcm_wait(struct pcm *pcm, int timeout)
{
    if (!pcm_is_ready(pcm))
        return oops(pcm, EINVAL, "pcm is not ready");

    return pcm->ops->poll(pcm->data, NULL, 0, timeout);
}
