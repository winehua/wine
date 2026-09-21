/*
 * WaveOut backend for MCI QTZ driver (OHOS)
 *
 * Provides MP3/WAV playback via minimp3 software decoding +
 * WinMM waveOut API, as an alternative to the DShow backend
 * which is not available on OHOS.
 *
 * Copyright 2025-2026 WineHua contributors
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


#include <stdarg.h>
#include <stdbool.h>
#include <math.h>
#include <limits.h>
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "mmsystem.h"
#include "mmddk.h"
#include "wine/debug.h"
#include "mciqtz_private.h"
#include "digitalv.h"
#include "wownt32.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

WINE_DEFAULT_DEBUG_CHANNEL(mciqtz);

/*======================================================================*
 *                     Internal helpers                                  *
 *======================================================================*/

static DWORD waveout_frames_to_time(const WINE_MCIQTZ *wma, DWORD frames)
{
    if (!wma->wave_format.nSamplesPerSec) return 0;
    if (wma->time_format == MCI_FORMAT_FRAMES) return frames;
    return (DWORD)(((ULONGLONG)frames * 1000) / wma->wave_format.nSamplesPerSec);
}

static DWORD waveout_time_to_frames(const WINE_MCIQTZ *wma, DWORD value)
{
    if (!wma->wave_format.nSamplesPerSec) return 0;
    if (wma->time_format == MCI_FORMAT_FRAMES) return value;
    return (DWORD)(((ULONGLONG)value * wma->wave_format.nSamplesPerSec) / 1000);
}

static DWORD waveout_get_position_frames(const WINE_MCIQTZ *wma)
{
    MMTIME mmtime;
    DWORD frames = 0;

    if (!wma->wave_out) return 0;

    memset(&mmtime, 0, sizeof(mmtime));
    mmtime.wType = TIME_SAMPLES;
    if (waveOutGetPosition(wma->wave_out, &mmtime, sizeof(mmtime)) == MMSYSERR_NOERROR)
    {
        if (mmtime.wType == TIME_SAMPLES)
            frames = mmtime.u.sample;
        else if (mmtime.wType == TIME_BYTES && wma->wave_format.nBlockAlign)
            frames = mmtime.u.cb / wma->wave_format.nBlockAlign;
    }

    return frames;
}

static DWORD waveout_playback_position_frames(const WINE_MCIQTZ *wma)
{
    DWORD frames = wma->wave_position_frames;

    if (wma->wave_out && (wma->wave_state == MCIQTZ_WAVE_PLAYING ||
        wma->wave_state == MCIQTZ_WAVE_PAUSED))
        frames = wma->wave_play_start_frames + waveout_get_position_frames(wma);

    if (frames > wma->wave_play_stop_frames)
        frames = wma->wave_play_stop_frames;
    if (frames > wma->wave_total_frames)
        frames = wma->wave_total_frames;
    return frames;
}

static void waveout_close_device(WINE_MCIQTZ *wma, BOOL reset_device)
{
    if (wma->wave_out)
    {
        if (reset_device)
            waveOutReset(wma->wave_out);
        if (wma->wave_header_prepared)
        {
            waveOutUnprepareHeader(wma->wave_out, &wma->wave_header, sizeof(wma->wave_header));
            wma->wave_header_prepared = FALSE;
        }
        waveOutClose(wma->wave_out);
        wma->wave_out = NULL;
    }
}

static BOOL waveout_has_extension(LPCWSTR path, LPCWSTR extension)
{
    const WCHAR *suffix;

    if (!path || !extension) return FALSE;
    suffix = wcsrchr(path, '.');
    return suffix && !lstrcmpiW(suffix, extension);
}

static DWORD waveout_load_file_bytes(LPCWSTR path, BYTE **out_data, DWORD *out_size)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    BYTE *data = NULL;
    DWORD bytes_read = 0;
    DWORD ret = MCIERR_INVALID_FILE;

    if (!out_data || !out_size) return MCIERR_INVALID_FILE;
    *out_data = NULL;
    *out_size = 0;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? MCIERR_FILE_NOT_FOUND : MCIERR_INVALID_FILE;

    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > INT_MAX)
        goto done;

    data = HeapAlloc(GetProcessHeap(), 0, size.QuadPart);
    if (!data)
    {
        ret = MCIERR_OUT_OF_MEMORY;
        goto done;
    }

    if (!ReadFile(file, data, size.QuadPart, &bytes_read, NULL) || bytes_read != size.QuadPart)
        goto done;

    *out_data = data;
    *out_size = bytes_read;
    data = NULL;
    ret = 0;

