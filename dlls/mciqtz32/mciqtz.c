/*
 * DirectShow MCI Driver
 *
 * Copyright 2009 Christian Costa
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

static const WCHAR mciqtz_class[] = L"MCIQTZ_Window";

static DWORD MCIQTZ_mciClose(UINT, DWORD, LPMCI_GENERIC_PARMS);
static DWORD MCIQTZ_mciStop(UINT, DWORD, LPMCI_GENERIC_PARMS);

/*======================================================================*
 *                          MCI QTZ implementation                      *
 *======================================================================*/

static HINSTANCE MCIQTZ_hInstance = 0;

/***********************************************************************
 *              DllMain (MCIQTZ.0)
 */
BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID fImpLoad)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstDLL);
        MCIQTZ_hInstance = hInstDLL;
        break;
    }
    return TRUE;
}

/**************************************************************************
 *                              MCIQTZ_mciGetOpenDev            [internal]
 */
static WINE_MCIQTZ* MCIQTZ_mciGetOpenDev(UINT wDevID)
{
    WINE_MCIQTZ* wma = (WINE_MCIQTZ*)mciGetDriverData(wDevID);

    if (!wma) {
        WARN("Invalid wDevID=%u\n", wDevID);
        return NULL;
    }
    return wma;
}

static void unregister_class(void)
{
    UnregisterClassW(mciqtz_class, MCIQTZ_hInstance);
}

