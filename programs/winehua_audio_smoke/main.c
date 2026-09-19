/*
 * WineHua audio smoke test.
 */

#define COBJMACROS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <initguid.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "../winehua_smoke_protocol.h"

#define MAX_WAV_SIZE (32 * 1024 * 1024)
#define PRIMARY_MP3_PATH "Z:\\Documents\\Media\\Alarm01.mp3"
#define FALLBACK_MP3_PATH "C:\\Documents\\Media\\Alarm01.mp3"
#define ROOT_MP3_PATH "C:\\Alarm01.mp3"
#define LOCAL_MP3_PATH "Alarm01.mp3"
#define PRIMARY_WAV_PATH "Z:\\Documents\\Media\\Alarm01.wav"
#define FALLBACK_WAV_PATH "C:\\Documents\\Media\\Alarm01.wav"
#define LOCAL_WAV_PATH "Alarm01.wav"

struct wav_file
{
    WAVEFORMATEX *format;
    DWORD format_size;
    BYTE *data;
    DWORD data_size;
    char path[MAX_PATH];
};

static DWORD read_u32le(const BYTE *data)
{
    return (DWORD)data[0] | ((DWORD)data[1] << 8) | ((DWORD)data[2] << 16) | ((DWORD)data[3] << 24);
}

static DWORD align_down(DWORD value, DWORD alignment)
{
    return alignment ? value - value % alignment : value;
}

static void free_wav_file(struct wav_file *wav)
{
    if (!wav) return;
    HeapFree(GetProcessHeap(), 0, wav->format);
    HeapFree(GetProcessHeap(), 0, wav->data);
    memset(wav, 0, sizeof(*wav));
}

static BOOL load_wav_file(const char *path, struct wav_file *wav)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    BYTE *file_bytes = NULL;
    DWORD bytes_read = 0;
    DWORD format_offset = 0, format_size = 0, data_offset = 0, data_size = 0;
    DWORD offset, next, copy_size;
    BOOL ok = FALSE;

    memset(wav, 0, sizeof(*wav));
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;

    if (!GetFileSizeEx(file, &size) || size.QuadPart < 44 || size.QuadPart > MAX_WAV_SIZE) goto done;

    file_bytes = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    if (!file_bytes) goto done;

    if (!ReadFile(file, file_bytes, (DWORD)size.QuadPart, &bytes_read, NULL) || bytes_read != size.QuadPart) goto done;
    if (memcmp(file_bytes, "RIFF", 4) || memcmp(file_bytes + 8, "WAVE", 4)) goto done;

    for (offset = 12; offset + 8 <= (DWORD)size.QuadPart; offset = next)
    {
        DWORD chunk_size = read_u32le(file_bytes + offset + 4);

        if ((DWORD)size.QuadPart - offset < 8 || chunk_size > (DWORD)size.QuadPart - offset - 8) break;
        next = offset + 8 + chunk_size + (chunk_size & 1);

        if (!memcmp(file_bytes + offset, "fmt ", 4) && chunk_size >= 16 && !format_offset)
        {
            format_offset = offset + 8;
            format_size = chunk_size;
        }
        else if (!memcmp(file_bytes + offset, "data", 4) && !data_offset)
        {
            data_offset = offset + 8;
            data_size = chunk_size;
        }
    }

    if (!format_offset || !data_offset) goto done;

    copy_size = format_size < sizeof(WAVEFORMATEX) ? sizeof(WAVEFORMATEX) : format_size;
    wav->format = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, copy_size);
    if (!wav->format) goto done;

    memcpy(wav->format, file_bytes + format_offset, format_size);
    wav->format_size = copy_size;
    if (format_size == 16) wav->format->cbSize = 0;
    if (!wav->format->nBlockAlign) goto done;

    data_size = align_down(data_size, wav->format->nBlockAlign);
    if (!data_size) goto done;

    wav->data = HeapAlloc(GetProcessHeap(), 0, data_size);
    if (!wav->data) goto done;

    memcpy(wav->data, file_bytes + data_offset, data_size);
    wav->data_size = data_size;
    lstrcpynA(wav->path, path, MAX_PATH);
    ok = TRUE;

