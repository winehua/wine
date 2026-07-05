/*
 * OHOS audio driver (unixlib)
 *
 * Copyright 2026 OpenAI
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#include "winternl.h"

#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"
#include "mmddk.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "../mmdevapi/unixlib.h"
#include "ohos_audio_client.h"
#include "ohos_midi.h"

WINE_DEFAULT_DEBUG_CHANNEL(ohosaudio);

static const REFERENCE_TIME default_period = 100000; /* 10 ms */
static const REFERENCE_TIME minimum_period = 50000;  /* 5 ms */

static const WCHAR render_device_name[] =
{
    'O','H','O','S',' ','A','u','d','i','o',' ','S','p','e','a','k','e','r',0
};
static const char render_device_id[] = "default";
static const WCHAR capture_device_name[] =
{
    'O','H','O','S',' ','A','u','d','i','o',' ','M','i','c','r','o','p','h','o','n','e',0
};
static const char capture_device_id[] = "capture-default";
#define MIX_FRAMES_ERROR (~0u)

struct ohos_stream
{
    OhosAudioClient *client;
    OhosAudioClientStream broker_stream;
    EDataFlow flow;
    WAVEFORMATEXTENSIBLE *fmt;
    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    BOOL started;
    BOOL stop_notify_thread;
    HANDLE notify_thread;
    UINT32 client_period_frames;
    UINT32 client_buffer_frames;
    UINT32 client_bytes_per_frame;
    UINT32 mix_period_frames;
    UINT32 mix_buffer_frames;
    UINT32 mix_bytes_per_frame;
    UINT32 locked_frames;
    UINT32 mix_buffer_capacity_frames;
    UINT64 client_frames_submitted;
    UINT64 last_position;
    UINT64 client_input_frames;
    UINT64 mix_output_frames;
    float channel_volume[2];
    float prev_frame[2];
    BOOL have_prev_frame;
    UINT32 capture_read_offset_frames;
    UINT32 capture_write_offset_frames;
    UINT32 capture_held_frames;
    UINT32 capture_wrap_buffer_frames;
    UINT64 capture_mix_input_frames;
    UINT64 capture_client_output_frames;
    float capture_prev_frame[2];
    BOOL capture_have_prev_frame;
    BYTE *local_buffer;
    BYTE *capture_wrap_buffer;
    INT16 *mix_buffer;
    pthread_mutex_t lock;
};

static BOOL is_capture_stream(const struct ohos_stream *stream)
{
    return stream->flow == eCapture;
}

static NTSTATUS ohos_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

static struct ohos_stream *handle_get_stream(stream_handle handle)
{
    return (struct ohos_stream *)(UINT_PTR)handle;
}

static void fill_capture_local_buffer_locked(struct ohos_stream *stream);

static void ohos_lock(struct ohos_stream *stream)
{
    pthread_mutex_lock(&stream->lock);
}

static void ohos_unlock(struct ohos_stream *stream)
{
    pthread_mutex_unlock(&stream->lock);
}

static NTSTATUS ohos_unlock_result(struct ohos_stream *stream, HRESULT *result, HRESULT value)
{
    *result = value;
    ohos_unlock(stream);
    return STATUS_SUCCESS;
}

static UINT64 query_qpc_100ns(void)
{
    LARGE_INTEGER stamp, freq;

    NtQueryPerformanceCounter(&stamp, &freq);
    return (stamp.QuadPart * (UINT64)10000000) / freq.QuadPart;
}

static UINT32 get_channel_mask(UINT32 channels)
{
    switch (channels)
    {
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    default:
        return 0;
    }
}

static BOOL is_float_format(const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return TRUE;
    return fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

static BOOL is_pcm_format(const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fmtex = (const WAVEFORMATEXTENSIBLE *)fmt;

    if (fmt->wFormatTag == WAVE_FORMAT_PCM) return TRUE;
    return fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM);
}

static UINT16 get_valid_bits_per_sample(const WAVEFORMATEX *fmt)
{
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return ((const WAVEFORMATEXTENSIBLE *)fmt)->Samples.wValidBitsPerSample;
    return fmt->wBitsPerSample;
}

static BOOL is_supported_sample_rate(UINT32 rate)
{
    /* The broker always mixes to 48 kHz stereo s16, so we can accept
     * common shared-mode client streams and resample them on either side
     * of the mix rate instead of rejecting them up front. */
    return rate >= 8000 && rate <= 192000;
}