done:
    HeapFree(GetProcessHeap(), 0, data);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ret;
}

static DWORD waveout_decode_wav_file(LPCWSTR path, WAVEFORMATEX *format,
                                     BYTE **out_pcm, DWORD *out_bytes, DWORD *out_frames)
{
    BYTE *file_bytes = NULL, *pcm = NULL;
    DWORD file_size = 0, offset, data_offset = 0, data_size = 0;
    DWORD fmt_offset = 0, fmt_size = 0, output_bytes, i;
    WORD channels, bits_per_sample, format_tag;
    DWORD sample_rate;
    DWORD ret;

    ret = waveout_load_file_bytes(path, &file_bytes, &file_size);
    if (ret) return ret;

    if (file_size < 44 || memcmp(file_bytes, "RIFF", 4) || memcmp(file_bytes + 8, "WAVE", 4))
    {
        ret = MCIERR_INVALID_FILE;
        goto done;
    }

    for (offset = 12; offset + 8 <= file_size; )
    {
        DWORD chunk_size = file_bytes[offset + 4] | (file_bytes[offset + 5] << 8) |
                           (file_bytes[offset + 6] << 16) | (file_bytes[offset + 7] << 24);
        DWORD next = offset + 8 + chunk_size + (chunk_size & 1);

        if (next < offset + 8 || next > file_size) break;
        if (!memcmp(file_bytes + offset, "fmt ", 4) && chunk_size >= 16 && !fmt_offset)
        {
            fmt_offset = offset + 8;
            fmt_size = chunk_size;
        }
        else if (!memcmp(file_bytes + offset, "data", 4) && !data_offset)
        {
            data_offset = offset + 8;
            data_size = chunk_size;
        }
        offset = next;
    }

    if (!fmt_offset || !data_offset || data_offset + data_size > file_size)
    {
        ret = MCIERR_INVALID_FILE;
        goto done;
    }

    format_tag = file_bytes[fmt_offset] | (file_bytes[fmt_offset + 1] << 8);
    channels = file_bytes[fmt_offset + 2] | (file_bytes[fmt_offset + 3] << 8);
    sample_rate = file_bytes[fmt_offset + 4] | (file_bytes[fmt_offset + 5] << 8) |
                  (file_bytes[fmt_offset + 6] << 16) | (file_bytes[fmt_offset + 7] << 24);
    bits_per_sample = file_bytes[fmt_offset + 14] | (file_bytes[fmt_offset + 15] << 8);

    if (format_tag != WAVE_FORMAT_PCM || (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16))
    {
        ret = MCIERR_INVALID_FILE;
        goto done;
    }

    output_bytes = bits_per_sample == 16 ? data_size : data_size * sizeof(INT16);
    pcm = HeapAlloc(GetProcessHeap(), 0, output_bytes);
    if (!pcm)
    {
        ret = MCIERR_OUT_OF_MEMORY;
        goto done;
    }

    if (bits_per_sample == 16)
        memcpy(pcm, file_bytes + data_offset, data_size);
    else
    {
        INT16 *dst = (INT16 *)pcm;
        const BYTE *src = file_bytes + data_offset;

        for (i = 0; i < data_size; ++i)
            dst[i] = ((INT16)src[i] - 128) << 8;
    }

    memset(format, 0, sizeof(*format));
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = channels;
    format->nSamplesPerSec = sample_rate;
    format->wBitsPerSample = 16;
    format->nBlockAlign = channels * sizeof(INT16);
    format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
    *out_pcm = pcm;
    *out_bytes = output_bytes - (output_bytes % format->nBlockAlign);
    *out_frames = *out_bytes / format->nBlockAlign;
    pcm = NULL;
    ret = 0;

done:
    HeapFree(GetProcessHeap(), 0, pcm);
    HeapFree(GetProcessHeap(), 0, file_bytes);
    return ret;
}