done:
    HeapFree(GetProcessHeap(), 0, file_bytes);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok) free_wav_file(wav);
    return ok;
}

static BOOL load_alarm_wav(struct wav_file *wav)
{
    static const char *const paths[] = {PRIMARY_WAV_PATH, FALLBACK_WAV_PATH, LOCAL_WAV_PATH};
    unsigned int i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    {
        if (load_wav_file(paths[i], wav))
        {
            fprintf(stderr, "winehua_audio_smoke: using %s\n", wav->path);
            return TRUE;
        }
    }

    fprintf(stderr, "winehua_audio_smoke: failed to open %s or fallbacks\n", PRIMARY_WAV_PATH);
    return FALSE;
}

static BOOL file_exists(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static int play_mp3_via_mci(const char *path)
{
    static const char alias_name[] = "winehua_mp3";
    char cmd[2 * MAX_PATH + 64];
    char error_text[256];
    MCIERROR error;

    snprintf(cmd, sizeof(cmd), "close %s", alias_name);
    mciSendStringA(cmd, NULL, 0, NULL);

    snprintf(cmd, sizeof(cmd), "open \"%s\" type mpegvideo alias %s", path, alias_name);
    error = mciSendStringA(cmd, NULL, 0, NULL);
    if (error)
    {
        snprintf(cmd, sizeof(cmd), "open \"%s\" alias %s", path, alias_name);
        error = mciSendStringA(cmd, NULL, 0, NULL);
    }

    if (error)
    {
        if (!mciGetErrorStringA(error, error_text, sizeof(error_text)))
            snprintf(error_text, sizeof(error_text), "MCI error %u", (unsigned int)error);
        fprintf(stderr, "winehua_audio_smoke: MCI open failed for %s: %s\n", path, error_text);
        return 4;
    }

    fprintf(stderr, "winehua_audio_smoke: using MP3 %s\n", path);

    snprintf(cmd, sizeof(cmd), "play %s wait", alias_name);
    error = mciSendStringA(cmd, NULL, 0, NULL);
    if (error)
    {
        if (!mciGetErrorStringA(error, error_text, sizeof(error_text)))
            snprintf(error_text, sizeof(error_text), "MCI error %u", (unsigned int)error);
        fprintf(stderr, "winehua_audio_smoke: MCI play failed for %s: %s\n", path, error_text);
        snprintf(cmd, sizeof(cmd), "close %s", alias_name);
        mciSendStringA(cmd, NULL, 0, NULL);
        return 5;
    }

    snprintf(cmd, sizeof(cmd), "close %s", alias_name);
    mciSendStringA(cmd, NULL, 0, NULL);
    fprintf(stderr, "winehua_audio_smoke: MP3 playback complete\n");
    return 0;
}

static int play_alarm_mp3(void)
{
    static const char *const paths[] = {PRIMARY_MP3_PATH, FALLBACK_MP3_PATH, ROOT_MP3_PATH, LOCAL_MP3_PATH};
    unsigned int i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
    {
        if (file_exists(paths[i]))
            return play_mp3_via_mci(paths[i]);
    }

    fprintf(stderr, "winehua_audio_smoke: no Alarm01.mp3 found in mapped Wine paths\n");
    return -1;
}

static int play_wav_via_audioclient(const struct wav_file *wav)
{
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    UINT32 buffer_frames = 0;
    UINT32 total_frames = wav->data_size / wav->format->nBlockAlign;
    UINT32 offset_frames = 0;
    UINT32 padding = 0;
    REFERENCE_TIME duration = 10000000;
    BYTE *dst = NULL;
    HRESULT hr;
    BOOL com_initialized = FALSE;

    hr = CoInitialize(NULL);
    if (SUCCEEDED(hr))
        com_initialized = TRUE;
    else if (hr != RPC_E_CHANGED_MODE)
    {
        fprintf(stderr, "CoInitialize failed: 0x%08lx\n", hr);
        return 2;
    }

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr))
    {
        fprintf(stderr, "CoCreateInstance(IMMDeviceEnumerator) failed: 0x%08lx\n", hr);
        goto fail;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr))
    {
        fprintf(stderr, "GetDefaultAudioEndpoint failed: 0x%08lx\n", hr);
        goto fail;
    }

    hr = IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client);
    if (FAILED(hr))
    {
        fprintf(stderr, "IMMDevice_Activate(IAudioClient) failed: 0x%08lx\n", hr);
        goto fail;
    }

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, duration, 0, wav->format, NULL);
    if (FAILED(hr))
    {
        fprintf(stderr, "IAudioClient_Initialize failed: 0x%08lx\n", hr);
        goto fail;
    }

    hr = IAudioClient_GetBufferSize(client, &buffer_frames);
    if (FAILED(hr))
    {
        fprintf(stderr, "IAudioClient_GetBufferSize failed: 0x%08lx\n", hr);
        goto fail;
    }

    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render);
    if (FAILED(hr))
    {
        fprintf(stderr, "IAudioClient_GetService(IAudioRenderClient) failed: 0x%08lx\n", hr);
        goto fail;
    }

    fprintf(stderr, "winehua_audio_smoke: format tag=%u channels=%u rate=%lu bits=%u frames=%u buffer=%u\n",
            wav->format->wFormatTag, wav->format->nChannels, wav->format->nSamplesPerSec,
            wav->format->wBitsPerSample, total_frames, buffer_frames);

    if (buffer_frames > total_frames) buffer_frames = total_frames;
    if (buffer_frames)
    {
        hr = IAudioRenderClient_GetBuffer(render, buffer_frames, &dst);
        if (FAILED(hr))
        {
            fprintf(stderr, "IAudioRenderClient_GetBuffer(initial) failed: 0x%08lx\n", hr);
            goto fail;
        }

        memcpy(dst, wav->data, buffer_frames * wav->format->nBlockAlign);
        hr = IAudioRenderClient_ReleaseBuffer(render, buffer_frames, 0);
        if (FAILED(hr))
        {
            fprintf(stderr, "IAudioRenderClient_ReleaseBuffer(initial) failed: 0x%08lx\n", hr);
            goto fail;
        }

        offset_frames = buffer_frames;
    }

    hr = IAudioClient_Start(client);
    if (FAILED(hr))
    {
        fprintf(stderr, "IAudioClient_Start failed: 0x%08lx\n", hr);
        goto fail;
    }

    while (offset_frames < total_frames)
    {
        UINT32 available_frames;
        UINT32 chunk_frames;

        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr))
        {
            fprintf(stderr, "IAudioClient_GetCurrentPadding failed: 0x%08lx\n", hr);
            goto fail_stop;
        }

        if (padding >= buffer_frames)
        {
            Sleep(5);
            continue;
        }

        available_frames = buffer_frames - padding;
        chunk_frames = total_frames - offset_frames;
        if (chunk_frames > available_frames) chunk_frames = available_frames;
        if (!chunk_frames)
        {
            Sleep(5);
            continue;
        }

        hr = IAudioRenderClient_GetBuffer(render, chunk_frames, &dst);
        if (FAILED(hr))
        {
            fprintf(stderr, "IAudioRenderClient_GetBuffer failed: 0x%08lx\n", hr);
            goto fail_stop;
        }

        memcpy(dst, wav->data + offset_frames * wav->format->nBlockAlign,
               chunk_frames * wav->format->nBlockAlign);
        hr = IAudioRenderClient_ReleaseBuffer(render, chunk_frames, 0);
        if (FAILED(hr))
        {
            fprintf(stderr, "IAudioRenderClient_ReleaseBuffer failed: 0x%08lx\n", hr);
            goto fail_stop;
        }

        offset_frames += chunk_frames;
    }

    do
    {
        Sleep(10);
        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr))
        {
            fprintf(stderr, "IAudioClient_GetCurrentPadding(drain) failed: 0x%08lx\n", hr);
            goto fail_stop;
        }
    } while (padding);

    IAudioClient_Stop(client);
    IAudioRenderClient_Release(render);
    IAudioClient_Release(client);
    IMMDevice_Release(device);
    IMMDeviceEnumerator_Release(enumerator);
    if (com_initialized) CoUninitialize();

    fprintf(stderr, "winehua_audio_smoke: playback complete\n");
    return 0;