static bool register_class(void)
{
    WNDCLASSW class = {0};

    class.lpfnWndProc = DefWindowProcW;
    class.cbWndExtra = sizeof(MCIDEVICEID);
    class.hInstance = MCIQTZ_hInstance;
    class.hCursor = LoadCursorW(0, (const WCHAR *)IDC_ARROW);
    class.lpszClassName = mciqtz_class;

    return RegisterClassW(&class) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static BOOL MCIQTZ_is_wave_backend(const WINE_MCIQTZ *wma)
{
    return wma->backend == MCIQTZ_BACKEND_WAVEOUT;
}

static void MCIQTZ_cleanup_finished_thread(WINE_MCIQTZ *wma)
{
    if (wma->thread && WaitForSingleObject(wma->thread, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(wma->thread);
        wma->thread = NULL;
    }
}

static void MCIQTZ_release_graph(WINE_MCIQTZ *wma, BOOL destroy_window)
{
    if (destroy_window && wma->window)
    {
        if (wma->vidwin)
        {
            IVideoWindow_put_MessageDrain(wma->vidwin, (OAHWND)NULL);
            IVideoWindow_put_Owner(wma->vidwin, (OAHWND)NULL);
        }
        DestroyWindow(wma->window);
        wma->window = NULL;
    }

    if (wma->vidwin)
        IVideoWindow_Release(wma->vidwin);
    wma->vidwin = NULL;
    if (wma->vidbasic)
        IBasicVideo_Release(wma->vidbasic);
    wma->vidbasic = NULL;
    if (wma->audio)
        IBasicAudio_Release(wma->audio);
    wma->audio = NULL;
    if (wma->seek)
        IMediaSeeking_Release(wma->seek);
    wma->seek = NULL;
    if (wma->mevent)
        IMediaEvent_Release(wma->mevent);
    wma->mevent = NULL;
    if (wma->pgraph)
        IGraphBuilder_Release(wma->pgraph);
    wma->pgraph = NULL;
    if (wma->pmctrl)
        IMediaControl_Release(wma->pmctrl);
    wma->pmctrl = NULL;

    if (wma->uninit)
        CoUninitialize();
    wma->uninit = FALSE;
    wma->parent = NULL;
}

static DWORD MCIQTZ_frames_to_time(const WINE_MCIQTZ *wma, DWORD frames)
{
    if (!wma->wave_format.nSamplesPerSec) return 0;
    if (wma->time_format == MCI_FORMAT_FRAMES) return frames;
    return (DWORD)(((ULONGLONG)frames * 1000) / wma->wave_format.nSamplesPerSec);
}

static DWORD MCIQTZ_time_to_frames(const WINE_MCIQTZ *wma, DWORD value)
{
    if (!wma->wave_format.nSamplesPerSec) return 0;
    if (wma->time_format == MCI_FORMAT_FRAMES) return value;
    return (DWORD)(((ULONGLONG)value * wma->wave_format.nSamplesPerSec) / 1000);
}

static DWORD MCIQTZ_get_wave_position_frames(const WINE_MCIQTZ *wma)
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

static DWORD MCIQTZ_get_wave_playback_position_frames(const WINE_MCIQTZ *wma)
{
    DWORD frames = wma->wave_position_frames;

    if (wma->wave_out && (wma->wave_state == MCIQTZ_WAVE_PLAYING ||
        wma->wave_state == MCIQTZ_WAVE_PAUSED))
        frames = wma->wave_play_start_frames + MCIQTZ_get_wave_position_frames(wma);

    if (frames > wma->wave_play_stop_frames)
        frames = wma->wave_play_stop_frames;
    if (frames > wma->wave_total_frames)
        frames = wma->wave_total_frames;
    return frames;
}

static DWORD MCIQTZ_wait_for_thread(WINE_MCIQTZ *wma)
{
    DWORD exit_code = 0;

    if (!wma->thread)
        return 0;

    WaitForSingleObject(wma->thread, INFINITE);
    if (!GetExitCodeThread(wma->thread, &exit_code))
        exit_code = MCIERR_INTERNAL;
    CloseHandle(wma->thread);
    wma->thread = NULL;
    return exit_code;
}

static void MCIQTZ_close_wave_output(WINE_MCIQTZ *wma, BOOL reset_device)
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

static void MCIQTZ_reset_wave_fallback(WINE_MCIQTZ *wma)
{
    MCIQTZ_close_wave_output(wma, TRUE);

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

static BOOL MCIQTZ_has_extension(LPCWSTR path, LPCWSTR extension)
{
    const WCHAR *suffix;

    if (!path || !extension) return FALSE;
    suffix = wcsrchr(path, '.');
    return suffix && !lstrcmpiW(suffix, extension);
}

static BOOL MCIQTZ_is_wave_fallback_candidate(LPCWSTR path)
{
    static const WCHAR ext_mp3[] = L".mp3";
    static const WCHAR ext_wav[] = L".wav";

    return MCIQTZ_has_extension(path, ext_mp3) || MCIQTZ_has_extension(path, ext_wav);
}

static DWORD MCIQTZ_load_file_bytes(LPCWSTR path, BYTE **out_data, DWORD *out_size)
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

static DWORD MCIQTZ_decode_wav_file(LPCWSTR path, WAVEFORMATEX *format,
                                    BYTE **out_pcm, DWORD *out_bytes, DWORD *out_frames)
{
    BYTE *file_bytes = NULL, *pcm = NULL;
    DWORD file_size = 0, offset, data_offset = 0, data_size = 0;
    DWORD fmt_offset = 0, fmt_size = 0, output_bytes, i;
    WORD channels, bits_per_sample, format_tag;
    DWORD sample_rate;
    DWORD ret;

    ret = MCIQTZ_load_file_bytes(path, &file_bytes, &file_size);
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

static DWORD MCIQTZ_decode_mp3_file(LPCWSTR path, WAVEFORMATEX *format,
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

    ret = MCIQTZ_load_file_bytes(path, &file_bytes, &file_size);
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

static DWORD MCIQTZ_open_wave_fallback(WINE_MCIQTZ *wma, DWORD flags, const MCI_DGV_OPEN_PARMSW *params)
{
    BYTE *pcm = NULL;
    DWORD pcm_bytes = 0, frames = 0;
    DWORD ret;

    if (MCIQTZ_has_extension(params->lpstrElementName, L".wav"))
        ret = MCIQTZ_decode_wav_file(params->lpstrElementName, &wma->wave_format, &pcm, &pcm_bytes, &frames);
    else if (MCIQTZ_has_extension(params->lpstrElementName, L".mp3"))
        ret = MCIQTZ_decode_mp3_file(params->lpstrElementName, &wma->wave_format, &pcm, &pcm_bytes, &frames);
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

static DWORD CALLBACK MCIQTZ_wave_notifyThread(LPVOID parm)
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
            MCIQTZ_close_wave_output(wma, TRUE);
            ret = MCIERR_INTERNAL;
            break;
        }

        wma->wave_header_prepared = TRUE;
        mmr = waveOutWrite(wma->wave_out, &wma->wave_header, sizeof(wma->wave_header));
        if (mmr != MMSYSERR_NOERROR)
        {
            WARN("waveOutWrite failed: %u\n", mmr);
            MCIQTZ_close_wave_output(wma, TRUE);
            ret = MCIERR_INTERNAL;
            break;
        }

        wma->wave_position_frames = wma->wave_play_start_frames;
        wma->wave_state = MCIQTZ_WAVE_PLAYING;

        wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0)
        {
            DWORD played = MCIQTZ_get_wave_position_frames(wma);
            wma->wave_position_frames = min(wma->wave_play_start_frames + played, wma->wave_play_stop_frames);
            notify_status = MCI_NOTIFY_ABORTED;
            MCIQTZ_close_wave_output(wma, TRUE);
            break;
        }

        if (wait_result != WAIT_OBJECT_0 + 1)
        {
            WARN("Unexpected wave wait result %#lx\n", wait_result);
            ret = MCIERR_INTERNAL;
            MCIQTZ_close_wave_output(wma, TRUE);
            break;
        }

        wma->wave_position_frames = wma->wave_play_stop_frames;
        MCIQTZ_close_wave_output(wma, FALSE);
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

/**************************************************************************
 *                              MCIQTZ_drvOpen                  [internal]
 */
static DWORD MCIQTZ_drvOpen(LPCWSTR str, LPMCI_OPEN_DRIVER_PARMSW modp)
{
    WINE_MCIQTZ* wma;

    TRACE("(%s, %p)\n", debugstr_w(str), modp);

    /* session instance */
    if (!modp)
        return 0xFFFFFFFF;

    if (!register_class())
        return 0;

    wma = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WINE_MCIQTZ));
    if (!wma)
        return 0;

    wma->stop_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    wma->time_format = MCI_FORMAT_MILLISECONDS;
    wma->wave_volume = 0xffffffff;
    modp->wType = MCI_DEVTYPE_DIGITAL_VIDEO;
    wma->wDevID = modp->wDeviceID;
    wma->notify_devid = modp->wDeviceID;
    modp->wCustomCommandTable = wma->command_table = mciLoadCommandResource(MCIQTZ_hInstance, L"MCIAVI", 0);
    mciSetDriverData(wma->wDevID, (DWORD_PTR)wma);

    return modp->wDeviceID;
}

/**************************************************************************
 *                              MCIQTZ_drvClose         [internal]
 */
static DWORD MCIQTZ_drvClose(DWORD dwDevID)
{
    WINE_MCIQTZ* wma;

    TRACE("(%04lx)\n", dwDevID);

    wma = MCIQTZ_mciGetOpenDev(dwDevID);

    if (wma) {
        /* finish all outstanding things */
        MCIQTZ_mciClose(dwDevID, MCI_WAIT, NULL);

        unregister_class();
        mciFreeCommandResource(wma->command_table);
        mciSetDriverData(dwDevID, 0);
        CloseHandle(wma->stop_event);
        HeapFree(GetProcessHeap(), 0, wma);
        return 1;
    }

    return (dwDevID == 0xFFFFFFFF) ? 1 : 0;
}

/**************************************************************************
 *                              MCIQTZ_drvConfigure             [internal]
 */
static DWORD MCIQTZ_drvConfigure(DWORD dwDevID)
{
    WINE_MCIQTZ* wma;

    TRACE("(%04lx)\n", dwDevID);

    wma = MCIQTZ_mciGetOpenDev(dwDevID);
    if (!wma)
        return 0;

    MCIQTZ_mciStop(dwDevID, MCI_WAIT, NULL);

    MessageBoxA(0, "Sample QTZ Wine Driver !", "MM-Wine Driver", MB_OK);

    return 1;
}

static bool create_window(WINE_MCIQTZ *wma, DWORD flags, const MCI_DGV_OPEN_PARMSW *params)
{
    DWORD style = (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN) & ~WS_MAXIMIZEBOX;
    LONG width, height, min_width;
    HWND parent = NULL;
    HRESULT hr;
    RECT rc;

    if (flags & MCI_DGV_OPEN_PARENT)
        parent = params->hWndParent;
    if (flags & MCI_DGV_OPEN_WS)
        style = params->dwStyle;

    hr = IBasicVideo_GetVideoSize(wma->vidbasic, &width, &height);
    if (hr == E_NOINTERFACE)
        return true; /* audio file */
    else if (FAILED(hr))
    {
        ERR("Failed to get video size, hr %#lx.\n", hr);
        return false;
    }

    /* Native always assumes an overlapped window
     * when calculating default video window size. */
    SetRect(&rc, 0, 0, width, height);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    min_width = GetSystemMetrics(SM_CXMIN);
    width = max(rc.right - rc.left, min_width);
    height = rc.bottom - rc.top;

    wma->window = CreateWindowW(mciqtz_class, params->lpstrElementName, style,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, parent, NULL, MCIQTZ_hInstance, NULL);

    TRACE("device %#x, flags %#lx, style %#lx, parent %p, dimensions %ldx%ld, created window %p.\n",
            wma->wDevID, flags, style, parent, width, height, wma->window);

    if (!wma->window)
    {
        ERR("Failed to create window, error %lu.\n", GetLastError());
        return false;
    }

    IVideoWindow_put_AutoShow(wma->vidwin, OAFALSE);
    IVideoWindow_put_MessageDrain(wma->vidwin, (OAHWND)wma->window);
    IVideoWindow_put_Owner(wma->vidwin, (OAHWND)wma->window);
    IVideoWindow_put_WindowStyle(wma->vidwin, WS_CHILD); /* reset window style */

    if (style & (WS_POPUP | WS_CHILD))
        IBasicVideo_GetVideoSize(wma->vidbasic, &width, &height);
    else
    {
        GetClientRect(wma->window, &rc);
        width = rc.right;
        height = rc.bottom;
    }

    IVideoWindow_SetWindowPosition(wma->vidwin, 0, 0, width, height);
    IVideoWindow_put_Visible(wma->vidwin, OATRUE);
    wma->parent = wma->window;

    return true;
}

/**************************************************************************
 *                              MCIQTZ_mciNotify                [internal]
 *
 * Notifications in MCI work like a 1-element queue.
 * Each new notification request supersedes the previous one.
 */
static void MCIQTZ_mciNotify(DWORD_PTR hWndCallBack, WINE_MCIQTZ* wma, UINT wStatus)
{
    MCIDEVICEID wDevID = wma->notify_devid;
    HANDLE old = InterlockedExchangePointer(&wma->callback, NULL);
    if (old) mciDriverNotify(old, wDevID, MCI_NOTIFY_SUPERSEDED);
    mciDriverNotify(HWND_32(LOWORD(hWndCallBack)), wDevID, wStatus);
}

/***************************************************************************
 *                              MCIQTZ_mciOpen                  [internal]
 */
static DWORD MCIQTZ_mciOpen(UINT wDevID, DWORD dwFlags,
                            LPMCI_DGV_OPEN_PARMSW lpOpenParms)
{
    WINE_MCIQTZ* wma;
    HRESULT hr;
    DWORD ret;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpOpenParms);

    if(!lpOpenParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    wma->notify_devid = wDevID;
    MCIQTZ_mciStop(wDevID, MCI_WAIT, NULL);

    if (!(dwFlags & MCI_OPEN_ELEMENT) || (dwFlags & MCI_OPEN_ELEMENT_ID)) {
        TRACE("Wrong dwFlags %lx\n", dwFlags);
        return MCIERR_INVALID_FILE;
    }

    if (!lpOpenParms->lpstrElementName || !lpOpenParms->lpstrElementName[0]) {
        TRACE("Invalid filename specified\n");
        return MCIERR_FILE_NOT_FOUND;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    wma->uninit = SUCCEEDED(hr);

    hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder, (LPVOID*)&wma->pgraph);
    if (FAILED(hr)) {
        TRACE("Cannot create filtergraph (hr = %lx)\n", hr);
        goto err;
    }

    hr = IGraphBuilder_QueryInterface(wma->pgraph, &IID_IMediaControl, (LPVOID*)&wma->pmctrl);
    if (FAILED(hr)) {
        TRACE("Cannot get IMediaControl interface (hr = %lx)\n", hr);
        goto err;
    }

    hr = IGraphBuilder_QueryInterface(wma->pgraph, &IID_IMediaSeeking, (void**)&wma->seek);
    if (FAILED(hr)) {
        TRACE("Cannot get IMediaSeeking interface (hr = %lx)\n", hr);
        goto err;
    }

    hr = IGraphBuilder_QueryInterface(wma->pgraph, &IID_IMediaEvent, (void**)&wma->mevent);
    if (FAILED(hr)) {
        TRACE("Cannot get IMediaEvent interface (hr = %lx)\n", hr);
        goto err;
    }

    hr = IGraphBuilder_QueryInterface(wma->pgraph, &IID_IVideoWindow, (void**)&wma->vidwin);
    if (FAILED(hr)) {
        TRACE("Cannot get IVideoWindow interface (hr = %lx)\n", hr);
        goto err;
    }

    hr = IGraphBuilder_QueryInterface(wma->pgraph, &IID_IBasicVideo, (void**)&wma->vidbasic);
    if (FAILED(hr)) {
        TRACE("Cannot get IBasicVideo interface (hr = %lx)\n", hr);
        goto err;
    }

    hr = IGraphBuilder_QueryInterface(wma->pgraph, &IID_IBasicAudio, (void**)&wma->audio);
    if (FAILED(hr)) {
        TRACE("Cannot get IBasicAudio interface (hr = %lx)\n", hr);
        goto err;
    }

    TRACE("Open file %s\n", debugstr_w(lpOpenParms->lpstrElementName));

    hr = IGraphBuilder_RenderFile(wma->pgraph, lpOpenParms->lpstrElementName, NULL);
    if (FAILED(hr)) {
        TRACE("Cannot render file (hr = %lx)\n", hr);
        goto err;
    }

    if (!create_window(wma, dwFlags, lpOpenParms))
        goto err;
    wma->backend = MCIQTZ_BACKEND_DSHOW;
    wma->opened = TRUE;

    if (dwFlags & MCI_NOTIFY)
        mciDriverNotify(HWND_32(LOWORD(lpOpenParms->dwCallback)), wDevID, MCI_NOTIFY_SUCCESSFUL);

    return 0;

err:
    MCIQTZ_release_graph(wma, TRUE);
    wma->backend = MCIQTZ_BACKEND_NONE;

    if (MCIQTZ_is_wave_fallback_candidate(lpOpenParms->lpstrElementName))
    {
        ret = MCIQTZ_open_wave_fallback(wma, dwFlags, lpOpenParms);
        if (!ret)
            return 0;
        TRACE("waveOut fallback failed for %s, ret %#lx\n",
              debugstr_w(lpOpenParms->lpstrElementName), ret);
        return ret;
    }

    return MCIERR_INTERNAL;
}

/***************************************************************************
 *                              MCIQTZ_mciClose                 [internal]
 */
static DWORD MCIQTZ_mciClose(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    WINE_MCIQTZ* wma;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    MCIQTZ_mciStop(wDevID, MCI_WAIT, NULL);

    if (wma->opened) {
        if (MCIQTZ_is_wave_backend(wma))
            MCIQTZ_reset_wave_fallback(wma);
        else
            MCIQTZ_release_graph(wma, TRUE);
        wma->opened = FALSE;
        wma->backend = MCIQTZ_BACKEND_NONE;
    }

    return 0;
}

/***************************************************************************
 *                              MCIQTZ_notifyThread             [internal]
 */
static DWORD CALLBACK MCIQTZ_notifyThread(LPVOID parm)
{
    WINE_MCIQTZ* wma = (WINE_MCIQTZ *)parm;
    HRESULT hr;
    HANDLE handle[2];
    DWORD n = 0, ret = 0;

    handle[n++] = wma->stop_event;
    IMediaEvent_GetEventHandle(wma->mevent, (OAEVENT *)&handle[n++]);

    for (;;) {
        DWORD r;
        HANDLE old;

        r = WaitForMultipleObjects(n, handle, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            TRACE("got stop event\n");
            old = InterlockedExchangePointer(&wma->callback, NULL);
            if (old)
                mciDriverNotify(old, wma->notify_devid, MCI_NOTIFY_ABORTED);
            break;
        }
        else if (r == WAIT_OBJECT_0+1) {
            LONG event_code;
            LONG_PTR p1, p2;
            do {
                hr = IMediaEvent_GetEvent(wma->mevent, &event_code, &p1, &p2, 0);
                if (SUCCEEDED(hr)) {
                    TRACE("got event_code = 0x%02lx\n", event_code);
                    IMediaEvent_FreeEventParams(wma->mevent, event_code, p1, p2);
                }
            } while (hr == S_OK && event_code != EC_COMPLETE);
            if (hr == S_OK && event_code == EC_COMPLETE) {
                /* Repeat the music by seeking and running again */
                if (wma->mci_flags & MCI_DGV_PLAY_REPEAT) {
                    TRACE("repeat media as requested\n");
                    IMediaControl_Stop(wma->pmctrl);
                    IMediaSeeking_SetPositions(wma->seek,
                                               &wma->seek_start,
                                               AM_SEEKING_AbsolutePositioning,
                                               &wma->seek_stop,
                                               AM_SEEKING_AbsolutePositioning);
                    IMediaControl_Run(wma->pmctrl);
                    continue;
                }
                old = InterlockedExchangePointer(&wma->callback, NULL);
                if (old)
                    mciDriverNotify(old, wma->notify_devid, MCI_NOTIFY_SUCCESSFUL);
                break;
            }
        }
        else {
            TRACE("Unknown error (%d)\n", (int)r);
            break;
        }
    }

    hr = IMediaControl_Stop(wma->pmctrl);
    if (FAILED(hr)) {
        TRACE("Cannot stop filtergraph (hr = %lx)\n", hr);
        ret = MCIERR_INTERNAL;
    }

    return ret;
}

/***************************************************************************
 *                              MCIQTZ_mciPlay                  [internal]
 */
static DWORD MCIQTZ_mciPlay(UINT wDevID, DWORD dwFlags, LPMCI_PLAY_PARMS lpParms)
{
    WINE_MCIQTZ* wma;
    HRESULT hr;
    GUID format;
    DWORD start_flags;
    DWORD ret = 0;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (MCIQTZ_is_wave_backend(wma))
        MCIQTZ_mciStop(wDevID, MCI_WAIT, NULL);

    MCIQTZ_cleanup_finished_thread(wma);
    ResetEvent(wma->stop_event);
    if (dwFlags & MCI_NOTIFY) {
        HANDLE old;
        old = InterlockedExchangePointer(&wma->callback, HWND_32(LOWORD(lpParms->dwCallback)));
        if (old)
            mciDriverNotify(old, wma->notify_devid, MCI_NOTIFY_ABORTED);
    }

    wma->mci_flags = dwFlags;
    if (MCIQTZ_is_wave_backend(wma))
    {
        DWORD start, stop;

        start = (dwFlags & MCI_FROM) ? MCIQTZ_time_to_frames(wma, lpParms->dwFrom) : wma->wave_position_frames;
        if (!(dwFlags & MCI_FROM) && start >= wma->wave_total_frames)
            start = 0;
        stop = (dwFlags & MCI_TO) ? MCIQTZ_time_to_frames(wma, lpParms->dwTo) : wma->wave_total_frames;

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
            wma->thread = CreateThread(NULL, 0, MCIQTZ_wave_notifyThread, wma, 0, NULL);
            if (!wma->thread) {
                TRACE("Can't create thread\n");
                return MCIERR_INTERNAL;
            }
        }
        else if (dwFlags & MCI_NOTIFY)
        {
            MCIQTZ_mciNotify(lpParms->dwCallback, wma, MCI_NOTIFY_SUCCESSFUL);
        }

        if (dwFlags & MCI_WAIT)
            ret = MCIQTZ_wait_for_thread(wma);
        return ret;
    }

    IMediaSeeking_GetTimeFormat(wma->seek, &format);
    if (dwFlags & MCI_FROM) {
        wma->seek_start = lpParms->dwFrom;
        if (IsEqualGUID(&format, &TIME_FORMAT_MEDIA_TIME))
            wma->seek_start *= 10000;

        start_flags = AM_SEEKING_AbsolutePositioning;
    } else {
        wma->seek_start = 0;
        start_flags = AM_SEEKING_NoPositioning;
    }
    if (dwFlags & MCI_TO) {
        wma->seek_stop = lpParms->dwTo;
        if (IsEqualGUID(&format, &TIME_FORMAT_MEDIA_TIME))
            wma->seek_stop *= 10000;
    } else {
        wma->seek_stop = 0;
        IMediaSeeking_GetDuration(wma->seek, &wma->seek_stop);
    }
    IMediaSeeking_SetPositions(wma->seek, &wma->seek_start, start_flags,
                               &wma->seek_stop, AM_SEEKING_AbsolutePositioning);

    hr = IMediaControl_Run(wma->pmctrl);
    if (FAILED(hr)) {
        TRACE("Cannot run filtergraph (hr = %lx)\n", hr);
        return MCIERR_INTERNAL;
    }

    if (wma->parent)
        ShowWindow(wma->parent, SW_SHOW);

    if (!wma->thread)
    {
        wma->thread = CreateThread(NULL, 0, MCIQTZ_notifyThread, wma, 0, NULL);
        if (!wma->thread) {
            TRACE("Can't create thread\n");
            return MCIERR_INTERNAL;
        }
    }

    if (dwFlags & MCI_WAIT)
        return MCIQTZ_wait_for_thread(wma);
    return 0;
}

/***************************************************************************
 *                              MCIQTZ_mciSeek                  [internal]
 */
static DWORD MCIQTZ_mciSeek(UINT wDevID, DWORD dwFlags, LPMCI_SEEK_PARMS lpParms)
{
    WINE_MCIQTZ* wma;
    HRESULT hr;
    LONGLONG newpos;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    MCIQTZ_mciStop(wDevID, MCI_WAIT, NULL);

    if (MCIQTZ_is_wave_backend(wma))
    {
        if (dwFlags & MCI_SEEK_TO_START)
            wma->wave_position_frames = 0;
        else if (dwFlags & MCI_SEEK_TO_END)
            wma->wave_position_frames = wma->wave_total_frames;
        else if (dwFlags & MCI_TO)
            wma->wave_position_frames = min(MCIQTZ_time_to_frames(wma, lpParms->dwTo), wma->wave_total_frames);
        else {
            WARN("dwFlag doesn't tell where to seek to...\n");
            return MCIERR_MISSING_PARAMETER;
        }

        if (dwFlags & MCI_NOTIFY)
            MCIQTZ_mciNotify(lpParms->dwCallback, wma, MCI_NOTIFY_SUCCESSFUL);
        return 0;
    }

    if (dwFlags & MCI_SEEK_TO_START) {
        newpos = 0;
    } else if (dwFlags & MCI_SEEK_TO_END) {
        FIXME("MCI_SEEK_TO_END not implemented yet\n");
        return MCIERR_INTERNAL;
    } else if (dwFlags & MCI_TO) {
        FIXME("MCI_TO not implemented yet\n");
        return MCIERR_INTERNAL;
    } else {
        WARN("dwFlag doesn't tell where to seek to...\n");
        return MCIERR_MISSING_PARAMETER;
    }

    hr = IMediaSeeking_SetPositions(wma->seek, &newpos, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning);
    if (FAILED(hr)) {
        FIXME("Cannot set position (hr = %lx)\n", hr);
        return MCIERR_INTERNAL;
    }

    if (dwFlags & MCI_NOTIFY)
        MCIQTZ_mciNotify(lpParms->dwCallback, wma, MCI_NOTIFY_SUCCESSFUL);

    return 0;
}

/***************************************************************************
 *                              MCIQTZ_mciStop                  [internal]
 */
static DWORD MCIQTZ_mciStop(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    WINE_MCIQTZ* wma;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (!wma->opened)
        return 0;

    MCIQTZ_cleanup_finished_thread(wma);

    if (MCIQTZ_is_wave_backend(wma) && wma->wave_state != MCIQTZ_WAVE_STOPPED)
        wma->wave_position_frames = MCIQTZ_get_wave_playback_position_frames(wma);

    if (wma->thread) {
        SetEvent(wma->stop_event);
        MCIQTZ_wait_for_thread(wma);
    }

    if (MCIQTZ_is_wave_backend(wma))
        wma->wave_state = MCIQTZ_WAVE_STOPPED;

    return 0;
}

/***************************************************************************
 *                              MCIQTZ_mciPause                 [internal]
 */
static DWORD MCIQTZ_mciPause(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    WINE_MCIQTZ* wma;
    HRESULT hr;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (MCIQTZ_is_wave_backend(wma))
    {
        if (wma->wave_out && wma->wave_state == MCIQTZ_WAVE_PLAYING)
        {
            wma->wave_position_frames = MCIQTZ_get_wave_playback_position_frames(wma);
            if (waveOutPause(wma->wave_out) != MMSYSERR_NOERROR)
                return MCIERR_INTERNAL;
            wma->wave_state = MCIQTZ_WAVE_PAUSED;
        }
        return 0;
    }

    hr = IMediaControl_Pause(wma->pmctrl);
    if (FAILED(hr)) {
        TRACE("Cannot pause filtergraph (hr = %lx)\n", hr);
        return MCIERR_INTERNAL;
    }

    return 0;
}

/***************************************************************************
 *                              MCIQTZ_mciResume                 [internal]
 */
static DWORD MCIQTZ_mciResume(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    WINE_MCIQTZ* wma;
    HRESULT hr;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (MCIQTZ_is_wave_backend(wma))
    {
        if (wma->wave_out && wma->wave_state == MCIQTZ_WAVE_PAUSED)
        {
            if (waveOutRestart(wma->wave_out) != MMSYSERR_NOERROR)
                return MCIERR_INTERNAL;
            wma->wave_state = MCIQTZ_WAVE_PLAYING;
        }
        return 0;
    }

    hr = IMediaControl_Run(wma->pmctrl);
    if (FAILED(hr)) {
        TRACE("Cannot run filtergraph (hr = %lx)\n", hr);
        return MCIERR_INTERNAL;
    }

    return 0;
}

/***************************************************************************
 *                              MCIQTZ_mciGetDevCaps            [internal]
 */
static DWORD MCIQTZ_mciGetDevCaps(UINT wDevID, DWORD dwFlags, LPMCI_GETDEVCAPS_PARMS lpParms)
{
    WINE_MCIQTZ* wma;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (!(dwFlags & MCI_GETDEVCAPS_ITEM))
        return MCIERR_MISSING_PARAMETER;

    switch (lpParms->dwItem) {
        case MCI_GETDEVCAPS_CAN_RECORD:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_GETDEVCAPS_CAN_RECORD = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_HAS_AUDIO:
            lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
            TRACE("MCI_GETDEVCAPS_HAS_AUDIO = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_HAS_VIDEO:
            lpParms->dwReturn = MAKEMCIRESOURCE(!MCIQTZ_is_wave_backend(wma), !MCIQTZ_is_wave_backend(wma) ? MCI_TRUE : MCI_FALSE);
            TRACE("MCI_GETDEVCAPS_HAS_VIDEO = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_DEVICE_TYPE:
            lpParms->dwReturn = MAKEMCIRESOURCE(MCI_DEVTYPE_DIGITAL_VIDEO, MCI_DEVTYPE_DIGITAL_VIDEO);
            TRACE("MCI_GETDEVCAPS_DEVICE_TYPE = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_USES_FILES:
            lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
            TRACE("MCI_GETDEVCAPS_USES_FILES = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_COMPOUND_DEVICE:
            lpParms->dwReturn = MAKEMCIRESOURCE(!MCIQTZ_is_wave_backend(wma), !MCIQTZ_is_wave_backend(wma) ? MCI_TRUE : MCI_FALSE);
            TRACE("MCI_GETDEVCAPS_COMPOUND_DEVICE = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_CAN_EJECT:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_GETDEVCAPS_EJECT = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_CAN_PLAY:
            lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
            TRACE("MCI_GETDEVCAPS_CAN_PLAY = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_GETDEVCAPS_CAN_SAVE:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_GETDEVCAPS_CAN_SAVE = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_CAN_REVERSE:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_DGV_GETDEVCAPS_CAN_REVERSE = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_CAN_STRETCH:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE); /* FIXME */
            TRACE("MCI_DGV_GETDEVCAPS_CAN_STRETCH = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_CAN_LOCK:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_DGV_GETDEVCAPS_CAN_LOCK = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_CAN_FREEZE:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_DGV_GETDEVCAPS_CAN_FREEZE = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_CAN_STR_IN:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_DGV_GETDEVCAPS_CAN_STRETCH_INPUT = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_HAS_STILL:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
            TRACE("MCI_DGV_GETDEVCAPS_HAS_STILL = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_CAN_TEST:
            lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE); /* FIXME */
            TRACE("MCI_DGV_GETDEVCAPS_CAN_TEST = %08lx\n", lpParms->dwReturn);
            break;
        case MCI_DGV_GETDEVCAPS_MAX_WINDOWS:
            lpParms->dwReturn = MCIQTZ_is_wave_backend(wma) ? 0 : 1;
            TRACE("MCI_DGV_GETDEVCAPS_MAX_WINDOWS = %lu\n", lpParms->dwReturn);
            return 0;
        default:
            WARN("Unknown capability %08lx\n", lpParms->dwItem);
            /* Fall through */
        case MCI_DGV_GETDEVCAPS_MAXIMUM_RATE: /* unknown to w2k */
        case MCI_DGV_GETDEVCAPS_MINIMUM_RATE: /* unknown to w2k */
            return MCIERR_UNSUPPORTED_FUNCTION;
    }

    return MCI_RESOURCE_RETURNED;
}

/***************************************************************************
 *                              MCIQTZ_mciSet                   [internal]
 */
static DWORD MCIQTZ_mciSet(UINT wDevID, DWORD dwFlags, LPMCI_DGV_SET_PARMS lpParms)
{
    WINE_MCIQTZ* wma;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (dwFlags & MCI_SET_TIME_FORMAT) {
        switch (lpParms->dwTimeFormat) {
            case MCI_FORMAT_MILLISECONDS:
                TRACE("MCI_SET_TIME_FORMAT = MCI_FORMAT_MILLISECONDS\n");
                wma->time_format = MCI_FORMAT_MILLISECONDS;
                break;
            case MCI_FORMAT_FRAMES:
                TRACE("MCI_SET_TIME_FORMAT = MCI_FORMAT_FRAMES\n");
                wma->time_format = MCI_FORMAT_FRAMES;
                break;
            default:
                WARN("Bad time format %lu\n", lpParms->dwTimeFormat);
                return MCIERR_BAD_TIME_FORMAT;
        }
    }

    if (dwFlags & MCI_SET_DOOR_OPEN)
        FIXME("MCI_SET_DOOR_OPEN not implemented yet\n");
    if (dwFlags & MCI_SET_DOOR_CLOSED)
        FIXME("MCI_SET_DOOR_CLOSED not implemented yet\n");
    if (dwFlags & MCI_SET_AUDIO)
        FIXME("MCI_SET_AUDIO not implemented yet\n");
    if (dwFlags & MCI_SET_VIDEO)
        FIXME("MCI_SET_VIDEO not implemented yet\n");
    if (dwFlags & MCI_SET_ON)
        FIXME("MCI_SET_ON not implemented yet\n");
    if (dwFlags & MCI_SET_OFF)
        FIXME("MCI_SET_OFF not implemented yet\n");
    if (dwFlags & MCI_SET_AUDIO_LEFT)
        FIXME("MCI_SET_AUDIO_LEFT not implemented yet\n");
    if (dwFlags & MCI_SET_AUDIO_RIGHT)
        FIXME("MCI_SET_AUDIO_RIGHT not implemented yet\n");

    if (dwFlags & ~0x7f03 /* All MCI_SET flags mask */)
        ERR("Unknown flags %08lx\n", dwFlags & ~0x7f03);

    return 0;
}

/***************************************************************************
 *                              MCIQTZ_mciStatus                [internal]
 */
static DWORD MCIQTZ_mciStatus(UINT wDevID, DWORD dwFlags, LPMCI_DGV_STATUS_PARMSW lpParms)
{
    WINE_MCIQTZ* wma;
    HRESULT hr;
    DWORD ret = MCI_INTEGER_RETURNED;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    MCIQTZ_cleanup_finished_thread(wma);

    if (!(dwFlags & MCI_STATUS_ITEM)) {
        WARN("No status item specified\n");
        return MCIERR_UNRECOGNIZED_COMMAND;
    }

    switch (lpParms->dwItem) {
        case MCI_STATUS_LENGTH: {
            if (MCIQTZ_is_wave_backend(wma))
            {
                lpParms->dwReturn = MCIQTZ_frames_to_time(wma, wma->wave_total_frames);
                break;
            }
            LONGLONG duration = -1;
            GUID format;
            switch (wma->time_format) {
                case MCI_FORMAT_MILLISECONDS: format = TIME_FORMAT_MEDIA_TIME; break;
                case MCI_FORMAT_FRAMES: format = TIME_FORMAT_FRAME; break;
                default: ERR("Unhandled format %lx\n", wma->time_format); break;
            }
            hr = IMediaSeeking_SetTimeFormat(wma->seek, &format);
            if (FAILED(hr)) {
                FIXME("Cannot set time format (hr = %lx)\n", hr);
                lpParms->dwReturn = 0;
                break;
            }
            hr = IMediaSeeking_GetDuration(wma->seek, &duration);
            if (FAILED(hr) || duration < 0) {
                FIXME("Cannot read duration (hr = %lx)\n", hr);
                lpParms->dwReturn = 0;
            } else if (wma->time_format != MCI_FORMAT_MILLISECONDS)
                lpParms->dwReturn = duration;
            else
                lpParms->dwReturn = duration / 10000;
            break;
        }
        case MCI_STATUS_POSITION: {
            if (MCIQTZ_is_wave_backend(wma))
            {
                lpParms->dwReturn = MCIQTZ_frames_to_time(wma, MCIQTZ_get_wave_playback_position_frames(wma));
                break;
            }
            REFERENCE_TIME curpos;
            GUID format;

            hr = IMediaSeeking_GetCurrentPosition(wma->seek, &curpos);
            if (FAILED(hr)) {
                FIXME("Cannot get position (hr = %lx)\n", hr);
                return MCIERR_INTERNAL;
            }
            IMediaSeeking_GetTimeFormat(wma->seek, &format);
            lpParms->dwReturn = IsEqualGUID(&format, &TIME_FORMAT_MEDIA_TIME) ? curpos / 10000 : curpos;
            break;
        }
        case MCI_STATUS_NUMBER_OF_TRACKS:
            FIXME("MCI_STATUS_NUMBER_OF_TRACKS not implemented yet\n");
            return MCIERR_UNRECOGNIZED_COMMAND;
        case MCI_STATUS_MODE: {
            if (MCIQTZ_is_wave_backend(wma))
            {
                UINT mode = MCI_MODE_STOP;

                if (wma->wave_state == MCIQTZ_WAVE_PLAYING && wma->thread)
                    mode = MCI_MODE_PLAY;
                else if (wma->wave_state == MCIQTZ_WAVE_PAUSED)
                    mode = MCI_MODE_PAUSE;
                lpParms->dwReturn = MAKEMCIRESOURCE(mode, mode);
                ret = MCI_RESOURCE_RETURNED;
                break;
            }
            LONG state = State_Stopped;
            IMediaControl_GetState(wma->pmctrl, -1, &state);
            if (state == State_Stopped)
                lpParms->dwReturn = MAKEMCIRESOURCE(MCI_MODE_STOP, MCI_MODE_STOP);
            else if (state == State_Running) {
                lpParms->dwReturn = MAKEMCIRESOURCE(MCI_MODE_PLAY, MCI_MODE_PLAY);
                if (!wma->thread || WaitForSingleObject(wma->thread, 0) == WAIT_OBJECT_0)
                    lpParms->dwReturn = MAKEMCIRESOURCE(MCI_MODE_STOP, MCI_MODE_STOP);
            } else if (state == State_Paused)
                lpParms->dwReturn = MAKEMCIRESOURCE(MCI_MODE_PAUSE, MCI_MODE_PAUSE);
            ret = MCI_RESOURCE_RETURNED;
            break;
        }
        case MCI_STATUS_MEDIA_PRESENT:
            FIXME("MCI_STATUS_MEDIA_PRESENT not implemented yet\n");
            return MCIERR_UNRECOGNIZED_COMMAND;
        case MCI_STATUS_TIME_FORMAT:
            lpParms->dwReturn = MAKEMCIRESOURCE(wma->time_format,
                                                MCI_FORMAT_RETURN_BASE + wma->time_format);
            ret = MCI_RESOURCE_RETURNED;
            break;
        case MCI_STATUS_READY:
            lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
            ret = MCI_RESOURCE_RETURNED;
            break;
        case MCI_STATUS_CURRENT_TRACK:
            FIXME("MCI_STATUS_CURRENT_TRACK not implemented yet\n");
            return MCIERR_UNRECOGNIZED_COMMAND;
        default:
            FIXME("Unknown command %08lX\n", lpParms->dwItem);
            return MCIERR_UNRECOGNIZED_COMMAND;
    }

    if (dwFlags & MCI_NOTIFY)
        MCIQTZ_mciNotify(lpParms->dwCallback, wma, MCI_NOTIFY_SUCCESSFUL);

    return ret;
}

/***************************************************************************
 *                              MCIQTZ_mciWhere                 [internal]
 */
static DWORD MCIQTZ_mciWhere(UINT wDevID, DWORD dwFlags, LPMCI_DGV_RECT_PARMS lpParms)
{
    WINE_MCIQTZ* wma;
    HRESULT hr;
    HWND hWnd;
    RECT rc;
    DWORD ret = MCIERR_UNRECOGNIZED_COMMAND;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (MCIQTZ_is_wave_backend(wma))
        return MCIERR_NO_WINDOW;

    hr = IVideoWindow_get_Owner(wma->vidwin, (OAHWND*)&hWnd);
    if (FAILED(hr)) {
        TRACE("No video stream, returning no window error\n");
        return MCIERR_NO_WINDOW;
    }

    if (dwFlags & MCI_DGV_WHERE_SOURCE) {
        if (dwFlags & MCI_DGV_WHERE_MAX)
            FIXME("MCI_DGV_WHERE_SOURCE_MAX stub\n");
        IBasicVideo_GetSourcePosition(wma->vidbasic, &rc.left, &rc.top, &rc.right, &rc.bottom);
        TRACE("MCI_DGV_WHERE_SOURCE %s\n", wine_dbgstr_rect(&rc));
    }
    if (dwFlags & MCI_DGV_WHERE_DESTINATION) {
        if (dwFlags & MCI_DGV_WHERE_MAX)
            FIXME("MCI_DGV_WHERE_DESTINATION_MAX stub\n");
        IBasicVideo_GetDestinationPosition(wma->vidbasic, &rc.left, &rc.top, &rc.right, &rc.bottom);
        TRACE("MCI_DGV_WHERE_DESTINATION %s\n", wine_dbgstr_rect(&rc));
    }
    if (dwFlags & MCI_DGV_WHERE_FRAME) {
        if (dwFlags & MCI_DGV_WHERE_MAX)
            FIXME("MCI_DGV_WHERE_FRAME_MAX not supported yet\n");
        else
            FIXME("MCI_DGV_WHERE_FRAME not supported yet\n");
        goto out;
    }
    if (dwFlags & MCI_DGV_WHERE_VIDEO) {
        if (dwFlags & MCI_DGV_WHERE_MAX)
            FIXME("MCI_DGV_WHERE_VIDEO_MAX not supported yet\n");
        else
            FIXME("MCI_DGV_WHERE_VIDEO not supported yet\n");
        goto out;
    }
    if (dwFlags & MCI_DGV_WHERE_WINDOW) {
        if (dwFlags & MCI_DGV_WHERE_MAX) {
            GetWindowRect(GetDesktopWindow(), &rc);
            rc.right -= rc.left;
            rc.bottom -= rc.top;
            TRACE("MCI_DGV_WHERE_WINDOW_MAX %s\n", wine_dbgstr_rect(&rc));
        } else {
            GetWindowRect(wma->parent, &rc);
            rc.right -= rc.left;
            rc.bottom -= rc.top;
            TRACE("MCI_DGV_WHERE_WINDOW %s\n", wine_dbgstr_rect(&rc));
        }
    }
    ret = 0;
out:
    lpParms->rc = rc;
    return ret;
}

/***************************************************************************
 *                              MCIQTZ_mciWindow                [internal]
 */
static DWORD MCIQTZ_mciWindow(UINT wDevID, DWORD dwFlags, LPMCI_DGV_WINDOW_PARMSW lpParms)
{
    WINE_MCIQTZ *wma = MCIQTZ_mciGetOpenDev(wDevID);

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;
    if (MCIQTZ_is_wave_backend(wma))
        return MCIERR_NO_WINDOW;
    if (dwFlags & MCI_TEST)
        return 0;

    if (dwFlags & MCI_DGV_WINDOW_HWND) {
        HWND hwnd;
        if (lpParms->hWnd && !IsWindow(lpParms->hWnd))
            return MCIERR_NO_WINDOW;
        if (!wma->parent)
            return MCIERR_INTERNAL;
        hwnd = lpParms->hWnd ? lpParms->hWnd : wma->window;
        TRACE("Setting parent window to %p.\n", hwnd);
        if (wma->parent != hwnd)
        {
            LONG width, height;

            IVideoWindow_put_MessageDrain(wma->vidwin, (OAHWND)hwnd);
            IVideoWindow_put_Owner(wma->vidwin, (OAHWND)hwnd);

            IBasicVideo_GetVideoSize(wma->vidbasic, &width, &height);
            IVideoWindow_SetWindowPosition(wma->vidwin, 0, 0, width, height);

            if (wma->parent == wma->window)
                ShowWindow(wma->window, SW_HIDE);
            else if (hwnd == wma->window)
                ShowWindow(wma->window, SW_SHOW);

            wma->parent = hwnd;
        }
    }
    if (dwFlags & MCI_DGV_WINDOW_STATE) {
        if (!wma->parent)
            return MCIERR_NO_WINDOW;
        TRACE("Setting nCmdShow to %d\n", lpParms->nCmdShow);
        ShowWindow(wma->parent, lpParms->nCmdShow);
    }
    if (dwFlags & MCI_DGV_WINDOW_TEXT) {
        if (!wma->parent)
            return MCIERR_NO_WINDOW;
        TRACE("Setting caption to %s\n", debugstr_w(lpParms->lpstrText));
        SetWindowTextW(wma->parent, lpParms->lpstrText);
    }
    return 0;
}

/***************************************************************************
 *                              MCIQTZ_mciPut                   [internal]
 */
static DWORD MCIQTZ_mciPut(UINT wDevID, DWORD dwFlags, MCI_GENERIC_PARMS *lpParms)
{
    WINE_MCIQTZ *wma = MCIQTZ_mciGetOpenDev(wDevID);
    MCI_DGV_RECT_PARMS *rectparms;
    HRESULT hr;

    TRACE("(%04x, %08lX, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;
    if (MCIQTZ_is_wave_backend(wma))
        return MCIERR_NO_WINDOW;

    if (!(dwFlags & MCI_DGV_RECT)) {
        FIXME("No support for non-RECT MCI_PUT\n");
        return 1;
    }

    if (dwFlags & MCI_TEST)
        return 0;

    dwFlags &= ~MCI_DGV_RECT;
    rectparms = (MCI_DGV_RECT_PARMS*)lpParms;

    if (dwFlags & MCI_DGV_PUT_DESTINATION) {
        hr = IVideoWindow_SetWindowPosition(wma->vidwin,
                rectparms->rc.left, rectparms->rc.top,
                rectparms->rc.right - rectparms->rc.left,
                rectparms->rc.bottom - rectparms->rc.top);
        if(FAILED(hr))
            WARN("IVideoWindow_SetWindowPosition failed: 0x%lx\n", hr);

        dwFlags &= ~MCI_DGV_PUT_DESTINATION;
    }

    if (dwFlags & MCI_NOTIFY) {
        MCIQTZ_mciNotify(lpParms->dwCallback, wma, MCI_NOTIFY_SUCCESSFUL);
        dwFlags &= ~MCI_NOTIFY;
    }

    if (dwFlags)
        FIXME("No support for some flags: 0x%lx\n", dwFlags);

    return 0;
}

/******************************************************************************
 *              MCIAVI_mciUpdate            [internal]
 */
static DWORD MCIQTZ_mciUpdate(UINT wDevID, DWORD dwFlags, LPMCI_DGV_UPDATE_PARMS lpParms)
{
    WINE_MCIQTZ *wma;
    DWORD res = 0;

    TRACE("%04x, %08lx, %p\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;
    if (MCIQTZ_is_wave_backend(wma))
        return MCIERR_NO_WINDOW;

    if (dwFlags & MCI_DGV_UPDATE_HDC) {
        LONG state, size;
        BYTE *data;
        BITMAPINFO *info;
        HRESULT hr;
        RECT src, dest;
        LONG visible = OATRUE;

        res = MCIERR_INTERNAL;
        IMediaControl_GetState(wma->pmctrl, -1, &state);
        if (state == State_Running)
            return MCIERR_UNSUPPORTED_FUNCTION;
        /* If in stopped state, nothing has been drawn to screen
         * moving to pause, which is needed for the old dib renderer, will result
         * in a single frame drawn, so hide the window here */
        IVideoWindow_get_Visible(wma->vidwin, &visible);
        if (wma->parent)
            IVideoWindow_put_Visible(wma->vidwin, OAFALSE);
        /* FIXME: Should we check the original state and restore it? */
        IMediaControl_Pause(wma->pmctrl);
        IMediaControl_GetState(wma->pmctrl, -1, &state);
        if (FAILED(hr = IBasicVideo_GetCurrentImage(wma->vidbasic, &size, NULL))) {
            WARN("Could not get image size (hr = %lx)\n", hr);
            goto out;
        }
        data = HeapAlloc(GetProcessHeap(), 0, size);
        info = (BITMAPINFO*)data;
        IBasicVideo_GetCurrentImage(wma->vidbasic, &size, (LONG*)data);
        data += info->bmiHeader.biSize;

        IBasicVideo_GetSourcePosition(wma->vidbasic, &src.left, &src.top, &src.right, &src.bottom);
        IBasicVideo_GetDestinationPosition(wma->vidbasic, &dest.left, &dest.top, &dest.right, &dest.bottom);
        StretchDIBits(lpParms->hDC,
              dest.left, dest.top, dest.right + dest.left, dest.bottom + dest.top,
              src.left, src.top, src.right + src.left, src.bottom + src.top,
              data, info, DIB_RGB_COLORS, SRCCOPY);
        HeapFree(GetProcessHeap(), 0, data);
        res = 0;
out:
        if (wma->parent)
            IVideoWindow_put_Visible(wma->vidwin, visible);
    }
    else if (dwFlags)
        FIXME("Unhandled flags %lx\n", dwFlags);
    return res;
}

/***************************************************************************
 *                              MCIQTZ_mciSetAudio              [internal]
 */
static DWORD MCIQTZ_mciSetAudio(UINT wDevID, DWORD dwFlags, LPMCI_DGV_SETAUDIO_PARMSW lpParms)
{
    WINE_MCIQTZ *wma;
    DWORD ret = 0;

    TRACE("(%04x, %08lx, %p)\n", wDevID, dwFlags, lpParms);

    if(!lpParms)
        return MCIERR_NULL_PARAMETER_BLOCK;

    wma = MCIQTZ_mciGetOpenDev(wDevID);
    if (!wma)
        return MCIERR_INVALID_DEVICE_ID;

    if (!(dwFlags & MCI_DGV_SETAUDIO_ITEM)) {
        FIXME("Unknown flags (%08lx)\n", dwFlags);
        return 0;
    }

    if (dwFlags & MCI_DGV_SETAUDIO_ITEM) {
        switch (lpParms->dwItem) {
        case MCI_DGV_SETAUDIO_VOLUME:
            if (dwFlags & MCI_DGV_SETAUDIO_VALUE) {
                if (lpParms->dwValue > 1000) {
                    ret = MCIERR_OUTOFRANGE;
                    break;
                }
                if (dwFlags & MCI_TEST)
                    break;
                if (MCIQTZ_is_wave_backend(wma))
                {
                    DWORD volume = MulDiv(lpParms->dwValue, 0xffff, 1000) & 0xffff;

                    wma->wave_volume = volume | (volume << 16);
                    if (wma->wave_out && waveOutSetVolume(wma->wave_out, wma->wave_volume) != MMSYSERR_NOERROR)
                        ret = MCIERR_INTERNAL;
                }
                else
                {
                    long vol;
                    HRESULT hr;

                    if (lpParms->dwValue != 0)
                        vol = (long)(2000.0 * (log10(lpParms->dwValue) - 3.0));
                    else
                        vol = -10000;
                    TRACE("Setting volume to %ld\n", vol);
                    hr = IBasicAudio_put_Volume(wma->audio, vol);
                    if (FAILED(hr)) {
                        WARN("Cannot set volume (hr = %lx)\n", hr);
                        ret = MCIERR_INTERNAL;
                    }
                }
            }
            break;
        default:
            FIXME("Unknown item %08lx\n", lpParms->dwItem);
            break;
        }
    }

    return ret;
}

/*======================================================================*
 *                          MCI QTZ entry points                        *
 *======================================================================*/

/**************************************************************************
 *                              DriverProc (MCIQTZ.@)
 */
LRESULT CALLBACK MCIQTZ_DriverProc(DWORD_PTR dwDevID, HDRVR hDriv, UINT wMsg,
                                   LPARAM dwParam1, LPARAM dwParam2)
{
    TRACE("(%08IX, %p, %08X, %08IX, %08IX)\n",
          dwDevID, hDriv, wMsg, dwParam1, dwParam2);

    switch (wMsg) {
        case DRV_LOAD:                  return 1;
        case DRV_FREE:                  return 1;
        case DRV_OPEN:                  return MCIQTZ_drvOpen((LPCWSTR)dwParam1, (LPMCI_OPEN_DRIVER_PARMSW)dwParam2);
        case DRV_CLOSE:                 return MCIQTZ_drvClose(dwDevID);
        case DRV_ENABLE:                return 1;
        case DRV_DISABLE:               return 1;
        case DRV_QUERYCONFIGURE:        return 1;
        case DRV_CONFIGURE:             return MCIQTZ_drvConfigure(dwDevID);
        case DRV_INSTALL:               return DRVCNF_RESTART;
        case DRV_REMOVE:                return DRVCNF_RESTART;
    }

    /* session instance */
    if (dwDevID == 0xFFFFFFFF)
        return 1;

    switch (wMsg) {
        case MCI_OPEN_DRIVER:   return MCIQTZ_mciOpen      (dwDevID, dwParam1, (LPMCI_DGV_OPEN_PARMSW)     dwParam2);
        case MCI_CLOSE_DRIVER:  return MCIQTZ_mciClose     (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)       dwParam2);
        case MCI_PLAY:          return MCIQTZ_mciPlay      (dwDevID, dwParam1, (LPMCI_PLAY_PARMS)          dwParam2);
        case MCI_SEEK:          return MCIQTZ_mciSeek      (dwDevID, dwParam1, (LPMCI_SEEK_PARMS)          dwParam2);
        case MCI_STOP:          return MCIQTZ_mciStop      (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)       dwParam2);
        case MCI_PAUSE:         return MCIQTZ_mciPause     (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)       dwParam2);
        case MCI_RESUME:        return MCIQTZ_mciResume    (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)       dwParam2);
        case MCI_GETDEVCAPS:    return MCIQTZ_mciGetDevCaps(dwDevID, dwParam1, (LPMCI_GETDEVCAPS_PARMS)    dwParam2);
        case MCI_SET:           return MCIQTZ_mciSet       (dwDevID, dwParam1, (LPMCI_DGV_SET_PARMS)       dwParam2);
        case MCI_STATUS:        return MCIQTZ_mciStatus    (dwDevID, dwParam1, (LPMCI_DGV_STATUS_PARMSW)   dwParam2);
        case MCI_WHERE:         return MCIQTZ_mciWhere     (dwDevID, dwParam1, (LPMCI_DGV_RECT_PARMS)      dwParam2);
        /* Digital Video specific */
        case MCI_SETAUDIO:      return MCIQTZ_mciSetAudio  (dwDevID, dwParam1, (LPMCI_DGV_SETAUDIO_PARMSW) dwParam2);
        case MCI_UPDATE:
            return MCIQTZ_mciUpdate(dwDevID, dwParam1, (LPMCI_DGV_UPDATE_PARMS)dwParam2);
        case MCI_WINDOW:
            return MCIQTZ_mciWindow(dwDevID, dwParam1, (LPMCI_DGV_WINDOW_PARMSW)dwParam2);
        case MCI_PUT:
            return MCIQTZ_mciPut(dwDevID, dwParam1, (MCI_GENERIC_PARMS*)dwParam2);
        case MCI_RECORD:
        case MCI_INFO:
        case MCI_LOAD:
        case MCI_SAVE:
        case MCI_FREEZE:
        case MCI_REALIZE:
        case MCI_UNFREEZE:
        case MCI_STEP:
        case MCI_COPY:
        case MCI_CUT:
        case MCI_DELETE:
        case MCI_PASTE:
        case MCI_CUE:
        /* Digital Video specific */
        case MCI_CAPTURE:
        case MCI_MONITOR:
        case MCI_RESERVE:
        case MCI_SIGNAL:
        case MCI_SETVIDEO:
        case MCI_QUALITY:
        case MCI_LIST:
        case MCI_UNDO:
        case MCI_CONFIGURE:
        case MCI_RESTORE:
            FIXME("Unimplemented command [%08X]\n", wMsg);
            break;
        case MCI_SPIN:
        case MCI_ESCAPE:
            WARN("Unsupported command [%08X]\n", wMsg);
            break;
        case MCI_OPEN:
        case MCI_CLOSE:
            FIXME("Shouldn't receive a MCI_OPEN or CLOSE message\n");
            break;
        default:
            TRACE("Sending msg [%08X] to default driver proc\n", wMsg);
            return DefDriverProc(dwDevID, hDriv, wMsg, dwParam1, dwParam2);
    }

    return MCIERR_UNRECOGNIZED_COMMAND;
}