static DWORD waveout_decode_mp3_file(LPCWSTR path, WAVEFORMATEX *format,
                                     BYTE **out_pcm, DWORD *out_bytes, DWORD *out_frames)
{
    BYTE *file_bytes = NULL;
    DWORD file_size = 0;
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    BYTE *cursor;
    int remaining;
    int channels = 0, sample_rate = 0;
    size_t total_samples = 0, decoded_samples = 0;
    INT16 *pcm = NULL;
    DWORD ret;

    ret = waveout_load_file_bytes(path, &file_bytes, &file_size);
    if (ret) return ret;

    mp3dec_init(&decoder);
    cursor = file_bytes;
    remaining = file_size;
    while (remaining > 0)
    {
        INT16 frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
        int samples = mp3dec_decode_frame(&decoder, cursor, remaining, frame, &info);

        if (info.frame_bytes <= 0) break;
        if (samples > 0)
        {
            if (!channels)
            {
                channels = info.channels;
                sample_rate = info.hz;
            }
            else if (channels != info.channels || sample_rate != info.hz)
            {
                ret = MCIERR_INVALID_FILE;
                goto done;
            }
            total_samples += (size_t)samples * channels;
        }

        cursor += info.frame_bytes;
        remaining -= info.frame_bytes;
    }

    if (!channels || !sample_rate || total_samples == 0 || (channels != 1 && channels != 2))
    {
        ret = MCIERR_INVALID_FILE;
        goto done;
    }

    pcm = HeapAlloc(GetProcessHeap(), 0, total_samples * sizeof(*pcm));
    if (!pcm)
    {
        ret = MCIERR_OUT_OF_MEMORY;
        goto done;
    }

    mp3dec_init(&decoder);
    cursor = file_bytes;
    remaining = file_size;
    while (remaining > 0)
    {
        int samples = mp3dec_decode_frame(&decoder, cursor, remaining, pcm + decoded_samples, &info);

        if (info.frame_bytes <= 0) break;
        if (samples > 0)
            decoded_samples += (size_t)samples * channels;

        cursor += info.frame_bytes;
        remaining -= info.frame_bytes;
    }

    memset(format, 0, sizeof(*format));
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = channels;
    format->nSamplesPerSec = sample_rate;
    format->wBitsPerSample = 16;
    format->nBlockAlign = channels * sizeof(INT16);
    format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
    *out_pcm = (BYTE *)pcm;
    *out_bytes = decoded_samples * sizeof(*pcm);
    *out_frames = decoded_samples / channels;
    pcm = NULL;
    ret = 0;

done:
    HeapFree(GetProcessHeap(), 0, pcm);
    HeapFree(GetProcessHeap(), 0, file_bytes);
    return ret;
}

static DWORD CALLBACK waveout_notify_thread(LPVOID parm)
{
    WINE_MCIQTZ *wma = parm;
    HANDLE waits[2];
    DWORD notify_status = 0;
    DWORD ret = 0;

    waits[0] = wma->stop_event;
    waits[1] = wma->wave_done_event;

    while (wma->wave_play_start_frames < wma->wave_play_stop_frames)
    {
        DWORD wait_result;
        DWORD buffer_frames = wma->wave_play_stop_frames - wma->wave_play_start_frames;
        MMRESULT mmr;

        ResetEvent(wma->wave_done_event);

        mmr = waveOutOpen(&wma->wave_out, WAVE_MAPPER, &wma->wave_format,
                          (DWORD_PTR)wma->wave_done_event, 0, CALLBACK_EVENT);
        if (mmr != MMSYSERR_NOERROR)
        {
            WARN("waveOutOpen failed: %u\n", mmr);
            ret = MCIERR_INTERNAL;
            break;
        }

        waveOutSetVolume(wma->wave_out, wma->wave_volume);
        memset(&wma->wave_header, 0, sizeof(wma->wave_header));
        wma->wave_header.lpData = (LPSTR)(wma->wave_pcm +
            wma->wave_play_start_frames * wma->wave_format.nBlockAlign);
        wma->wave_header.dwBufferLength = buffer_frames * wma->wave_format.nBlockAlign;
        mmr = waveOutPrepareHeader(wma->wave_out, &wma->wave_header, sizeof(wma->wave_header));
        if (mmr != MMSYSERR_NOERROR)
        {
            WARN("waveOutPrepareHeader failed: %u\n", mmr);
            waveout_close_device(wma, TRUE);
            ret = MCIERR_INTERNAL;
            break;
        }

        wma->wave_header_prepared = TRUE;
        mmr = waveOutWrite(wma->wave_out, &wma->wave_header, sizeof(wma->wave_header));
        if (mmr != MMSYSERR_NOERROR)
        {
            WARN("waveOutWrite failed: %u\n", mmr);
            waveout_close_device(wma, TRUE);
            ret = MCIERR_INTERNAL;
            break;
        }

        wma->wave_position_frames = wma->wave_play_start_frames;
        wma->wave_state = MCIQTZ_WAVE_PLAYING;

        wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0)
        {
            DWORD played = waveout_get_position_frames(wma);
            wma->wave_position_frames = min(wma->wave_play_start_frames + played, wma->wave_play_stop_frames);
            notify_status = MCI_NOTIFY_ABORTED;
            waveout_close_device(wma, TRUE);
            break;
        }

        if (wait_result != WAIT_OBJECT_0 + 1)
        {
            WARN("Unexpected wave wait result %#lx\n", wait_result);
            ret = MCIERR_INTERNAL;
            waveout_close_device(wma, TRUE);
            break;
        }

        wma->wave_position_frames = wma->wave_play_stop_frames;
        waveout_close_device(wma, FALSE);
        if (wma->mci_flags & MCI_DGV_PLAY_REPEAT)
        {
            wma->wave_play_start_frames = wma->wave_loop_start_frames;
            continue;
        }

        notify_status = MCI_NOTIFY_SUCCESSFUL;
        break;
    }

    wma->wave_state = MCIQTZ_WAVE_STOPPED;
    if (notify_status)
    {
        HANDLE old = InterlockedExchangePointer(&wma->callback, NULL);
        if (old)
            mciDriverNotify(old, wma->notify_devid, notify_status);
    }

    return ret;
}