fail_stop:
    IAudioClient_Stop(client);
fail:
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (com_initialized) CoUninitialize();
    return 3;
}

static double smoke_sqrt(double value)
{
    double estimate = value > 1.0 ? value : 1.0;
    int i;
    for (i = 0; i < 16; ++i) estimate = 0.5 * (estimate + value / estimate);
    return estimate;
}

static BOOL generate_deterministic_pcm(struct wav_file *wav, DWORD seconds,
                                       double *out_rms, int *out_peak)
{
    const DWORD sample_rate = 48000;
    const WORD channels = 2;
    const DWORD frames = sample_rate * seconds;
    DWORD frame;
    unsigned long long sum_squares = 0;
    int peak = 0;
    short *samples;

    memset(wav, 0, sizeof(*wav));
    wav->format = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*wav->format));
    wav->data_size = frames * channels * sizeof(short);
    wav->data = HeapAlloc(GetProcessHeap(), 0, wav->data_size);
    if (!wav->format || !wav->data)
    {
        free_wav_file(wav);
        return FALSE;
    }

    wav->format_size = sizeof(*wav->format);
    wav->format->wFormatTag = WAVE_FORMAT_PCM;
    wav->format->nChannels = channels;
    wav->format->nSamplesPerSec = sample_rate;
    wav->format->wBitsPerSample = 16;
    wav->format->nBlockAlign = channels * sizeof(short);
    wav->format->nAvgBytesPerSec = sample_rate * wav->format->nBlockAlign;
    lstrcpynA(wav->path, "generated:triangle-440hz-s16le-stereo", MAX_PATH);

    samples = (short *)wav->data;
    for (frame = 0; frame < frames; ++frame)
    {
        unsigned int phase = (unsigned int)(((unsigned long long)frame * 440ULL * 65536ULL / sample_rate) & 0xffff);
        int triangle = phase < 32768 ? (int)phase * 2 - 32768 : 98303 - (int)phase * 2;
        int sample = triangle * 12000 / 32768;
        int absolute = sample < 0 ? -sample : sample;
        samples[frame * 2] = (short)sample;
        samples[frame * 2 + 1] = (short)sample;
        sum_squares += (unsigned long long)((long long)sample * sample) * 2ULL;
        if (absolute > peak) peak = absolute;
    }

    *out_rms = smoke_sqrt((double)sum_squares / (double)(frames * channels));
    *out_peak = peak;
    return TRUE;
}