static HRESULT validate_format(const WAVEFORMATEX *fmt)
{
    UINT16 valid_bits;

    if (!fmt) return E_POINTER;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (fmt->nChannels != 1 && fmt->nChannels != 2)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (!is_supported_sample_rate(fmt->nSamplesPerSec))
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    valid_bits = get_valid_bits_per_sample(fmt);
    if (!valid_bits || valid_bits > fmt->wBitsPerSample)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (is_pcm_format(fmt))
    {
        if (fmt->wBitsPerSample != 8 && fmt->wBitsPerSample != 16 &&
            fmt->wBitsPerSample != 24 && fmt->wBitsPerSample != 32)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (fmt->wBitsPerSample == 32)
        {
            if (valid_bits != 24 && valid_bits != 32)
                return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        else if (valid_bits != fmt->wBitsPerSample)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    else if (is_float_format(fmt))
    {
        if (fmt->wBitsPerSample != 32 && fmt->wBitsPerSample != 64)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        if (valid_bits != fmt->wBitsPerSample)
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    else return AUDCLNT_E_UNSUPPORTED_FORMAT;

    if (fmt->nBlockAlign != fmt->nChannels * fmt->wBitsPerSample / 8)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (fmt->nAvgBytesPerSec != fmt->nSamplesPerSec * fmt->nBlockAlign)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    return S_OK;
}

static WAVEFORMATEXTENSIBLE *clone_wave_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEXTENSIBLE *ret;

    if (!(ret = calloc(1, sizeof(*ret)))) return NULL;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        memcpy(ret, fmt, sizeof(*ret));
        return ret;
    }

    ret->Format = *fmt;
    ret->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ret->Format.cbSize = sizeof(*ret) - sizeof(ret->Format);
    ret->Samples.wValidBitsPerSample = fmt->wBitsPerSample;
    ret->dwChannelMask = get_channel_mask(fmt->nChannels);
    ret->SubFormat = is_float_format(fmt) ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                          : KSDATAFORMAT_SUBTYPE_PCM;
    return ret;
}

static float read_pcm_u8(const BYTE *src)
{
    return ((INT32)src[0] - 128) / 128.0f;
}

static float read_pcm_s16(const BYTE *src)
{
    INT16 sample;

    memcpy(&sample, src, sizeof(sample));
    return sample / 32768.0f;
}

static float read_pcm_s24(const BYTE *src)
{
    INT32 sample = (INT32)((UINT32)src[0] | ((UINT32)src[1] << 8) | ((UINT32)src[2] << 16));

    if (sample & 0x00800000) sample |= ~0x00ffffff;
    return sample / 8388608.0f;
}

static float read_pcm_s32(const BYTE *src)
{
    INT32 sample;

    memcpy(&sample, src, sizeof(sample));
    return (float)((double)sample / 2147483648.0);
}

static float read_ieee_float32(const BYTE *src)
{
    float sample;

    memcpy(&sample, src, sizeof(sample));
    return sample;
}

static float read_ieee_float64(const BYTE *src)
{
    double sample;

    memcpy(&sample, src, sizeof(sample));
    return (float)sample;
}

static INT16 clamp_s16_float(float sample)
{
    if (sample > 32767.0f) return 32767;
    if (sample < -32768.0f) return -32768;
    return (INT16)sample;
}

static float clamp_unit_float(float sample)
{
    if (sample > 1.0f) return 1.0f;
    if (sample < -1.0f) return -1.0f;
    return sample;
}

static void write_client_frame(struct ohos_stream *stream, BYTE *dst, float left, float right)
{
    UINT32 channels = stream->fmt->Format.nChannels;
    UINT32 bytes_per_sample = stream->fmt->Format.wBitsPerSample / 8;
    UINT16 valid_bits = get_valid_bits_per_sample(&stream->fmt->Format);
    float samples[2];
    UINT32 ch;

    left = clamp_unit_float(left);
    right = clamp_unit_float(right);
    samples[0] = channels == 1 ? (left + right) * 0.5f : left;
    samples[1] = channels == 1 ? samples[0] : right;

    for (ch = 0; ch < channels; ++ch)
    {
        BYTE *out = dst + ch * bytes_per_sample;
        float sample = samples[ch];

        if (is_float_format(&stream->fmt->Format))
        {
            if (bytes_per_sample == 4)
            {
                memcpy(out, &sample, sizeof(sample));
            }
            else
            {
                double sample64 = sample;
                memcpy(out, &sample64, sizeof(sample64));
            }
            continue;
        }

        switch (bytes_per_sample)
        {
        case 1:
        {
            int value = lrintf(sample * 127.0f) + 128;
            if (value < 0) value = 0;
            if (value > 255) value = 255;
            out[0] = (BYTE)value;
            break;
        }
        case 2:
        {
            INT16 value = clamp_s16_float(sample * 32767.0f);
            memcpy(out, &value, sizeof(value));
            break;
        }
        case 3:
        {
            INT32 value = lrintf(sample * 8388607.0f);
            if (value < -8388608) value = -8388608;
            if (value > 8388607) value = 8388607;
            out[0] = value & 0xff;
            out[1] = (value >> 8) & 0xff;
            out[2] = (value >> 16) & 0xff;
            break;
        }
        default:
        {
            INT32 value;

            if (valid_bits == 24)
            {
                value = lrintf(sample * 8388607.0f) << 8;
            }
            else
            {
                double scaled = sample * 2147483647.0;
                if (scaled > 2147483647.0) scaled = 2147483647.0;
                if (scaled < -2147483648.0) scaled = -2147483648.0;
                value = (INT32)scaled;
            }
            memcpy(out, &value, sizeof(value));
            break;
        }
        }
    }
}

static HRESULT map_broker_error(int err, HRESULT fallback)
{
    switch (err < 0 ? -err : err)
    {
    case 0:
        return S_OK;
    case ENOENT:
    case ENODEV:
    case ECONNRESET:
    case EPIPE:
        return AUDCLNT_E_SERVICE_NOT_RUNNING;
    case ENOMEM:
        return E_OUTOFMEMORY;
    case EINVAL:
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    case EACCES:
    case EPERM:
        return E_ACCESSDENIED;
    case EBUSY:
        return AUDCLNT_E_BUFFER_TOO_LARGE;
    default:
        return fallback;
    }
}

static BOOL probe_broker_available(void)
{
    OhosAudioClient *client = NULL;
    int ret = ohos_audio_client_connect(&client);

    if (ret != 0)
    {
        TRACE("failed to connect audio broker: %d\n", ret);
        return FALSE;
    }

    ohos_audio_client_disconnect(client);
    return TRUE;
}

static UINT32 client_frames_to_mix_frames_ceil(const struct ohos_stream *stream, UINT32 client_frames)
{
    UINT32 rate = stream->fmt->Format.nSamplesPerSec;

    if (!client_frames) return 0;
    if (rate == 48000) return client_frames;
    return (UINT32)(((UINT64)client_frames * 48000 + rate - 1) / rate);
}

static UINT32 mix_frames_to_client_frames_floor(const struct ohos_stream *stream, UINT32 mix_frames)
{
    UINT32 rate = stream->fmt->Format.nSamplesPerSec;

    if (!mix_frames) return 0;
    if (rate == 48000) return mix_frames;
    return (UINT32)(((UINT64)mix_frames * rate) / 48000);
}

static UINT32 mix_frames_to_client_frames_ceil(const struct ohos_stream *stream, UINT32 mix_frames)
{
    UINT32 rate = stream->fmt->Format.nSamplesPerSec;

    if (!mix_frames) return 0;
    if (rate == 48000) return mix_frames;
    return (UINT32)(((UINT64)mix_frames * rate + 47999) / 48000);
}

static void reset_stream_counters_locked(struct ohos_stream *stream)
{
    stream->client_frames_submitted = 0;
    stream->last_position = 0;
    stream->client_input_frames = 0;
    stream->mix_output_frames = 0;
    stream->have_prev_frame = FALSE;
    stream->prev_frame[0] = 0.0f;
    stream->prev_frame[1] = 0.0f;
    stream->capture_read_offset_frames = 0;
    stream->capture_write_offset_frames = 0;
    stream->capture_held_frames = 0;
    stream->capture_mix_input_frames = 0;
    stream->capture_client_output_frames = 0;
    stream->capture_have_prev_frame = FALSE;
    stream->capture_prev_frame[0] = 0.0f;
    stream->capture_prev_frame[1] = 0.0f;
}

static UINT32 query_broker_padding_frames_locked(struct ohos_stream *stream)
{
    WinehuaAudioGetStatusResp status;
    int ret;

    if (!stream->client) return 0;
    ret = ohos_audio_client_get_status(stream->client, stream->broker_stream.stream_id, &status);
    if (ret != 0)
    {
        WARN("ohos_audio_client_get_status failed: %d\n", ret);
        return 0;
    }

    return status.queued_frames;
}

static UINT32 query_client_padding_frames_locked(struct ohos_stream *stream)
{
    if (is_capture_stream(stream))
    {
        fill_capture_local_buffer_locked(stream);
        return stream->capture_held_frames;
    }

    return mix_frames_to_client_frames_ceil(stream, query_broker_padding_frames_locked(stream));
}

static void signal_period_event_locked(struct ohos_stream *stream)
{
    if (!stream->event || !stream->started) return;

    NtSetEvent(stream->event, NULL);
}

static void notify_thread_main(void *arg)
{
    struct ohos_stream *stream = arg;
    LARGE_INTEGER delay;
    UINT32 period_frames = max(1u, stream->client_period_frames);
    UINT32 sample_rate = max(1u, stream->fmt->Format.nSamplesPerSec);
    LONGLONG period_100ns = -max((LONGLONG)10000,
                                 ((LONGLONG)period_frames * 10000000) / sample_rate);

    while (1)
    {
        ohos_lock(stream);
        if (stream->stop_notify_thread)
        {
            ohos_unlock(stream);
            break;
        }
        signal_period_event_locked(stream);
        ohos_unlock(stream);

        delay.QuadPart = period_100ns;
        NtDelayExecution(FALSE, &delay);
    }
}

static HRESULT create_broker_stream(struct ohos_stream *stream, UINT32 requested_client_buffer_frames)
{
    WinehuaAudioOpenStreamReq req;
    int ret;

    memset(&req, 0, sizeof(req));
    req.sample_rate = 48000;
    req.channels = 2;
    req.sample_format = WINEHUA_AUDIO_SAMPLE_S16LE;
    req.buffer_frames = client_frames_to_mix_frames_ceil(stream, requested_client_buffer_frames);
    req.period_frames = client_frames_to_mix_frames_ceil(stream, stream->client_period_frames);
    req.flags = is_capture_stream(stream) ? WINEHUA_AUDIO_STREAM_FLAG_CAPTURE : 0;

    ret = ohos_audio_client_connect(&stream->client);
    if (ret != 0)
    {
        WARN("ohos_audio_client_connect failed: %d\n", ret);
        return map_broker_error(ret, AUDCLNT_E_SERVICE_NOT_RUNNING);
    }

    memset(&stream->broker_stream, 0, sizeof(stream->broker_stream));
    stream->broker_stream.ring_fd = -1;
    ret = ohos_audio_client_open_stream(stream->client, &req, &stream->broker_stream);
    if (ret != 0)
    {
        WARN("ohos_audio_client_open_stream failed: %d\n", ret);
        ohos_audio_client_disconnect(stream->client);
        stream->client = NULL;
        return map_broker_error(ret, AUDCLNT_E_DEVICE_INVALIDATED);
    }

    stream->mix_period_frames = stream->broker_stream.preferred_period_frames ?
        stream->broker_stream.preferred_period_frames : req.period_frames;
    stream->mix_buffer_frames = stream->broker_stream.ring_capacity_frames;
    stream->mix_bytes_per_frame = 4;
    stream->client_buffer_frames = max(1u, mix_frames_to_client_frames_floor(stream, stream->mix_buffer_frames));
    stream->mix_buffer_capacity_frames = client_frames_to_mix_frames_ceil(stream, stream->client_buffer_frames) + 4;

    TRACE("created broker stream rate=%u channels=%u bits=%u client_period=%u client_buffer=%u mix_period=%u mix_buffer=%u stream_id=%u\n",
          stream->fmt->Format.nSamplesPerSec, stream->fmt->Format.nChannels,
          stream->fmt->Format.wBitsPerSample, stream->client_period_frames,
          stream->client_buffer_frames, stream->mix_period_frames,
          stream->mix_buffer_frames, stream->broker_stream.stream_id);
    return S_OK;
}

static void decode_input_frame(const struct ohos_stream *stream, const BYTE *buffer, UINT32 frame, float *left, float *right)
{
    UINT32 channels = stream->fmt->Format.nChannels;
    UINT32 bytes_per_sample = stream->fmt->Format.wBitsPerSample / 8;
    const BYTE *src = buffer + frame * stream->client_bytes_per_frame;
    float sample0 = 0.0f, sample1 = 0.0f;

    if (is_float_format(&stream->fmt->Format))
    {
        if (bytes_per_sample == 4)
        {
            sample0 = read_ieee_float32(src);
            sample1 = channels == 1 ? sample0 : read_ieee_float32(src + bytes_per_sample);
        }
        else
        {
            sample0 = read_ieee_float64(src);
            sample1 = channels == 1 ? sample0 : read_ieee_float64(src + bytes_per_sample);
        }
    }
    else
    {
        switch (bytes_per_sample)
        {
        case 1:
            sample0 = read_pcm_u8(src);
            sample1 = channels == 1 ? sample0 : read_pcm_u8(src + bytes_per_sample);
            break;
        case 2:
            sample0 = read_pcm_s16(src);
            sample1 = channels == 1 ? sample0 : read_pcm_s16(src + bytes_per_sample);
            break;
        case 3:
            sample0 = read_pcm_s24(src);
            sample1 = channels == 1 ? sample0 : read_pcm_s24(src + bytes_per_sample);
            break;
        default:
            sample0 = read_pcm_s32(src);
            sample1 = channels == 1 ? sample0 : read_pcm_s32(src + bytes_per_sample);
            break;
        }
    }

    *left = sample0;
    *right = sample1;
}

static void apply_client_volume(const struct ohos_stream *stream, float *left, float *right)
{
    if (stream->fmt->Format.nChannels == 1)
    {
        float volume = stream->channel_volume[0];
        *left *= volume;
        *right *= volume;
    }
    else
    {
        *left *= stream->channel_volume[0];
        *right *= stream->channel_volume[1];
    }
}

static void store_prev_frame(struct ohos_stream *stream, const BYTE *buffer, UINT32 frames)
{
    if (!frames) return;
    decode_input_frame(stream, buffer, frames - 1, &stream->prev_frame[0], &stream->prev_frame[1]);
    stream->have_prev_frame = TRUE;
}

static void load_source_frame(const struct ohos_stream *stream, const BYTE *buffer, UINT32 frames,
                              UINT64 src_base, INT64 abs_index, float out[2])
{
    if (!frames)
    {
        if (stream->have_prev_frame)
        {
            out[0] = stream->prev_frame[0];
            out[1] = stream->prev_frame[1];
        }
        else out[0] = out[1] = 0.0f;
        return;
    }

    if (abs_index < (INT64)src_base)
    {
        if (stream->have_prev_frame)
        {
            out[0] = stream->prev_frame[0];
            out[1] = stream->prev_frame[1];
            return;
        }
        abs_index = src_base;
    }

    if ((UINT64)abs_index >= src_base + frames)
        abs_index = (INT64)(src_base + frames - 1);

    decode_input_frame(stream, buffer, (UINT32)((UINT64)abs_index - src_base), &out[0], &out[1]);
}

static UINT32 convert_client_frames_to_mix_locked(struct ohos_stream *stream, const BYTE *src,
                                                  UINT32 in_frames, INT16 *dst, UINT32 dst_capacity_frames)
{
    UINT32 i;

    if (!in_frames) return 0;

    if (stream->fmt->Format.nSamplesPerSec == 48000)
    {
        if (in_frames > dst_capacity_frames) return MIX_FRAMES_ERROR;
        for (i = 0; i < in_frames; ++i)
        {
            float left, right;

            decode_input_frame(stream, src, i, &left, &right);
            apply_client_volume(stream, &left, &right);
            dst[i * 2] = clamp_s16_float(left * 32767.0f);
            dst[i * 2 + 1] = clamp_s16_float(right * 32767.0f);
        }

        store_prev_frame(stream, src, in_frames);
        stream->client_input_frames += in_frames;
        stream->mix_output_frames += in_frames;
        return in_frames;
    }
    else
    {
        UINT64 src_base = stream->client_input_frames;
        UINT64 src_total = src_base + in_frames;
        UINT64 target_mix_total = (src_total * 48000ULL) / stream->fmt->Format.nSamplesPerSec;
        UINT64 needed = target_mix_total - stream->mix_output_frames;

        if (needed > dst_capacity_frames) return MIX_FRAMES_ERROR;

        for (i = 0; i < needed; ++i)
        {
            UINT64 out_abs = stream->mix_output_frames + i;
            UINT64 scaled = out_abs * (UINT64)stream->fmt->Format.nSamplesPerSec;
            UINT64 i0 = scaled / 48000ULL;
            UINT64 rem = scaled % 48000ULL;
            float frac = rem / 48000.0f;
            float a[2], b[2], left, right;

            load_source_frame(stream, src, in_frames, src_base, (INT64)i0, a);
            load_source_frame(stream, src, in_frames, src_base, (INT64)i0 + 1, b);
            left = a[0] + (b[0] - a[0]) * frac;
            right = a[1] + (b[1] - a[1]) * frac;
            apply_client_volume(stream, &left, &right);
            dst[i * 2] = clamp_s16_float(left * 32767.0f);
            dst[i * 2 + 1] = clamp_s16_float(right * 32767.0f);
        }

        store_prev_frame(stream, src, in_frames);
        stream->client_input_frames = src_total;
        stream->mix_output_frames = target_mix_total;
        return (UINT32)needed;
    }
}

static void store_capture_prev_frame(struct ohos_stream *stream, const INT16 *buffer, UINT32 frames)
{
    if (!frames) return;

    stream->capture_prev_frame[0] = buffer[(frames - 1) * 2] / 32768.0f;
    stream->capture_prev_frame[1] = buffer[(frames - 1) * 2 + 1] / 32768.0f;
    stream->capture_have_prev_frame = TRUE;
}

static void load_capture_source_frame(const struct ohos_stream *stream, const INT16 *buffer, UINT32 frames,
                                      UINT64 src_base, INT64 abs_index, float out[2])
{
    if (!frames)
    {
        if (stream->capture_have_prev_frame)
        {
            out[0] = stream->capture_prev_frame[0];
            out[1] = stream->capture_prev_frame[1];
        }
        else out[0] = out[1] = 0.0f;
        return;
    }

    if (abs_index < (INT64)src_base)
    {
        if (stream->capture_have_prev_frame)
        {
            out[0] = stream->capture_prev_frame[0];
            out[1] = stream->capture_prev_frame[1];
            return;
        }
        abs_index = src_base;
    }

    if ((UINT64)abs_index >= src_base + frames)
        abs_index = (INT64)(src_base + frames - 1);

    abs_index = (INT64)((UINT64)abs_index - src_base);
    out[0] = buffer[abs_index * 2] / 32768.0f;
    out[1] = buffer[abs_index * 2 + 1] / 32768.0f;
}

static void append_capture_client_frame_locked(struct ohos_stream *stream, float left, float right)
{
    BYTE *dst = stream->local_buffer + stream->capture_write_offset_frames * stream->client_bytes_per_frame;

    write_client_frame(stream, dst, left, right);
    stream->capture_write_offset_frames = (stream->capture_write_offset_frames + 1) % stream->client_buffer_frames;
    stream->capture_held_frames++;
}

static UINT32 convert_mix_frames_to_client_locked(struct ohos_stream *stream, const INT16 *src, UINT32 in_frames)
{
    UINT32 free_frames = stream->client_buffer_frames - stream->capture_held_frames;
    UINT32 i;

    if (!in_frames || !free_frames) return 0;

    if (stream->fmt->Format.nSamplesPerSec == 48000)
    {
        if (in_frames > free_frames) return MIX_FRAMES_ERROR;

        for (i = 0; i < in_frames; ++i)
        {
            float left = src[i * 2] / 32768.0f;
            float right = src[i * 2 + 1] / 32768.0f;

            append_capture_client_frame_locked(stream, left, right);
        }

        store_capture_prev_frame(stream, src, in_frames);
        stream->capture_mix_input_frames += in_frames;
        stream->capture_client_output_frames += in_frames;
        stream->client_frames_submitted += in_frames;
        return in_frames;
    }
    else
    {
        UINT64 src_base = stream->capture_mix_input_frames;
        UINT64 src_total = src_base + in_frames;
        UINT64 target_client_total = (src_total * stream->fmt->Format.nSamplesPerSec) / 48000ULL;
        UINT64 needed = target_client_total - stream->capture_client_output_frames;

        if (needed > free_frames) return MIX_FRAMES_ERROR;

        for (i = 0; i < needed; ++i)
        {
            UINT64 out_abs = stream->capture_client_output_frames + i;
            UINT64 scaled = out_abs * 48000ULL;
            UINT64 i0 = scaled / stream->fmt->Format.nSamplesPerSec;
            UINT64 rem = scaled % stream->fmt->Format.nSamplesPerSec;
            float frac = rem / (float)stream->fmt->Format.nSamplesPerSec;
            float a[2], b[2];
            float left, right;

            load_capture_source_frame(stream, src, in_frames, src_base, (INT64)i0, a);
            load_capture_source_frame(stream, src, in_frames, src_base, (INT64)i0 + 1, b);
            left = a[0] + (b[0] - a[0]) * frac;
            right = a[1] + (b[1] - a[1]) * frac;
            append_capture_client_frame_locked(stream, left, right);
        }

        store_capture_prev_frame(stream, src, in_frames);
        stream->capture_mix_input_frames = src_total;
        stream->capture_client_output_frames = target_client_total;
        stream->client_frames_submitted += (UINT32)needed;
        return (UINT32)needed;
    }
}

static void fill_capture_local_buffer_locked(struct ohos_stream *stream)
{
    UINT32 free_frames;

    if (!is_capture_stream(stream) || !stream->client) return;

    free_frames = stream->client_buffer_frames - stream->capture_held_frames;
    while (free_frames)
    {
        UINT32 available_mix_frames;
        UINT32 mix_frames_to_read;
        size_t got;

        available_mix_frames = (UINT32)ohos_audio_client_get_queued_frames(&stream->broker_stream);
        if (!available_mix_frames) break;

        mix_frames_to_read = min(available_mix_frames, stream->mix_buffer_capacity_frames);
        mix_frames_to_read = min(mix_frames_to_read,
                                 client_frames_to_mix_frames_ceil(stream, free_frames));
        if (!mix_frames_to_read) break;

        got = ohos_audio_client_read_frames(&stream->broker_stream, stream->mix_buffer, mix_frames_to_read);
        if (!got) break;
        if (convert_mix_frames_to_client_locked(stream, stream->mix_buffer, got) == MIX_FRAMES_ERROR) break;
        free_frames = stream->client_buffer_frames - stream->capture_held_frames;
    }
}

static void destroy_stream(struct ohos_stream *stream)
{
    if (!stream) return;

    if (stream->notify_thread)
    {
        ohos_lock(stream);
        stream->stop_notify_thread = TRUE;
        ohos_unlock(stream);
        NtWaitForSingleObject(stream->notify_thread, FALSE, NULL);
        NtClose(stream->notify_thread);
        stream->notify_thread = NULL;
    }

    if (stream->client)
    {
        ohos_audio_client_close_stream(stream->client, &stream->broker_stream);
        ohos_audio_client_disconnect(stream->client);
        stream->client = NULL;
    }

    free(stream->local_buffer);
    free(stream->capture_wrap_buffer);
    free(stream->mix_buffer);
    free(stream->fmt);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
}

static NTSTATUS ohos_process_attach(void *args)
{
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_process_detach(void *args)
{
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    unsigned int needed = 0;
    const WCHAR *device_name;
    const char *device_id;
    UINT32 device_name_size;
    UINT32 device_id_size;

    params->num = 0;
    params->default_idx = 0;

    if (params->flow == eRender)
    {
        device_name = render_device_name;
        device_id = render_device_id;
        device_name_size = sizeof(render_device_name);
        device_id_size = sizeof(render_device_id);
    }
    else if (params->flow == eCapture)
    {
        device_name = capture_device_name;
        device_id = capture_device_id;
        device_name_size = sizeof(capture_device_name);
        device_id_size = sizeof(capture_device_id);
    }
    else
    {
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    needed = sizeof(*params->endpoints) + device_name_size + ((device_id_size + 1) & ~1);
    params->num = 1;

    if (params->size < needed)
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        return STATUS_SUCCESS;
    }

    params->endpoints[0].name = sizeof(*params->endpoints);
    memcpy((char *)params->endpoints + params->endpoints[0].name,
           device_name, device_name_size);
    params->endpoints[0].device = sizeof(*params->endpoints) + device_name_size;
    memcpy((char *)params->endpoints + params->endpoints[0].device,
           device_id, device_id_size);
    params->default_idx = 0;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct ohos_stream *stream = NULL;
    HRESULT hr;
    UINT32 requested_client_buffer_frames;

    TRACE("ohos_create_stream flow=%u share=%u rate=%u channels=%u bits=%u duration=%s period=%s flags=%#x\n",
          params->flow, params->share, params->fmt ? params->fmt->nSamplesPerSec : 0,
          params->fmt ? params->fmt->nChannels : 0, params->fmt ? params->fmt->wBitsPerSample : 0,
          wine_dbgstr_longlong(params->duration), wine_dbgstr_longlong(params->period), params->flags);

    params->result = S_OK;
    *params->stream = 0;
    *params->channel_count = 0;

    if (params->flow != eRender && params->flow != eCapture)
    {
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        return STATUS_SUCCESS;
    }
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
    {
        params->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
        return STATUS_SUCCESS;
    }
    if (FAILED(hr = validate_format(params->fmt)))
    {
        params->result = hr;
        return STATUS_SUCCESS;
    }

    if (!(stream = calloc(1, sizeof(*stream))))
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    pthread_mutex_init(&stream->lock, NULL);
    stream->flow = params->flow;
    stream->share = params->share;
    stream->flags = params->flags;
    stream->client_period_frames = max(1u, (UINT32)((params->period * params->fmt->nSamplesPerSec) / 10000000));
    requested_client_buffer_frames = max(stream->client_period_frames * 2,
                                         (UINT32)((params->duration * params->fmt->nSamplesPerSec + 9999999) / 10000000));
    stream->client_bytes_per_frame = params->fmt->nBlockAlign;
    stream->channel_volume[0] = 1.0f;
    stream->channel_volume[1] = 1.0f;

    if (!(stream->fmt = clone_wave_format(params->fmt)))
    {
        params->result = E_OUTOFMEMORY;
        goto fail;
    }

    if (FAILED(hr = create_broker_stream(stream, requested_client_buffer_frames)))
    {
        params->result = hr;
        goto fail;
    }

    stream->local_buffer = calloc(stream->client_buffer_frames, stream->client_bytes_per_frame);
    if (!stream->local_buffer)
    {
        params->result = E_OUTOFMEMORY;
        goto fail;
    }

    stream->mix_buffer = calloc(stream->mix_buffer_capacity_frames, sizeof(INT16) * 2);
    if (!stream->mix_buffer)
    {
        params->result = E_OUTOFMEMORY;
        goto fail;
    }

    {
        static const WCHAR name[] =
        {
            'a','u','d','i','o','_','c','l','i','e','n','t','_','t','i','m','e','r',0
        };
        NTSTATUS status = create_unix_thread(&stream->notify_thread, name, notify_thread_main, stream);

        if (status)
        {
            stream->notify_thread = NULL;
            WARN("failed to start notify thread for stream, status %#x\n", status);
        }
    }

    reset_stream_counters_locked(stream);
    *params->channel_count = params->fmt->nChannels;
    *params->stream = (stream_handle)(UINT_PTR)stream;
    return STATUS_SUCCESS;

fail:
    destroy_stream(stream);
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    destroy_stream(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_start(void *args)
{
    struct start_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    int ret;

    ohos_lock(stream);

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_SET);
    if (stream->started)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_NOT_STOPPED);

    ret = ohos_audio_client_start(stream->client, stream->broker_stream.stream_id);
    if (ret != 0)
    {
        WARN("ohos_audio_client_start failed: %d\n", ret);
        return ohos_unlock_result(stream, &params->result,
                                  map_broker_error(ret, AUDCLNT_E_DEVICE_INVALIDATED));
    }

    stream->started = TRUE;
    signal_period_event_locked(stream);
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_stop(void *args)
{
    struct stop_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    int ret;

    ohos_lock(stream);
    if (!stream->started)
        return ohos_unlock_result(stream, &params->result, S_FALSE);

    ret = ohos_audio_client_stop(stream->client, stream->broker_stream.stream_id);
    if (ret != 0)
    {
        WARN("ohos_audio_client_stop failed: %d\n", ret);
        return ohos_unlock_result(stream, &params->result,
                                  map_broker_error(ret, AUDCLNT_E_DEVICE_INVALIDATED));
    }

    stream->started = FALSE;
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_reset(void *args)
{
    struct reset_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    int ret;

    ohos_lock(stream);
    if (stream->started)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_NOT_STOPPED);
    if (stream->locked_frames)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_BUFFER_OPERATION_PENDING);

    ret = ohos_audio_client_reset(stream->client, stream->broker_stream.stream_id);
    if (ret != 0)
    {
        WARN("ohos_audio_client_reset failed: %d\n", ret);
        return ohos_unlock_result(stream, &params->result,
                                  map_broker_error(ret, AUDCLNT_E_DEVICE_INVALIDATED));
    }

    reset_stream_counters_locked(stream);
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    UINT32 requested_mix_frames;
    UINT32 padding_mix_frames;

    ohos_lock(stream);

    if (stream->locked_frames)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);
    if (!params->frames)
    {
        *params->data = NULL;
        return ohos_unlock_result(stream, &params->result, S_OK);
    }

    requested_mix_frames = client_frames_to_mix_frames_ceil(stream, params->frames);
    padding_mix_frames = query_broker_padding_frames_locked(stream);
    if (padding_mix_frames + requested_mix_frames > stream->mix_buffer_frames)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_BUFFER_TOO_LARGE);

    memset(stream->local_buffer, 0, params->frames * stream->client_bytes_per_frame);
    stream->locked_frames = params->frames;
    *params->data = stream->local_buffer;
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    UINT32 mix_frames;
    size_t written;

    ohos_lock(stream);

    if (!stream->locked_frames)
        return ohos_unlock_result(stream, &params->result,
                                  params->written_frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK);
    if (params->written_frames > stream->locked_frames)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_INVALID_SIZE);
    if (!params->written_frames)
    {
        stream->locked_frames = 0;
        return ohos_unlock_result(stream, &params->result, S_OK);
    }

    if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
        memset(stream->local_buffer, 0, params->written_frames * stream->client_bytes_per_frame);

    mix_frames = convert_client_frames_to_mix_locked(stream, stream->local_buffer, params->written_frames,
                                                     stream->mix_buffer, stream->mix_buffer_capacity_frames);
    if (mix_frames == MIX_FRAMES_ERROR)
    {
        stream->locked_frames = 0;
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_BUFFER_ERROR);
    }

    written = ohos_audio_client_write_frames(&stream->broker_stream, stream->mix_buffer, mix_frames);
    if (written != mix_frames)
    {
        stream->locked_frames = 0;
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_BUFFER_TOO_LARGE);
    }

    stream->client_frames_submitted += params->written_frames;
    stream->locked_frames = 0;
    signal_period_event_locked(stream);
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    UINT32 chunk_bytes, chunk_frames;

    ohos_lock(stream);

    if (!is_capture_stream(stream))
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_WRONG_ENDPOINT_TYPE);
    if (stream->locked_frames)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);

    fill_capture_local_buffer_locked(stream);
    *params->frames = 0;
    *params->flags = 0;

    if (stream->capture_held_frames < stream->client_period_frames)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_S_BUFFER_EMPTY);

    chunk_frames = stream->client_buffer_frames - stream->capture_read_offset_frames;
    if (chunk_frames < stream->client_period_frames)
    {
        chunk_bytes = chunk_frames * stream->client_bytes_per_frame;
        if (stream->capture_wrap_buffer_frames < stream->client_period_frames)
        {
            free(stream->capture_wrap_buffer);
            stream->capture_wrap_buffer =
                calloc(stream->client_period_frames, stream->client_bytes_per_frame);
            if (!stream->capture_wrap_buffer)
                return ohos_unlock_result(stream, &params->result, E_OUTOFMEMORY);
            stream->capture_wrap_buffer_frames = stream->client_period_frames;
        }

        *params->data = stream->capture_wrap_buffer;
        memcpy(*params->data,
               stream->local_buffer + stream->capture_read_offset_frames * stream->client_bytes_per_frame,
               chunk_bytes);
        memcpy(*params->data + chunk_bytes, stream->local_buffer,
               stream->client_period_frames * stream->client_bytes_per_frame - chunk_bytes);
    }
    else
    {
        *params->data = stream->local_buffer + stream->capture_read_offset_frames * stream->client_bytes_per_frame;
    }

    stream->locked_frames = *params->frames = stream->client_period_frames;
    if (params->devpos)
        *params->devpos = stream->client_frames_submitted - stream->capture_held_frames;
    if (params->qpcpos)
        *params->qpcpos = query_qpc_100ns();
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);

    if (!is_capture_stream(stream))
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_WRONG_ENDPOINT_TYPE);
    if (!params->done)
    {
        stream->locked_frames = 0;
        return ohos_unlock_result(stream, &params->result, S_OK);
    }
    if (!stream->locked_frames)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_OUT_OF_ORDER);
    if (stream->locked_frames != params->done)
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_INVALID_SIZE);

    stream->capture_held_frames -= params->done;
    stream->capture_read_offset_frames += params->done;
    stream->capture_read_offset_frames %= stream->client_buffer_frames;
    stream->locked_frames = 0;
    signal_period_event_locked(stream);
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;

    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        params->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    else
        params->result = validate_format(params->fmt_in);
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    WAVEFORMATEXTENSIBLE *fmt = params->fmt;

    if (params->flow != eRender && params->flow != eCapture)
    {
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    memset(fmt, 0, sizeof(*fmt));
    fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt->Format.nChannels = 2;
    fmt->Format.nSamplesPerSec = 48000;
    fmt->Format.wBitsPerSample = 16;
    fmt->Format.nBlockAlign = 4;
    fmt->Format.nAvgBytesPerSec = 192000;
    fmt->Format.cbSize = sizeof(*fmt) - sizeof(fmt->Format);
    fmt->Samples.wValidBitsPerSample = 16;
    fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    fmt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = default_period;
    if (params->min_period) *params->min_period = minimum_period;
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);
    *params->frames = stream->client_buffer_frames;
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);
    *params->latency = (stream->mix_period_frames * 10000000ULL) / 48000 + 6666;
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);
    *params->padding = query_client_padding_frames_locked(stream);
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);
    if (is_capture_stream(stream))
    {
        fill_capture_local_buffer_locked(stream);
        *params->frames = stream->capture_held_frames >= stream->client_period_frames ?
            stream->client_period_frames : 0;
    }
    else
    {
        *params->frames = 0;
    }
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);
    *params->freq = stream->fmt->Format.nSamplesPerSec;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq *= stream->client_bytes_per_frame;
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_get_position(void *args)
{
    struct get_position_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    UINT64 padding;
    UINT64 position;

    ohos_lock(stream);
    padding = query_client_padding_frames_locked(stream);
    position = stream->client_frames_submitted >= padding ? stream->client_frames_submitted - padding : 0;
    if (position < stream->last_position)
        position = stream->last_position;
    else
        stream->last_position = position;

    *params->pos = position;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED && !params->device)
        *params->pos *= stream->client_bytes_per_frame;
    ohos_unlock(stream);

    if (params->qpctime)
        *params->qpctime = query_qpc_100ns();

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);
    UINT32 i;

    ohos_lock(stream);
    for (i = 0; i < stream->fmt->Format.nChannels && i < 2; ++i)
        stream->channel_volume[i] = params->master_volume * params->volumes[i] * params->session_volumes[i];
    if (stream->fmt->Format.nChannels == 1)
        stream->channel_volume[1] = stream->channel_volume[0];
    ohos_unlock(stream);
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);
    if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        return ohos_unlock_result(stream, &params->result, AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED);
    if (stream->event)
        return ohos_unlock_result(stream, &params->result, HRESULT_FROM_WIN32(ERROR_INVALID_NAME));

    stream->event = params->event;
    signal_period_event_locked(stream);
    return ohos_unlock_result(stream, &params->result, S_OK);
}