/*======================================================================*
 *                     Public entry points                               *
 *======================================================================*/

BOOL MCIQTZ_waveout_is_candidate(LPCWSTR path)
{
    static const WCHAR ext_mp3[] = L".mp3";
    static const WCHAR ext_wav[] = L".wav";

    return waveout_has_extension(path, ext_mp3) || waveout_has_extension(path, ext_wav);
}

DWORD MCIQTZ_waveout_open(WINE_MCIQTZ *wma, DWORD flags,
                          const MCI_DGV_OPEN_PARMSW *params)
{
    BYTE *pcm = NULL;
    DWORD pcm_bytes = 0, frames = 0;
    DWORD ret;

    if (waveout_has_extension(params->lpstrElementName, L".wav"))
        ret = waveout_decode_wav_file(params->lpstrElementName, &wma->wave_format, &pcm, &pcm_bytes, &frames);
    else if (waveout_has_extension(params->lpstrElementName, L".mp3"))
        ret = waveout_decode_mp3_file(params->lpstrElementName, &wma->wave_format, &pcm, &pcm_bytes, &frames);
    else
        return MCIERR_INVALID_FILE;

    if (ret) return ret;

    wma->wave_done_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!wma->wave_done_event)
    {
        HeapFree(GetProcessHeap(), 0, pcm);
        return MCIERR_OUT_OF_MEMORY;
    }

    wma->wave_pcm = pcm;
    wma->wave_pcm_bytes = pcm_bytes;
    wma->wave_total_frames = frames;
    wma->wave_position_frames = 0;
    wma->wave_play_start_frames = 0;
    wma->wave_loop_start_frames = 0;
    wma->wave_play_stop_frames = frames;
    wma->wave_state = MCIQTZ_WAVE_STOPPED;
    wma->backend = MCIQTZ_BACKEND_WAVEOUT;
    wma->opened = TRUE;

    if (flags & MCI_NOTIFY)
        mciDriverNotify(HWND_32(LOWORD(params->dwCallback)), wma->wDevID, MCI_NOTIFY_SUCCESSFUL);

    TRACE("using waveOut fallback for %s, rate=%lu channels=%u frames=%lu\n",
          debugstr_w(params->lpstrElementName), wma->wave_format.nSamplesPerSec,
          wma->wave_format.nChannels, frames);
    return 0;
}

DWORD MCIQTZ_waveout_play(WINE_MCIQTZ *wma, DWORD flags, LPMCI_PLAY_PARMS lpParms)
{
    DWORD start, stop;

    start = (flags & MCI_FROM) ? waveout_time_to_frames(wma, lpParms->dwFrom) : wma->wave_position_frames;
    if (!(flags & MCI_FROM) && start >= wma->wave_total_frames)
        start = 0;
    stop = (flags & MCI_TO) ? waveout_time_to_frames(wma, lpParms->dwTo) : wma->wave_total_frames;

    start = min(start, wma->wave_total_frames);
    stop = min(stop, wma->wave_total_frames);
    if (stop < start)
        stop = start;

    wma->wave_position_frames = start;
    wma->wave_play_start_frames = start;
    wma->wave_loop_start_frames = start;
    wma->wave_play_stop_frames = stop;
    wma->wave_state = MCIQTZ_WAVE_STOPPED;

    if (start < stop)
    {
        wma->thread = CreateThread(NULL, 0, waveout_notify_thread, wma, 0, NULL);
        if (!wma->thread)
        {
            TRACE("Can't create thread\n");
            return MCIERR_INTERNAL;
        }
    }

    return 0;
}