int main(int argc, char **argv)
{
    struct wav_file wav;
    struct winehua_smoke_options smoke;
    char metrics[768];
    double rms = 0.0;
    int peak = 0;
    int ret;

    if (!winehua_smoke_parse_options(&smoke, argc, argv, 3)) return 6;
    winehua_smoke_write_result(&smoke, "STARTED", "startup", "audio smoke starting", "{}");

    if (smoke.automation)
    {
        if (!generate_deterministic_pcm(&wav, smoke.seconds, &rms, &peak))
        {
            winehua_smoke_write_result(&smoke, "FAIL", "startup", "deterministic PCM allocation failed", "{}");
            return 1;
        }
        ret = play_wav_via_audioclient(&wav);
        snprintf(metrics, sizeof(metrics),
                 "{\"sampleRate\":48000,\"channels\":2,\"framesSubmitted\":%lu,"
                 "\"rms\":%.3f,\"peak\":%d,\"underrunCount\":-1,"
                 "\"hostConsumptionVerified\":%s}",
                 (unsigned long)(wav.data_size / wav.format->nBlockAlign), rms, peak,
                 ret == 0 ? "true" : "false");
        free_wav_file(&wav);
        winehua_smoke_write_result(&smoke, ret == 0 ? "PASS" : "FAIL", "audio",
                                   ret == 0 ? "AudioClient stream drained through AudioBroker" :
                                              "AudioClient playback failed",
                                   metrics);
        return ret;
    }

    ret = play_alarm_mp3();
    if (ret == 0) return 0;
    if (ret > 0)
        fprintf(stderr, "winehua_audio_smoke: MP3 playback unavailable, falling back to WAV path\n");

    if (!load_alarm_wav(&wav)) return 1;

    ret = play_wav_via_audioclient(&wav);
    free_wav_file(&wav);
    return ret;
}
