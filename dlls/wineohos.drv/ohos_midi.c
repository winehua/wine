/*
 * OHOS MIDI soft synth
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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#include "winternl.h"

#include "mmddk.h"

#include "wine/debug.h"

#include "../mmdevapi/unixlib.h"
#include "ohos_audio_client.h"
#include "ohos_midi.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

WINE_DEFAULT_DEBUG_CHANNEL(ohosaudio);

#define OHOS_MIDI_DEVICE_COUNT 1
#define OHOS_MIDI_SAMPLE_RATE 48000u
#define OHOS_MIDI_CHANNELS 2u
#define OHOS_MIDI_PERIOD_FRAMES 480u
#define OHOS_MIDI_BUFFER_FRAMES 4096u
#define OHOS_MIDI_MAX_VOICES 128
#define OHOS_DEFAULT_SOUNDFONT_NAME "winehua-gm.sf2"
#define OHOS_DEFAULT_SOUNDFONT_DIR  "/data/storage/el2/base/files/audio"
#define OHOS_DEFAULT_SOUNDFONT_PATH OHOS_DEFAULT_SOUNDFONT_DIR "/" OHOS_DEFAULT_SOUNDFONT_NAME

struct ohos_midi_dest
{
    MIDIOUTCAPSW caps;
    MIDIOPENDESC midi_desc;
    WORD callback_flags;
    DWORD volume;
    UINT running_status;
    BOOL open;
    BOOL closing;
    BOOL stop_render_thread;
    BOOL render_thread_started;
    pthread_t render_thread;
    OhosAudioClient *client;
    OhosAudioClientStream stream;
    tsf *synth;
    UINT32 render_chunk_frames;
    float *render_float_buffer;
    INT16 *render_s16_buffer;
    float left_gain;
    float right_gain;
};

struct ohos_midi_state
{
    pthread_mutex_t lock;
    pthread_cond_t notify_cond;
    BOOL shutdown;
    struct ohos_midi_dest dest;
};

static struct ohos_midi_state midi_state =
{
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    FALSE,
    {0}
};

static INT16 clamp_s16(float sample)
{
    float scaled = sample * 32767.0f;

    if (scaled > 32767.0f) return 32767;
    if (scaled < -32768.0f) return -32768;
    return (INT16)scaled;
}

static void midi_sleep_msec(unsigned int msec)
{
    usleep(msec * 1000);
}

static void set_out_notify(struct notify_context *notify, struct ohos_midi_dest *dest,
                           WORD dev_id, WORD msg, UINT_PTR param_1, UINT_PTR param_2)
{
    notify->send_notify = TRUE;
    notify->dev_id = dev_id;
    notify->msg = msg;
    notify->param_1 = param_1;
    notify->param_2 = param_2;
    notify->callback = dest->midi_desc.dwCallback;
    notify->flags = dest->callback_flags;
    notify->device = dest->midi_desc.hMidi;
    notify->instance = dest->midi_desc.dwInstance;
}

static void midi_init_caps(struct ohos_midi_dest *dest)
{
    static const WCHAR device_name[] =
    {
        'O','H','O','S',' ','M','I','D','I',' ','S','y','n','t','h',0
    };

    memset(&dest->caps, 0, sizeof(dest->caps));
    dest->caps.wMid = MM_MICROSOFT;
    dest->caps.wPid = MM_MIDI_MAPPER;
    dest->caps.vDriverVersion = 0x0100;
    memcpy(dest->caps.szPname, device_name, sizeof(device_name));
    dest->caps.wTechnology = MOD_SYNTH;
    dest->caps.wVoices = OHOS_MIDI_MAX_VOICES;
    dest->caps.wNotes = 128;
    dest->caps.wChannelMask = 0xffff;
    dest->caps.dwSupport = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
}

static void midi_update_volume_locked(struct ohos_midi_dest *dest, DWORD volume)
{
    dest->volume = volume;
    dest->left_gain = LOWORD(volume) / 65535.0f;
    dest->right_gain = HIWORD(volume) / 65535.0f;
}

static BOOL midi_try_soundfont_path(const char *candidate, char *path, size_t size)
{
    if (!candidate || !candidate[0]) return FALSE;
    if (access(candidate, R_OK) != 0)
    {
        TRACE("MIDI soundfont candidate unreadable: %s (errno=%d)\n",
              debugstr_a(candidate), errno);
        return FALSE;
    }

    if (path && size)
    {
        size_t len = strlen(candidate);
        if (len >= size) len = size - 1;
        memcpy(path, candidate, len);
        path[len] = 0;
    }
    return TRUE;
}

static BOOL midi_try_soundfont_in_dir(const char *dir, const char *name, char *path, size_t size)
{
    char candidate[512];

    if (!dir || !dir[0] || !name || !name[0]) return FALSE;
    snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
    return midi_try_soundfont_path(candidate, path, size);
}

static BOOL midi_try_soundfont_from_wineprefix(char *path, size_t size)
{
    const char *prefix = getenv("WINEPREFIX");
    char parent[512];
    char *slash;

    if (!prefix || !prefix[0]) return FALSE;
    snprintf(parent, sizeof(parent), "%s", prefix);
    slash = strrchr(parent, '/');
    if (!slash || slash == parent) return FALSE;
    *slash = 0;

    if (midi_try_soundfont_in_dir(parent, "audio/" OHOS_DEFAULT_SOUNDFONT_NAME, path, size)) return TRUE;
    if (midi_try_soundfont_in_dir(parent, OHOS_DEFAULT_SOUNDFONT_NAME, path, size)) return TRUE;
    return FALSE;
}

static BOOL midi_get_soundfont_path(char *path, size_t size)
{
    const char *env_path = getenv("MIDI_SOUNDFONT_PATH");

    if (midi_try_soundfont_path(env_path, path, size)) return TRUE;
    if (midi_try_soundfont_path(OHOS_DEFAULT_SOUNDFONT_PATH, path, size)) return TRUE;
    if (midi_try_soundfont_from_wineprefix(path, size)) return TRUE;
    if (midi_try_soundfont_path(OHOS_DEFAULT_SOUNDFONT_NAME, path, size)) return TRUE;
    return FALSE;
}

static UINT midi_out_get_num_devs(void)
{
    return OHOS_MIDI_DEVICE_COUNT;
}

static BOOL midi_initialize_channels_locked(struct ohos_midi_dest *dest)
{
    int channel;

    for (channel = 0; channel < 16; ++channel)
    {
        if (!tsf_channel_set_presetnumber(dest->synth, channel, 0, channel == 9))
            return FALSE;
        tsf_channel_set_pan(dest->synth, channel, 0.5f);
        tsf_channel_set_volume(dest->synth, channel, 1.0f);
        tsf_channel_set_pitchwheel(dest->synth, channel, 8192);
        tsf_channel_set_pitchrange(dest->synth, channel, 2.0f);
        tsf_channel_set_tuning(dest->synth, channel, 0.0f);
        tsf_channel_set_sustain(dest->synth, channel, 0);
    }

    return TRUE;
}

static void midi_reset_synth_locked(struct ohos_midi_dest *dest)
{
    if (!dest->synth) return;
    tsf_reset(dest->synth);
    midi_initialize_channels_locked(dest);
    dest->running_status = 0;
}

static UINT midi_open_audio_stream(struct ohos_midi_dest *dest)
{
    WinehuaAudioOpenStreamReq req;
    int ret;

    memset(&req, 0, sizeof(req));
    memset(&dest->stream, 0, sizeof(dest->stream));
    dest->stream.ring_fd = -1;

    req.sample_rate = OHOS_MIDI_SAMPLE_RATE;
    req.channels = OHOS_MIDI_CHANNELS;
    req.sample_format = WINEHUA_AUDIO_SAMPLE_S16LE;
    req.buffer_frames = OHOS_MIDI_BUFFER_FRAMES;
    req.period_frames = OHOS_MIDI_PERIOD_FRAMES;

    ret = ohos_audio_client_connect(&dest->client);
    if (ret != 0)
    {
        WARN("ohos_audio_client_connect failed: %d\n", ret);
        dest->client = NULL;
        return MMSYSERR_NODRIVER;
    }

    ret = ohos_audio_client_open_stream(dest->client, &req, &dest->stream);
    if (ret != 0)
    {
        WARN("ohos_audio_client_open_stream failed: %d\n", ret);
        ohos_audio_client_disconnect(dest->client);
        dest->client = NULL;
        return MMSYSERR_NODRIVER;
    }

    dest->render_chunk_frames = dest->stream.preferred_period_frames ?
        dest->stream.preferred_period_frames : OHOS_MIDI_PERIOD_FRAMES;
    dest->render_float_buffer = calloc(dest->render_chunk_frames * OHOS_MIDI_CHANNELS,
                                       sizeof(*dest->render_float_buffer));
    dest->render_s16_buffer = calloc(dest->render_chunk_frames * OHOS_MIDI_CHANNELS,
                                     sizeof(*dest->render_s16_buffer));
    if (!dest->render_float_buffer || !dest->render_s16_buffer)
    {
        WARN("failed to allocate MIDI render buffers\n");
        return MMSYSERR_NOMEM;
    }

    ret = ohos_audio_client_start(dest->client, dest->stream.stream_id);
    if (ret != 0)
    {
        WARN("ohos_audio_client_start failed: %d\n", ret);
        return MMSYSERR_NODRIVER;
    }

    return MMSYSERR_NOERROR;
}

static void midi_release_audio_resources(struct ohos_midi_dest *dest)
{
    if (dest->client)
    {
        if (dest->stream.stream_id) ohos_audio_client_stop(dest->client, dest->stream.stream_id);
        if (dest->stream.stream_id) ohos_audio_client_close_stream(dest->client, &dest->stream);
        ohos_audio_client_disconnect(dest->client);
        dest->client = NULL;
    }

    free(dest->render_float_buffer);
    free(dest->render_s16_buffer);
    dest->render_float_buffer = NULL;
    dest->render_s16_buffer = NULL;
    dest->render_chunk_frames = 0;

    if (dest->synth)
    {
        tsf_close(dest->synth);
        dest->synth = NULL;
    }

    memset(&dest->stream, 0, sizeof(dest->stream));
    dest->stream.ring_fd = -1;
}

static void *midi_render_thread_main(void *arg)
{
    struct ohos_midi_dest *dest = arg;

    while (1)
    {
        UINT32 frames_to_render;
        UINT32 i;
        size_t free_frames;

        pthread_mutex_lock(&midi_state.lock);
        if (dest->stop_render_thread)
        {
            pthread_mutex_unlock(&midi_state.lock);
            break;
        }

        free_frames = ohos_audio_client_get_free_frames(&dest->stream);
        if (!free_frames)
        {
            pthread_mutex_unlock(&midi_state.lock);
            midi_sleep_msec(2);
            continue;
        }

        frames_to_render = free_frames > dest->render_chunk_frames ?
            dest->render_chunk_frames : (UINT32)free_frames;
        tsf_render_float(dest->synth, dest->render_float_buffer, frames_to_render, 0);

        for (i = 0; i < frames_to_render; ++i)
        {
            float left = dest->render_float_buffer[i * 2] * dest->left_gain;
            float right = dest->render_float_buffer[i * 2 + 1] * dest->right_gain;

            dest->render_s16_buffer[i * 2] = clamp_s16(left);
            dest->render_s16_buffer[i * 2 + 1] = clamp_s16(right);
        }
        pthread_mutex_unlock(&midi_state.lock);

        if (ohos_audio_client_write_frames(&dest->stream, dest->render_s16_buffer, frames_to_render) !=
            frames_to_render)
            midi_sleep_msec(2);
    }

    return NULL;
}

static UINT midi_out_get_devcaps(WORD dev_id, MIDIOUTCAPSW *caps, UINT size)
{
    if (!caps) return MMSYSERR_INVALPARAM;
    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;

    pthread_mutex_lock(&midi_state.lock);
    midi_init_caps(&midi_state.dest);
    memcpy(caps, &midi_state.dest.caps, size < sizeof(*caps) ? size : sizeof(*caps));
    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_prepare(WORD dev_id, MIDIHDR *hdr, UINT hdr_size)
{
    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;
    if (hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (hdr->dwFlags & MHDR_PREPARED) return MMSYSERR_NOERROR;

    hdr->lpNext = 0;
    hdr->dwFlags |= MHDR_PREPARED;
    hdr->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_unprepare(WORD dev_id, MIDIHDR *hdr, UINT hdr_size)
{
    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;
    if (hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED)) return MMSYSERR_NOERROR;
    if (hdr->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;

    hdr->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static UINT midi_out_get_volume(UINT *volume)
{
    if (!volume) return MMSYSERR_INVALPARAM;

    pthread_mutex_lock(&midi_state.lock);
    *volume = midi_state.dest.volume ? midi_state.dest.volume : 0xffffffffu;
    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_set_volume(DWORD volume)
{
    pthread_mutex_lock(&midi_state.lock);
    if (!midi_state.dest.volume) midi_state.dest.volume = 0xffffffffu;
    midi_update_volume_locked(&midi_state.dest, volume);
    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_open(WORD dev_id, MIDIOPENDESC *midi_desc, UINT flags,
                          struct notify_context *notify)
{
    struct ohos_midi_dest *dest = &midi_state.dest;
    char soundfont_path[512];
    UINT err = MMSYSERR_NOERROR;

    if (!midi_desc) return MMSYSERR_INVALPARAM;
    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;
    if ((flags & ~(CALLBACK_TYPEMASK | MIDI_IO_STATUS)) != 0) return MMSYSERR_INVALFLAG;
    if (!midi_get_soundfont_path(soundfont_path, sizeof(soundfont_path)))
    {
        WARN("no readable MIDI soundfont found (env=%s, WINEPREFIX=%s)\n",
             debugstr_a(getenv("MIDI_SOUNDFONT_PATH")), debugstr_a(getenv("WINEPREFIX")));
        return MMSYSERR_NOTENABLED;
    }

    pthread_mutex_lock(&midi_state.lock);
    midi_init_caps(dest);
    if (dest->open || dest->closing)
    {
        pthread_mutex_unlock(&midi_state.lock);
        return MMSYSERR_ALLOCATED;
    }

    midi_update_volume_locked(dest, dest->volume ? dest->volume : 0xffffffffu);
    dest->running_status = 0;

    dest->synth = tsf_load_filename(soundfont_path);
    if (!dest->synth)
    {
        WARN("failed to load soundfont %s\n", debugstr_a(soundfont_path));
        err = MMSYSERR_NOTENABLED;
        goto fail;
    }

    tsf_set_output(dest->synth, TSF_STEREO_INTERLEAVED, OHOS_MIDI_SAMPLE_RATE, 0.0f);
    if (!tsf_set_max_voices(dest->synth, OHOS_MIDI_MAX_VOICES) ||
        !midi_initialize_channels_locked(dest))
    {
        WARN("failed to initialize TinySoundFont channels\n");
        err = MMSYSERR_NOMEM;
        goto fail;
    }

    if ((err = midi_open_audio_stream(dest)) != MMSYSERR_NOERROR)
        goto fail;

    dest->callback_flags = HIWORD(flags & CALLBACK_TYPEMASK);
    dest->midi_desc = *midi_desc;
    dest->stop_render_thread = FALSE;
    dest->closing = FALSE;
    dest->open = TRUE;

    if (pthread_create(&dest->render_thread, NULL, midi_render_thread_main, dest) != 0)
    {
        WARN("failed to create MIDI render thread\n");
        err = MMSYSERR_NOMEM;
        dest->open = FALSE;
        goto fail;
    }

    dest->render_thread_started = TRUE;
    set_out_notify(notify, dest, dev_id, MOM_OPEN, 0, 0);
    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;

fail:
    midi_release_audio_resources(dest);
    memset(&dest->midi_desc, 0, sizeof(dest->midi_desc));
    dest->open = FALSE;
    dest->closing = FALSE;
    pthread_mutex_unlock(&midi_state.lock);
    return err;
}

static UINT midi_out_close(WORD dev_id, struct notify_context *notify)
{
    struct ohos_midi_dest *dest = &midi_state.dest;
    pthread_t render_thread;
    BOOL join_thread = FALSE;

    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;

    pthread_mutex_lock(&midi_state.lock);
    if (!dest->open || dest->closing)
    {
        pthread_mutex_unlock(&midi_state.lock);
        return MMSYSERR_ERROR;
    }

    set_out_notify(notify, dest, dev_id, MOM_CLOSE, 0, 0);
    dest->open = FALSE;
    dest->closing = TRUE;
    dest->stop_render_thread = TRUE;
    if (dest->render_thread_started)
    {
        render_thread = dest->render_thread;
        join_thread = TRUE;
        dest->render_thread_started = FALSE;
    }
    pthread_mutex_unlock(&midi_state.lock);

    if (join_thread) pthread_join(render_thread, NULL);

    pthread_mutex_lock(&midi_state.lock);
    midi_release_audio_resources(dest);
    memset(&dest->midi_desc, 0, sizeof(dest->midi_desc));
    dest->closing = FALSE;
    dest->running_status = 0;
    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_reset(WORD dev_id)
{
    struct ohos_midi_dest *dest = &midi_state.dest;

    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;

    pthread_mutex_lock(&midi_state.lock);
    if (!dest->open || dest->closing)
    {
        pthread_mutex_unlock(&midi_state.lock);
        return MIDIERR_NODEVICE;
    }

    midi_reset_synth_locked(dest);
    if (dest->client) ohos_audio_client_reset(dest->client, dest->stream.stream_id);
    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;
}

static void midi_handle_sysex_locked(struct ohos_midi_dest *dest, const BYTE *data, UINT length)
{
    if (length < 2 || data[0] != 0xf0 || data[length - 1] != 0xf7) return;

    if (length >= 6 && data[1] == 0x7e && data[3] == 0x09 &&
        (data[4] == 0x01 || data[4] == 0x03))
    {
        midi_reset_synth_locked(dest);
        return;
    }

    if (length >= 11 && data[1] == 0x41 && data[3] == 0x42 && data[4] == 0x12 &&
        data[5] == 0x40 && data[6] == 0x00 && data[7] == 0x7f && data[8] == 0x00)
    {
        midi_reset_synth_locked(dest);
        return;
    }

    if (length >= 9 && data[1] == 0x43 && data[2] == 0x10 && data[3] == 0x4c &&
        data[4] == 0x00 && data[5] == 0x00 && data[6] == 0x7e && data[7] == 0x00)
    {
        midi_reset_synth_locked(dest);
        return;
    }
}

static UINT midi_out_long_data(WORD dev_id, MIDIHDR *hdr, UINT hdr_size, struct notify_context *notify)
{
    struct ohos_midi_dest *dest = &midi_state.dest;

    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;
    if (hdr_size < offsetof(MIDIHDR, dwOffset) || !hdr || !hdr->lpData)
        return MMSYSERR_INVALPARAM;
    if (!(hdr->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
    if (hdr->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;

    pthread_mutex_lock(&midi_state.lock);
    if (!dest->open || dest->closing)
    {
        pthread_mutex_unlock(&midi_state.lock);
        return MIDIERR_NODEVICE;
    }

    hdr->dwFlags &= ~MHDR_DONE;
    hdr->dwFlags |= MHDR_INQUEUE;

    midi_handle_sysex_locked(dest, (const BYTE *)hdr->lpData, hdr->dwBufferLength);
    dest->running_status = 0;

    hdr->dwFlags &= ~MHDR_INQUEUE;
    hdr->dwFlags |= MHDR_DONE;
    set_out_notify(notify, dest, dev_id, MOM_DONE, (UINT_PTR)hdr, 0);
    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;
}

static UINT midi_out_data(WORD dev_id, UINT data)
{
    struct ohos_midi_dest *dest = &midi_state.dest;
    BYTE status = LOBYTE(LOWORD(data));
    BYTE data1 = HIBYTE(LOWORD(data));
    BYTE data2 = LOBYTE(HIWORD(data));
    BYTE channel;
    UINT pitch;

    if (dev_id >= OHOS_MIDI_DEVICE_COUNT) return MMSYSERR_BADDEVICEID;

    pthread_mutex_lock(&midi_state.lock);
    if (!dest->open || dest->closing)
    {
        pthread_mutex_unlock(&midi_state.lock);
        return MIDIERR_NODEVICE;
    }

    if (status & 0x80)
    {
        if (status < 0xf0)
            dest->running_status = status;
        else if (status <= 0xf7)
            dest->running_status = 0;
    }
    else if (dest->running_status)
    {
        status = (BYTE)dest->running_status;
        data2 = data1;
        data1 = LOBYTE(LOWORD(data));
    }
    else
    {
        pthread_mutex_unlock(&midi_state.lock);
        return MMSYSERR_NOERROR;
    }

    channel = status & 0x0f;
    switch (status & 0xf0)
    {
    case 0x80:
        tsf_channel_note_off(dest->synth, channel, data1);
        break;
    case 0x90:
        if (data2)
            tsf_channel_note_on(dest->synth, channel, data1, data2 / 127.0f);
        else
            tsf_channel_note_off(dest->synth, channel, data1);
        break;
    case 0xa0:
        break;
    case 0xb0:
        tsf_channel_midi_control(dest->synth, channel, data1, data2);
        break;
    case 0xc0:
        tsf_channel_set_presetnumber(dest->synth, channel, data1, channel == 9);
        break;
    case 0xd0:
        break;
    case 0xe0:
        pitch = ((UINT)data2 << 7) | data1;
        tsf_channel_set_pitchwheel(dest->synth, channel, pitch);
        break;
    default:
        switch (status)
        {
        case 0xff:
            midi_reset_synth_locked(dest);
            if (dest->client) ohos_audio_client_reset(dest->client, dest->stream.stream_id);
            break;
        default:
            break;
        }
        break;
    }

    pthread_mutex_unlock(&midi_state.lock);
    return MMSYSERR_NOERROR;
}

NTSTATUS ohos_midi_driver_get(void *args)
{
    ((WCHAR *)args)[0] = 0;
    return STATUS_SUCCESS;
}

NTSTATUS ohos_midi_driver_init(void *args)
{
    struct midi_init_params *params = args;

    pthread_mutex_lock(&midi_state.lock);
    midi_init_caps(&midi_state.dest);
    if (!midi_state.dest.volume) midi_update_volume_locked(&midi_state.dest, 0xffffffffu);
    midi_state.shutdown = FALSE;
    pthread_mutex_unlock(&midi_state.lock);

    /* mmdevapi DriverProc(DRV_LOAD) only treats DRV_SUCCESS as a successful MIDI driver load. */
    *params->err = DRV_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS ohos_midi_driver_release(void *args)
{
    struct ohos_midi_dest *dest = &midi_state.dest;
    pthread_t render_thread;
    BOOL join_thread = FALSE;

    pthread_mutex_lock(&midi_state.lock);
    midi_state.shutdown = TRUE;
    pthread_cond_broadcast(&midi_state.notify_cond);

    dest->stop_render_thread = TRUE;
    if (dest->render_thread_started)
    {
        render_thread = dest->render_thread;
        join_thread = TRUE;
        dest->render_thread_started = FALSE;
    }
    pthread_mutex_unlock(&midi_state.lock);

    if (join_thread) pthread_join(render_thread, NULL);

    pthread_mutex_lock(&midi_state.lock);
    midi_release_audio_resources(dest);
    memset(&dest->midi_desc, 0, sizeof(dest->midi_desc));
    dest->open = FALSE;
    dest->closing = FALSE;
    dest->running_status = 0;
    pthread_mutex_unlock(&midi_state.lock);

    return STATUS_SUCCESS;
}