static NTSTATUS ohos_test_connect(void *args)
{
    struct test_connect_params *params = args;

    params->priority = probe_broker_available() ? Priority_Preferred : Priority_Unavailable;
    if (params->priority == Priority_Unavailable)
        ERR("ohos_test_connect marked driver unavailable (audio broker not reachable)\n");
    else
        TRACE("ohos_test_connect marked driver preferred\n");
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_is_started(void *args)
{
    struct is_started_params *params = args;
    struct ohos_stream *stream = handle_get_stream(params->stream);

    ohos_lock(stream);
    return ohos_unlock_result(stream, &params->result, stream->started ? S_OK : S_FALSE);
}

static NTSTATUS ohos_get_prop_value(void *args)
{
    struct get_prop_value_params *params = args;

    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_midi_get_driver(void *args)
{
    return ohos_midi_driver_get(args);
}

static NTSTATUS ohos_midi_init(void *args)
{
    return ohos_midi_driver_init(args);
}

static NTSTATUS ohos_midi_release(void *args)
{
    return ohos_midi_driver_release(args);
}

static NTSTATUS ohos_midi_out_message(void *args)
{
    return ohos_midi_driver_out_message(args);
}

static NTSTATUS ohos_midi_in_message(void *args)
{
    return ohos_midi_driver_in_message(args);
}

static NTSTATUS ohos_midi_notify_wait(void *args)
{
    return ohos_midi_driver_notify_wait(args);
}

static NTSTATUS ohos_aux_message(void *args)
{
    struct aux_message_params *params = args;

    switch (params->msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case AUXDM_GETNUMDEVS:
        *params->err = 0;
        break;
    default:
        *params->err = MMSYSERR_NOTSUPPORTED;
        break;
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    ohos_process_attach,
    ohos_process_detach,
    ohos_not_implemented,
    ohos_not_implemented,
    ohos_get_endpoint_ids,
    ohos_create_stream,
    ohos_release_stream,
    ohos_start,
    ohos_stop,
    ohos_reset,
    ohos_get_render_buffer,
    ohos_release_render_buffer,
    ohos_get_capture_buffer,
    ohos_release_capture_buffer,
    ohos_is_format_supported,
    ohos_not_implemented,
    ohos_get_mix_format,
    ohos_get_device_period,
    ohos_get_buffer_size,
    ohos_get_latency,
    ohos_get_current_padding,
    ohos_get_next_packet_size,
    ohos_get_frequency,
    ohos_get_position,
    ohos_set_volumes,
    ohos_set_event_handle,
    ohos_not_implemented,
    ohos_test_connect,
    ohos_is_started,
    ohos_get_prop_value,
    ohos_midi_get_driver,
    ohos_midi_init,
    ohos_midi_release,
    ohos_midi_out_message,
    ohos_midi_in_message,
    ohos_midi_notify_wait,
    ohos_aux_message,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);