DWORD MCIQTZ_waveout_seek(WINE_MCIQTZ *wma, DWORD flags, LPMCI_SEEK_PARMS lpParms)
{
    if (flags & MCI_SEEK_TO_START)
        wma->wave_position_frames = 0;
    else if (flags & MCI_SEEK_TO_END)
        wma->wave_position_frames = wma->wave_total_frames;
    else if (flags & MCI_TO)
        wma->wave_position_frames = min(waveout_time_to_frames(wma, lpParms->dwTo), wma->wave_total_frames);
    else
    {
        WARN("dwFlag doesn't tell where to seek to...\n");
        return MCIERR_MISSING_PARAMETER;
    }

    return 0;
}

void MCIQTZ_waveout_pos_sync(WINE_MCIQTZ *wma)
{
    if (wma->wave_state != MCIQTZ_WAVE_STOPPED)
        wma->wave_position_frames = waveout_playback_position_frames(wma);
}

void MCIQTZ_waveout_state_reset(WINE_MCIQTZ *wma)
{
    wma->wave_state = MCIQTZ_WAVE_STOPPED;
}

void MCIQTZ_waveout_close(WINE_MCIQTZ *wma)
{
    waveout_close_device(wma, TRUE);

    if (wma->wave_done_event)
    {
        CloseHandle(wma->wave_done_event);
        wma->wave_done_event = NULL;
    }

    HeapFree(GetProcessHeap(), 0, wma->wave_pcm);
    wma->wave_pcm = NULL;
    memset(&wma->wave_format, 0, sizeof(wma->wave_format));
    memset(&wma->wave_header, 0, sizeof(wma->wave_header));
    wma->wave_pcm_bytes = 0;
    wma->wave_total_frames = 0;
    wma->wave_position_frames = 0;
    wma->wave_play_start_frames = 0;
    wma->wave_loop_start_frames = 0;
    wma->wave_play_stop_frames = 0;
    wma->wave_state = MCIQTZ_WAVE_STOPPED;
}

DWORD MCIQTZ_waveout_pause(WINE_MCIQTZ *wma)
{
    if (wma->wave_out && wma->wave_state == MCIQTZ_WAVE_PLAYING)
    {
        wma->wave_position_frames = waveout_playback_position_frames(wma);
        if (waveOutPause(wma->wave_out) != MMSYSERR_NOERROR)
            return MCIERR_INTERNAL;
        wma->wave_state = MCIQTZ_WAVE_PAUSED;
    }
    return 0;
}

DWORD MCIQTZ_waveout_resume(WINE_MCIQTZ *wma)
{
    if (wma->wave_out && wma->wave_state == MCIQTZ_WAVE_PAUSED)
    {
        if (waveOutRestart(wma->wave_out) != MMSYSERR_NOERROR)
            return MCIERR_INTERNAL;
        wma->wave_state = MCIQTZ_WAVE_PLAYING;
    }
    return 0;
}

DWORD MCIQTZ_waveout_length(const WINE_MCIQTZ *wma)
{
    return waveout_frames_to_time(wma, wma->wave_total_frames);
}

DWORD MCIQTZ_waveout_position(const WINE_MCIQTZ *wma)
{
    return waveout_frames_to_time(wma, waveout_playback_position_frames(wma));
}

DWORD MCIQTZ_waveout_mode(const WINE_MCIQTZ *wma)
{
    UINT mode = MCI_MODE_STOP;

    if (wma->wave_state == MCIQTZ_WAVE_PLAYING && wma->thread)
        mode = MCI_MODE_PLAY;
    else if (wma->wave_state == MCIQTZ_WAVE_PAUSED)
        mode = MCI_MODE_PAUSE;
    return MAKEMCIRESOURCE(mode, mode);
}

DWORD MCIQTZ_waveout_set_volume(WINE_MCIQTZ *wma, DWORD value)
{
    DWORD volume = MulDiv(value, 0xffff, 1000) & 0xffff;

    wma->wave_volume = volume | (volume << 16);
    if (wma->wave_out && waveOutSetVolume(wma->wave_out, wma->wave_volume) != MMSYSERR_NOERROR)
        return MCIERR_INTERNAL;
    return 0;
}