NTSTATUS ohos_midi_driver_out_message(void *args)
{
    struct midi_out_message_params *params = args;

    params->notify->send_notify = FALSE;

    switch (params->msg)
    {
    case DRVM_INIT:
        *params->err = MMSYSERR_NOERROR;
        break;
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case MODM_OPEN:
        *params->err = midi_out_open(params->dev_id, (MIDIOPENDESC *)params->param_1,
                                     params->param_2, params->notify);
        break;
    case MODM_CLOSE:
        *params->err = midi_out_close(params->dev_id, params->notify);
        break;
    case MODM_DATA:
        *params->err = midi_out_data(params->dev_id, params->param_1);
        break;
    case MODM_LONGDATA:
        *params->err = midi_out_long_data(params->dev_id, (MIDIHDR *)params->param_1,
                                          params->param_2, params->notify);
        break;
    case MODM_PREPARE:
        *params->err = midi_out_prepare(params->dev_id, (MIDIHDR *)params->param_1, params->param_2);
        break;
    case MODM_UNPREPARE:
        *params->err = midi_out_unprepare(params->dev_id, (MIDIHDR *)params->param_1, params->param_2);
        break;
    case MODM_GETDEVCAPS:
        *params->err = midi_out_get_devcaps(params->dev_id, (MIDIOUTCAPSW *)params->param_1, params->param_2);
        break;
    case MODM_GETNUMDEVS:
        *params->err = midi_out_get_num_devs();
        break;
    case MODM_GETVOLUME:
        *params->err = midi_out_get_volume((UINT *)params->param_1);
        break;
    case MODM_SETVOLUME:
        *params->err = midi_out_set_volume(params->param_1);
        break;
    case MODM_RESET:
        *params->err = midi_out_reset(params->dev_id);
        break;
    default:
        *params->err = MMSYSERR_NOTSUPPORTED;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS ohos_midi_driver_in_message(void *args)
{
    struct midi_in_message_params *params = args;

    params->notify->send_notify = FALSE;

    switch (params->msg)
    {
    case DRVM_INIT:
    case DRVM_EXIT:
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        *params->err = MMSYSERR_NOERROR;
        break;
    case MIDM_GETNUMDEVS:
        *params->err = 0;
        break;
    default:
        *params->err = MMSYSERR_NOTSUPPORTED;
        break;
    }
    return STATUS_SUCCESS;
}

NTSTATUS ohos_midi_driver_notify_wait(void *args)
{
    struct midi_notify_wait_params *params = args;

    params->notify->send_notify = FALSE;

    pthread_mutex_lock(&midi_state.lock);
    while (!midi_state.shutdown)
        pthread_cond_wait(&midi_state.notify_cond, &midi_state.lock);
    *params->quit = TRUE;
    pthread_mutex_unlock(&midi_state.lock);

    return STATUS_SUCCESS;
}
