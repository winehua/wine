/*  			DirectSound
 *
 * Copyright 1998 Marcus Meissner
 * Copyright 1998 Rob Riggs
 * Copyright 2000-2002 TransGaming Technologies, Inc.
 * Copyright 2007 Peter Dons Tychsen
 * Copyright 2007 Maarten Lankhorst
 * Copyright 2011 Owen Rudge for CodeWeavers
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

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>	/* Insomnia - pow() function */
#ifdef __SSE__
#include <xmmintrin.h>
#endif

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "mmsystem.h"
#include "wingdi.h"
#include "mmreg.h"
#include "wine/debug.h"
#include "dsound.h"
#include "ks.h"
#include "ksmedia.h"
#include "dsound_private.h"
#include "fir.h"

WINE_DEFAULT_DEBUG_CHANNEL(dsound);

#define FREQ_ADJUST_SHIFT 32
#define FIXED_0_32_TO_FLOAT(x) ((int)((x) >> 1) * (1.0f / (1ll << 31)))

#define D0_IDLE 0
#define D0_CAPTURING 1
#define D0_READY 2
#define D0_WRITING 3
#define D0_WRITTEN 4

static LONG g_d0_next_id;
static int g_d0_enabled = -1;

static void d0_log(const char *fmt, ...)
{
    va_list ap;

    fputs("[DSOUND-DIAG] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

static int d0_enabled(void)
{
    const char *e;

    if (g_d0_enabled >= 0) return g_d0_enabled;
    e = getenv("WINEHUA_AUDIO_D0");
    g_d0_enabled = (e && e[0] == '0' && !e[1]) ? 0 : 1;
    d0_log("enabled=%d env=%s\n", g_d0_enabled, e ? e : "(null)");
    return g_d0_enabled;
}

static const char *d0_dump_dir(void)
{
    static char dir[512];
    static int ready;
    const char *e;

    if (ready) return dir;
    e = getenv("WINEHUA_AUDIO_W01_DIR");
    if (e && e[0])
    {
        snprintf(dir, sizeof(dir), "%s", e);
        ready = 1;
        return dir;
    }
    e = getenv("HOME");
    if (e && e[0])
    {
        snprintf(dir, sizeof(dir), "%s/logs", e);
        ready = 1;
        return dir;
    }
    GetTempPathA(sizeof(dir), dir);
    ready = 1;
    return dir;
}

static unsigned d0_count_audible(const DirectSoundDevice *device)
{
    unsigned n = 0;
    int i;
    IDirectSoundBufferImpl *dsb;

    for (i = 0; i < device->nrofbuffers; i++)
    {
        dsb = device->buffers[i];
        if (dsb && dsb->buflen && dsb->state && dsb->state != STATE_STOPPED &&
            secondarybuffer_is_audible(dsb))
            n++;
    }
    return n;
}

static void d0_observe_channel(float sample, int ch, DirectSoundDevice *device, BOOL capturing)
{
    float prev, adelta, a;

    if (ch > 1) ch = 1;
    a = (float)fabs((double)sample);
    if (a > device->d0_hz_peak[ch]) device->d0_hz_peak[ch] = a;
    device->d0_hz_sumsq[ch] += (double)sample * (double)sample;
    device->d0_hz_samples[ch]++;
    if (a >= 1.0f) device->d0_hz_over1[ch]++;
    if (a >= 1.25f) device->d0_hz_over125[ch]++;
    if (a >= 1.5f) device->d0_hz_over15[ch]++;
    if (a >= 2.0f) device->d0_hz_over2[ch]++;
    if (device->d0_hz_have_prev[ch])
    {
        prev = device->d0_hz_prev[ch];
        adelta = fabsf(sample - prev);
        if (adelta > device->d0_hz_maxdelta[ch]) device->d0_hz_maxdelta[ch] = adelta;
        if ((prev > 0.5f && sample < -0.5f) || (prev < -0.5f && sample > 0.5f))
            device->d0_hz_signflip[ch]++;
    }
    device->d0_hz_prev[ch] = sample;
    device->d0_hz_have_prev[ch] = TRUE;

    if (!capturing) return;
    if (a > device->d0_cap_peak[ch]) device->d0_cap_peak[ch] = a;
    device->d0_cap_sumsq[ch] += (double)sample * (double)sample;
    if (a >= 1.0f) device->d0_cap_over1[ch]++;
    if (a >= 1.25f) device->d0_cap_over125[ch]++;
    if (a >= 1.5f) device->d0_cap_over15[ch]++;
    if (a >= 2.0f) device->d0_cap_over2[ch]++;
    if (device->d0_cap_have_prev[ch])
    {
        prev = device->d0_cap_prev[ch];
        adelta = fabsf(sample - prev);
        if (adelta > device->d0_cap_maxdelta[ch]) device->d0_cap_maxdelta[ch] = adelta;
        if ((prev > 0.5f && sample < -0.5f) || (prev < -0.5f && sample > 0.5f))
            device->d0_cap_signflip[ch]++;
    }
    device->d0_cap_prev[ch] = sample;
    device->d0_cap_have_prev[ch] = TRUE;
}

static DWORD WINAPI d0_dump_thread(void *arg)
{
    DirectSoundDevice *device = arg;
    FILE *fp;
    char path[768], json_path[768];
    const char *dir;
    UINT id;
    DWORD written, ch, rate, bits, tag, priolevel, nbufs;
    BOOL forcewave, mixfloat, has_norm;
    float peak0, peak1, delta0, delta1;
    DWORD over1_0, over1_1, over2_0, over2_1, flip0, flip1, audible;
    double rms0, rms1;

    id = device->d0_id;
    written = device->d0_written;
    ch = device->pwfx ? device->pwfx->nChannels : 2;
    rate = device->pwfx ? device->pwfx->nSamplesPerSec : 48000;
    bits = device->pwfx ? device->pwfx->wBitsPerSample : 16;
    tag = device->pwfx ? device->pwfx->wFormatTag : 0;
    priolevel = device->priolevel;
    nbufs = device->nrofbuffers;
    forcewave = device->d0_forcewave;
    mixfloat = device->d0_mixfloat;
    has_norm = device->normfunction != NULL;
    peak0 = device->d0_cap_peak[0];
    peak1 = device->d0_cap_peak[1];
    delta0 = device->d0_cap_maxdelta[0];
    delta1 = device->d0_cap_maxdelta[1];
    over1_0 = device->d0_cap_over1[0];
    over1_1 = device->d0_cap_over1[1];
    over2_0 = device->d0_cap_over2[0];
    over2_1 = device->d0_cap_over2[1];
    flip0 = device->d0_cap_signflip[0];
    flip1 = device->d0_cap_signflip[1];
    audible = device->d0_cap_audible_max;
    rms0 = (ch && written) ? sqrt(device->d0_cap_sumsq[0] / (written / (ch ? ch : 1))) : 0;
    rms1 = (ch > 1 && written) ? sqrt(device->d0_cap_sumsq[1] / (written / ch)) : 0;

    dir = d0_dump_dir();
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s/m5_dsound_d%u_pre_norm_float32.raw", dir, id);
    snprintf(json_path, sizeof(json_path), "%s/m5_dsound_d%u_pre_norm.json", dir, id);
    d0_log("flush id=%u samples=%lu dir=%s\n", id, (unsigned long)written, dir);

    fp = fopen(path, "wb");
    if (fp)
    {
        if (written) fwrite(device->d0_buf, sizeof(float), written, fp);
        fclose(fp);
    }
    else d0_log("fopen raw errno=%d path=%s\n", errno, path);

    fp = fopen(json_path, "wb");
    if (fp)
    {
        fprintf(fp,
                "{\n"
                "  \"d0Id\": %u,\n"
                "  \"sampleRate\": %lu,\n"
                "  \"channels\": %lu,\n"
                "  \"bitsPerSample\": %lu,\n"
                "  \"formatTag\": %lu,\n"
                "  \"priolevel\": %lu,\n"
                "  \"forcewave\": %u,\n"
                "  \"mixfloat\": %u,\n"
                "  \"normfunction\": %u,\n"
                "  \"nrofbuffers\": %lu,\n"
                "  \"audibleMax\": %lu,\n"
                "  \"samples\": %lu,\n"
                "  \"peakL\": %.6f,\n"
                "  \"peakR\": %.6f,\n"
                "  \"rmsL\": %.6f,\n"
                "  \"rmsR\": %.6f,\n"
                "  \"maxDeltaL\": %.6f,\n"
                "  \"maxDeltaR\": %.6f,\n"
                "  \"over1L\": %lu,\n"
                "  \"over1R\": %lu,\n"
                "  \"over1_25L\": %lu,\n"
                "  \"over1_25R\": %lu,\n"
                "  \"over1_5L\": %lu,\n"
                "  \"over1_5R\": %lu,\n"
                "  \"over2L\": %lu,\n"
                "  \"over2R\": %lu,\n"
                "  \"signFlipL\": %lu,\n"
                "  \"signFlipR\": %lu,\n"
                "  \"rawPath\": \"%s\"\n"
                "}\n",
                id, (unsigned long)rate, (unsigned long)ch, (unsigned long)bits,
                (unsigned long)tag, (unsigned long)priolevel, forcewave ? 1 : 0,
                mixfloat ? 1 : 0, has_norm ? 1 : 0, (unsigned long)nbufs,
                (unsigned long)audible, (unsigned long)written,
                peak0, peak1, rms0, rms1, delta0, delta1,
                (unsigned long)over1_0, (unsigned long)over1_1,
                (unsigned long)device->d0_cap_over125[0], (unsigned long)device->d0_cap_over125[1],
                (unsigned long)device->d0_cap_over15[0], (unsigned long)device->d0_cap_over15[1],
                (unsigned long)over2_0, (unsigned long)over2_1,
                (unsigned long)flip0, (unsigned long)flip1, path);
        fclose(fp);
        d0_log("wrote id=%u samples=%lu json=%s\n", id, (unsigned long)written, json_path);
    }
    else d0_log("fopen json errno=%d path=%s\n", errno, json_path);

    device->d0_state = D0_WRITTEN;
    return 0;
}

void DSOUND_D0_NotePrimary(DirectSoundDevice *device, BOOL forcewave, BOOL mixfloat)
{
    if (!device || !d0_enabled()) return;
    device->d0_forcewave = forcewave;
    device->d0_mixfloat = mixfloat;
    if (!device->d0_id)
        device->d0_id = (UINT)InterlockedIncrement(&g_d0_next_id);
    d0_log("primary id=%u forcewave=%d mixfloat=%d bits=%u rate=%u ch=%u tag=%u priolevel=%lu norm=%d nbufs=%d\n",
           device->d0_id, forcewave ? 1 : 0, mixfloat ? 1 : 0,
           device->pwfx ? device->pwfx->wBitsPerSample : 0,
           device->pwfx ? device->pwfx->nSamplesPerSec : 0,
           device->pwfx ? device->pwfx->nChannels : 0,
           device->pwfx ? device->pwfx->wFormatTag : 0,
           (unsigned long)device->priolevel,
           device->normfunction ? 1 : 0,
           device->nrofbuffers);
}

void DSOUND_D0_AfterMix(DirectSoundDevice *device, float *mix, DWORD frames, double mix_us)
{
    DWORD ch, i, n, room, copy, audible;
    ULONGLONG now;
    BOOL capturing;
    float *src;

    if (!device || !mix || !frames || !device->pwfx || !d0_enabled()) return;
    ch = device->pwfx->nChannels;
    if (!ch) return;
    n = frames * ch;
    src = mix;
    audible = d0_count_audible(device);

    if (!device->d0_buf)
    {
        DWORD rate = device->pwfx->nSamplesPerSec ? device->pwfx->nSamplesPerSec : 48000;
        device->d0_cap_samples = rate * ch * 2;
        if (device->d0_cap_samples < 8192) device->d0_cap_samples = 8192;
        device->d0_buf = calloc(device->d0_cap_samples, sizeof(float));
        if (!device->d0_buf) return;
        if (!device->d0_id)
            device->d0_id = (UINT)InterlockedIncrement(&g_d0_next_id);
        device->d0_state = D0_CAPTURING;
        device->d0_hz_tick = GetTickCount64();
        d0_log("alloc id=%u samples=%lu rate=%u ch=%u dir=%s\n",
               device->d0_id, (unsigned long)device->d0_cap_samples,
               device->pwfx->nSamplesPerSec, ch, d0_dump_dir());
    }

    capturing = (device->d0_state == D0_CAPTURING && device->d0_buf);
    if (capturing)
    {
        room = device->d0_cap_samples - device->d0_written;
        copy = n < room ? n : room;
        memcpy(device->d0_buf + device->d0_written, src, copy * sizeof(float));
        device->d0_written += copy;
        if (audible > device->d0_cap_audible_max)
            device->d0_cap_audible_max = audible;
        if (device->d0_written >= device->d0_cap_samples)
            device->d0_state = D0_READY;
    }

    device->d0_hz_calls++;
    device->d0_hz_frames += frames;
    if (audible > device->d0_hz_audible_max) device->d0_hz_audible_max = audible;
    device->d0_hz_mix_us += mix_us;
    if (mix_us > device->d0_hz_mix_max_us) device->d0_hz_mix_max_us = mix_us;

    for (i = 0; i < n; i++)
        d0_observe_channel(src[i], (int)(i % ch), device, capturing);

    now = GetTickCount64();
    if (!device->d0_hz_tick) device->d0_hz_tick = now;
    if (now - device->d0_hz_tick >= 1000)
    {
        DWORD sl = device->d0_hz_samples[0] ? device->d0_hz_samples[0] : 1;
        DWORD sr = device->d0_hz_samples[1] ? device->d0_hz_samples[1] : 1;
        d0_log("id=%u buffers=%d audible=%lu mixCalls=%lu frames=%lu mixAvgUs=%.1f mixMaxUs=%.1f "
               "preNormPeakL=%.4f preNormPeakR=%.4f maxDeltaL=%.4f maxDeltaR=%.4f "
               "over1L=%lu over1R=%lu over1_25L=%lu over1_25R=%lu over1_5L=%lu over1_5R=%lu "
               "over2L=%lu over2R=%lu signFlipL=%lu signFlipR=%lu rmsL=%.4f rmsR=%.4f "
               "norm=%d forcewave=%d mixfloat=%d bits=%u priolevel=%lu\n",
               device->d0_id, device->nrofbuffers, (unsigned long)device->d0_hz_audible_max,
               (unsigned long)device->d0_hz_calls, (unsigned long)device->d0_hz_frames,
               device->d0_hz_calls ? device->d0_hz_mix_us / device->d0_hz_calls : 0.0,
               device->d0_hz_mix_max_us,
               device->d0_hz_peak[0], device->d0_hz_peak[1],
               device->d0_hz_maxdelta[0], device->d0_hz_maxdelta[1],
               (unsigned long)device->d0_hz_over1[0], (unsigned long)device->d0_hz_over1[1],
               (unsigned long)device->d0_hz_over125[0], (unsigned long)device->d0_hz_over125[1],
               (unsigned long)device->d0_hz_over15[0], (unsigned long)device->d0_hz_over15[1],
               (unsigned long)device->d0_hz_over2[0], (unsigned long)device->d0_hz_over2[1],
               (unsigned long)device->d0_hz_signflip[0], (unsigned long)device->d0_hz_signflip[1],
               sqrt(device->d0_hz_sumsq[0] / sl), sqrt(device->d0_hz_sumsq[1] / sr),
               device->normfunction ? 1 : 0, device->d0_forcewave ? 1 : 0,
               device->d0_mixfloat ? 1 : 0,
               device->pwfx->wBitsPerSample, (unsigned long)device->priolevel);
        memset(device->d0_hz_peak, 0, sizeof(device->d0_hz_peak));
        memset(device->d0_hz_maxdelta, 0, sizeof(device->d0_hz_maxdelta));
        memset(device->d0_hz_sumsq, 0, sizeof(device->d0_hz_sumsq));
        memset(device->d0_hz_samples, 0, sizeof(device->d0_hz_samples));
        memset(device->d0_hz_over1, 0, sizeof(device->d0_hz_over1));
        memset(device->d0_hz_over125, 0, sizeof(device->d0_hz_over125));
        memset(device->d0_hz_over15, 0, sizeof(device->d0_hz_over15));
        memset(device->d0_hz_over2, 0, sizeof(device->d0_hz_over2));
        memset(device->d0_hz_signflip, 0, sizeof(device->d0_hz_signflip));
        device->d0_hz_calls = 0;
        device->d0_hz_frames = 0;
        device->d0_hz_audible_max = 0;
        device->d0_hz_mix_us = 0;
        device->d0_hz_mix_max_us = 0;
        device->d0_hz_tick = now;
    }

    if (device->d0_state == D0_READY)
    {
        device->d0_state = D0_WRITING;
        device->d0_thread = CreateThread(NULL, 0, d0_dump_thread, device, 0, NULL);
        if (!device->d0_thread)
            d0_dump_thread(device);
    }
}

void DSOUND_D0_Destroy(DirectSoundDevice *device)
{
    if (!device) return;
    if (device->d0_state == D0_CAPTURING && device->d0_written)
    {
        device->d0_state = D0_READY;
        d0_dump_thread(device);
    }
    if (device->d0_thread)
    {
        WaitForSingleObject(device->d0_thread, 5000);
        CloseHandle(device->d0_thread);
        device->d0_thread = NULL;
    }
    free(device->d0_buf);
    device->d0_buf = NULL;
}

void DSOUND_RecalcVolPan(PDSVOLUMEPAN volpan)
{
	double temp;
	TRACE("(%p)\n",volpan);

	TRACE("Vol=%ld Pan=%ld\n", volpan->lVolume, volpan->lPan);
	/* the AmpFactors are expressed in 16.16 fixed point */

	if (volpan->lVolume == DSBVOLUME_MIN)
	{
		for (unsigned int i = 0; i < DS_MAX_CHANNELS; i++)
			volpan->dwTotalAmpFactor[i] = 0;
		TRACE("setting all channel volumes to 0\n");
		return;
	}
	/* FIXME: use calculated vol and pan ampfactors */
	temp = (double) (volpan->lVolume - (volpan->lPan > 0 ? volpan->lPan : 0));
	volpan->dwTotalAmpFactor[0] = (ULONG) (pow(2.0, temp / 600.0) * 0xffff);
	temp = (double) (volpan->lVolume + (volpan->lPan < 0 ? volpan->lPan : 0));
	volpan->dwTotalAmpFactor[1] = (ULONG) (pow(2.0, temp / 600.0) * 0xffff);

	TRACE("left = %lx, right = %lx\n", volpan->dwTotalAmpFactor[0], volpan->dwTotalAmpFactor[1]);
}

void DSOUND_AmpFactorToVolPan(PDSVOLUMEPAN volpan)
{
    double left,right;
    TRACE("(%p)\n",volpan);

    TRACE("left=%lx, right=%lx\n",volpan->dwTotalAmpFactor[0],volpan->dwTotalAmpFactor[1]);
    if (volpan->dwTotalAmpFactor[0]==0)
        left=-10000;
    else
        left=600 * log(((double)volpan->dwTotalAmpFactor[0]) / 0xffff) / log(2);
    if (volpan->dwTotalAmpFactor[1]==0)
        right=-10000;
    else
        right=600 * log(((double)volpan->dwTotalAmpFactor[1]) / 0xffff) / log(2);
    if (left<right)
        volpan->lVolume=right;
    else
        volpan->lVolume=left;
    if (volpan->lVolume < -10000)
        volpan->lVolume=-10000;
    volpan->lPan=right-left;
    if (volpan->lPan < -10000)
        volpan->lPan=-10000;

    TRACE("Vol=%ld Pan=%ld\n", volpan->lVolume, volpan->lPan);
}

/**
 * Recalculate the size for temporary buffer, and new writelead
 * Should be called when one of the following things occur:
 * - Primary buffer format is changed
 * - This buffer format (frequency) is changed
 */
void DSOUND_RecalcFormat(IDirectSoundBufferImpl *dsb)
{
	DWORD ichannels = dsb->pwfx->nChannels;
	DWORD ochannels = dsb->device->pwfx->nChannels;
	DWORD oldFreqAdjustDen = dsb->freqAdjustDen;
	WAVEFORMATEXTENSIBLE *pwfxe;
	BOOL ieee = FALSE;

	TRACE("(%p)\n",dsb);

	pwfxe = (WAVEFORMATEXTENSIBLE *) dsb->pwfx;
	dsb->freqAdjustNum = dsb->freq;
	dsb->freqAdjustDen = dsb->device->pwfx->nSamplesPerSec;

	if ((pwfxe->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) || ((pwfxe->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
	    && (IsEqualGUID(&pwfxe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))))
		ieee = TRUE;

	/**
	 * Recalculate FIR gain, which specifies what to multiply the FIR
	 * output by in order to attenuate it correctly.
	 */
	dsb->firgain = min(1.0f, dsb->freqAdjustDen / (float)dsb->freqAdjustNum);

	/* calculate the 10ms write lead */
	dsb->writelead = (dsb->freq / 100) * dsb->pwfx->nBlockAlign;
	dsb->maxwritelead = (DSBFREQUENCY_MAX / 100) * dsb->pwfx->nBlockAlign;

	if (oldFreqAdjustDen)
		dsb->freqAccNum = (dsb->freqAccNum * (LONG64)dsb->freqAdjustDen +
				oldFreqAdjustDen / 2) / oldFreqAdjustDen;

	dsb->get_aux = ieee ? getbpp[4] : getbpp[dsb->pwfx->wBitsPerSample/8 - 1];
	dsb->put_aux = putieee32;

	dsb->get = dsb->get_aux;
	dsb->put = dsb->put_aux;

	if (ichannels == ochannels)
	{
		dsb->mix_channels = ichannels;
		if (ichannels > 32) {
			FIXME("Copying %lu channels is unsupported, limiting to first 32\n", ichannels);
			dsb->mix_channels = 32;
		}
	}
	else if (ichannels == 1)
	{
		dsb->mix_channels = 1;

		if (ochannels == 2)
			dsb->put = put_mono2stereo;
		else if (ochannels == 4)
			dsb->put = put_mono2quad;
		else if (ochannels == 6)
			dsb->put = put_mono2surround51;
	}
	else if (ochannels == 1)
	{
		dsb->mix_channels = 1;
		dsb->get = get_mono;
	}
	else if (ichannels == 2 && ochannels == 4)
	{
		dsb->mix_channels = 2;
		dsb->put = put_stereo2quad;
	}
	else if (ichannels == 2 && ochannels == 6)
	{
		dsb->mix_channels = 2;
		dsb->put = put_stereo2surround51;
	}
	else if (ichannels == 6 && ochannels == 2)
	{
		dsb->mix_channels = 6;
		dsb->put = put_surround512stereo;
		dsb->put_aux = putieee32_sum;
	}
	else if (ichannels == 8 && ochannels == 2)
	{
		dsb->mix_channels = 8;
		dsb->put = put_surround712stereo;
		dsb->put_aux = putieee32_sum;
	}
	else if (ichannels == 4 && ochannels == 2)
	{
		dsb->mix_channels = 4;
		dsb->put = put_quad2stereo;
		dsb->put_aux = putieee32_sum;
	}
	else
	{
		if (ichannels > 2)
			FIXME("Conversion from %lu to %lu channels is not implemented, falling back to stereo\n", ichannels, ochannels);
		dsb->mix_channels = 2;
	}
}

/**
 * Check for application callback requests for when the play position
 * reaches certain points.
 *
 * The offsets that will be triggered will be those between the recorded
 * "last played" position for the buffer (i.e. dsb->playpos) and "len" bytes
 * beyond that position.
 */
void DSOUND_CheckEvent(const IDirectSoundBufferImpl *dsb, DWORD playpos, int len)
{
    int first, left, right, check;

    if(dsb->nrofnotifies == 0)
        return;

    if(dsb->state == STATE_STOPPED){
        TRACE("Stopped...\n");
        /* DSBPN_OFFSETSTOP notifies are always at the start of the sorted array */
        for(left = 0; left < dsb->nrofnotifies; ++left){
            if(dsb->notifies[left].dwOffset != DSBPN_OFFSETSTOP)
                break;

            TRACE("Signalling %p\n", dsb->notifies[left].hEventNotify);
            SetEvent(dsb->notifies[left].hEventNotify);
        }
    }

    for(first = 0; first < dsb->nrofnotifies && dsb->notifies[first].dwOffset == DSBPN_OFFSETSTOP; ++first)
        ;

    if(first == dsb->nrofnotifies)
        return;

    check = left = first;
    right = dsb->nrofnotifies - 1;

    /* find leftmost notify that is greater than playpos */
    while(left != right){
        check = left + (right - left) / 2;
        if(dsb->notifies[check].dwOffset < playpos)
            left = check + 1;
        else if(dsb->notifies[check].dwOffset > playpos)
            right = check;
        else{
            left = check;
            break;
        }
    }

    TRACE("Not stopped: first notify: %u (%lu), left notify: %u (%lu), range: [%lu,%lu)\n",
            first, dsb->notifies[first].dwOffset,
            left, dsb->notifies[left].dwOffset,
            playpos, (playpos + len) % dsb->buflen);

    /* send notifications in range */
    if(dsb->notifies[left].dwOffset >= playpos){
        for(check = left; check < dsb->nrofnotifies; ++check){
            if(dsb->notifies[check].dwOffset >= playpos + len)
                break;

            TRACE("Signalling %p (%lu)\n", dsb->notifies[check].hEventNotify, dsb->notifies[check].dwOffset);
            SetEvent(dsb->notifies[check].hEventNotify);
        }
    }

    if(playpos + len > dsb->buflen){
        for(check = first; check < left; ++check){
            if(dsb->notifies[check].dwOffset >= (playpos + len) % dsb->buflen)
                break;

            TRACE("Signalling %p (%lu)\n", dsb->notifies[check].hEventNotify, dsb->notifies[check].dwOffset);
            SetEvent(dsb->notifies[check].hEventNotify);
        }
    }
}

static inline float get_current_sample(const IDirectSoundBufferImpl *dsb,
        BYTE *buffer, DWORD buflen, DWORD mixpos, DWORD channel)
{
    if (mixpos >= buflen && !(dsb->playflags & DSBPLAY_LOOPING))
        return 0.0f;
    return dsb->get(dsb, buffer + (mixpos % buflen), channel);
}

#ifdef __SSE__

/**
 * Note that this function will overwrite up to FIR_WIDTH - 1 frames before and
 * after output[].
 */
void downsample_sse(LONG64 opos_num, DWORD opos_num_step, float rem_float, float rem_step_float,
        float firgain_float, UINT required_input, float *input, float *output)
{
    __m128 rem = _mm_set1_ps(rem_float);
    __m128 rem_step = _mm_set1_ps(rem_step_float);
    __m128 firgain = _mm_set_ss(firgain_float);
    __m128 one = _mm_set1_ps(1.0f);
    int j;

    for (j = 0; j < required_input; ++j) {
        /* opos is in the range [-(fir_width - 1), count) */
        int opos = (int)(opos_num >> FREQ_ADJUST_SHIFT) - FIR_WIDTH;
        UINT idx = ~(DWORD)opos_num >> (FREQ_ADJUST_SHIFT - FIR_STEP_SHIFT) << FIR_WIDTH_SHIFT;
        __m128 rem_inv = _mm_sub_ps(one, rem);

        __m128 input_value_ss = _mm_mul_ss(_mm_load_ss(&input[j]), firgain);
        __m128 input_value = _mm_shuffle_ps(input_value_ss, input_value_ss, 0);
        __m128 input_value0 = _mm_mul_ps(rem_inv, input_value);
        __m128 input_value1 = _mm_mul_ps(rem, input_value);

        int i;
        C_ASSERT(!(FIR_WIDTH % 4));
        for (i = 0; i < FIR_WIDTH; i += 4) {
            __m128 value0 = _mm_mul_ps(_mm_load_ps(&fir[idx + i]), input_value0);
            __m128 value1 = _mm_mul_ps(_mm_load_ps(&fir[idx + FIR_WIDTH + i]), input_value1);
            __m128 value = _mm_add_ps(value0, value1);
            _mm_storeu_ps(&output[opos + i], _mm_add_ps(_mm_loadu_ps(&output[opos + i]), value));
        }

        rem = _mm_add_ps(rem, rem_step);
        rem = _mm_sub_ps(rem, _mm_and_ps(one, _mm_cmple_ps(one, rem)));

        opos_num += opos_num_step;
    }
}

#endif

/**
 * Note that this function will overwrite up to FIR_WIDTH - 1 frames before and
 * after output[].
 */
static void downsample(DWORD freq_adjust_den, DWORD freq_acc_start, float firgain,
        UINT required_input, float *input, float *output)
{
    /* Both opos_num and rem are calculated in an incremental fashion,
     * independently of each other. This improves performance a bit, presumably
     * because it allows the CPU to do the calculation in parallel.
     *
     * However, the value of rem must still be kept in perfect sync with the
     * lower part of opos_num. Otherwise, even a small divergence can cause them
     * to wrap around on different iterations of the outer loop, which will
     * produce artifacts.
     *
     * To prevent this, clear the lower bits of opos_num and opos_num_step so
     * that rem can always represent the calculated value exactly. As rem is
     * always less than 2, its exponent is less than or equal to zero. This
     * means that in the worst case, rem has the same number of fractional bits
     * as the significand, which is 23 for a single-precision floating point.
     *
     * Clearing the bits is safe as it has the same effect as rounding up the
     * resampling ratio and the subsample position and doesn't affect the
     * initial opos value. */
    LONG64 opos_num_mask = ~0ull << (FREQ_ADJUST_SHIFT - 23 - FIR_STEP_SHIFT);
    LONG64 opos_num = (freq_adjust_den - freq_acc_start + (1ll << FREQ_ADJUST_SHIFT) - 1) & opos_num_mask;
    DWORD opos_num_step = freq_adjust_den & (DWORD)opos_num_mask;

    /* Use XOR to invert the lower part of opos_num so that the lower bits
     * remain cleared. */
    float rem = FIXED_0_32_TO_FLOAT(((DWORD)opos_num ^ (DWORD)opos_num_mask) << FIR_STEP_SHIFT);
    float rem_step = FIXED_0_32_TO_FLOAT(-opos_num_step << FIR_STEP_SHIFT);

#ifdef __SSE__
    downsample_sse(opos_num, opos_num_step, rem, rem_step, firgain, required_input, input, output);
#else
    int j;
    for (j = 0; j < required_input; ++j) {
        /* opos is in the range [-(fir_width - 1), count) */
        int opos = (int)(opos_num >> FREQ_ADJUST_SHIFT) - FIR_WIDTH;
        UINT idx = ~(DWORD)opos_num >> (FREQ_ADJUST_SHIFT - FIR_STEP_SHIFT) << FIR_WIDTH_SHIFT;

        float input_value = input[j] * firgain;
        float input_value0 = (1.0f - rem) * input_value;
        float input_value1 = rem * input_value;

        int i;
        for (i = 0; i < FIR_WIDTH; ++i)
            output[opos + i] += fir[idx + i] * input_value0 + fir[idx + FIR_WIDTH + i] * input_value1;

        rem += rem_step;
        rem -= rem >= 1.0f ? 1.0f : 0.0f;

        opos_num += opos_num_step;
    }
#endif
}

#ifdef __SSE__

void upsample_sse(LONG64 ipos_num, DWORD ipos_num_step, float rem_inv_float,
        float rem_inv_step_float, UINT count, float *input, float *output)
{
    __m128 rem_inv = _mm_set1_ps(rem_inv_float);
    __m128 rem_inv_step = _mm_set1_ps(rem_inv_step_float);
    __m128 one = _mm_set1_ps(1.0f);

    UINT i;

    for(i = 0; i < count; ++i) {
        UINT ipos = ipos_num >> FREQ_ADJUST_SHIFT;
        UINT idx = ~(DWORD)ipos_num >> (FREQ_ADJUST_SHIFT - FIR_STEP_SHIFT) << FIR_WIDTH_SHIFT;
        __m128 rem = _mm_sub_ps(one, rem_inv);

        int j;
        __m128 sum = _mm_set1_ps(0.0f);
        float* cache = &input[ipos];

        C_ASSERT(!(FIR_WIDTH % 4));
        for (j = 0; j < FIR_WIDTH; j += 4) {
            __m128 fir_value0 = _mm_mul_ps(_mm_load_ps(&fir[idx + j]), rem_inv);
            __m128 fir_value1 = _mm_mul_ps(_mm_load_ps(&fir[idx + j + FIR_WIDTH]), rem);
            __m128 fir_value = _mm_add_ps(fir_value0, fir_value1);
            __m128 input_value = _mm_loadu_ps(&cache[j]);
            sum = _mm_add_ps(sum, _mm_mul_ps(fir_value, input_value));
        }

        /* Add the even-numbered sums to the odd-numbered ones. */
        sum = _mm_add_ps(sum, _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(0, 3, 0, 1)));
        /* Calculate the final sum and store it to the output array. */
        sum = _mm_add_ss(sum, _mm_movehl_ps(sum, sum));
        _mm_store_ss(&output[i], sum);

        rem_inv = _mm_add_ps(rem_inv, rem_inv_step);
        rem_inv = _mm_sub_ps(rem_inv, _mm_and_ps(one, _mm_cmple_ps(one, rem_inv)));

        ipos_num += ipos_num_step;
    }
}

#endif

static void upsample(DWORD freq_adjust_num, DWORD freq_acc_start, UINT count, float *input,
        float *output)
{
    /* Both ipos_num and rem_inv are calculated in an incremental fashion,
     * independently of each other. This improves performance a bit, presumably
     * because it allows the CPU to do the calculation in parallel.
     *
     * However, the value of rem_inv must still be kept in perfect sync with the
     * lower part of ipos_num. Otherwise, even a small divergence can cause them
     * to wrap around on different iterations of the outer loop, which will
     * produce artifacts.
     *
     * To prevent this, clear the lower bits of ipos_num and ipos_num_step so
     * that rem_inv can always represent the calculated value exactly. As
     * rem_inv is always less than 2, its exponent is less than or equal to
     * zero. This means that in the worst case, rem_inv has the same number of
     * fractional bits as the significand, which is 23 for a single-precision
     * floating point.
     *
     * Clearing the bits is safe as it has the same effect as rounding down the
     * resampling ratio and the subsample position. */
    DWORD ipos_num_mask = ~0u << (FREQ_ADJUST_SHIFT - 23 - FIR_STEP_SHIFT);
    LONG64 ipos_num = freq_acc_start & ipos_num_mask;
    DWORD ipos_num_step = freq_adjust_num & ipos_num_mask;

    float rem_inv = FIXED_0_32_TO_FLOAT((DWORD)ipos_num << FIR_STEP_SHIFT);
    float rem_inv_step = FIXED_0_32_TO_FLOAT(ipos_num_step << FIR_STEP_SHIFT);

#ifdef __SSE__
    upsample_sse(ipos_num, ipos_num_step, rem_inv, rem_inv_step, count, input, output);
#else
    UINT i;
    for(i = 0; i < count; ++i) {
        UINT ipos = ipos_num >> FREQ_ADJUST_SHIFT;
        UINT idx = ~(DWORD)ipos_num >> (FREQ_ADJUST_SHIFT - FIR_STEP_SHIFT) << FIR_WIDTH_SHIFT;
        float rem = 1.0f - rem_inv;

        int j;
        float sum = 0.0;
        float* cache = &input[ipos];

        for (j = 0; j < FIR_WIDTH; j++)
            sum += (fir[idx + j] * rem_inv + fir[idx + j + FIR_WIDTH] * rem) * cache[j];
        output[i] = sum;

        rem_inv += rem_inv_step;
        rem_inv -= rem_inv >= 1.0f ? 1.0f : 0.0f;

        ipos_num += ipos_num_step;
    }
#endif
}

/**
 * Note that this function will overwrite up to FIR_WIDTH - 1 frames before and
 * after output[].
 */
static void resample(DWORD freq_adjust_num, DWORD freq_adjust_den, DWORD freq_acc_start,
        float firgain, UINT required_input, UINT count, float *input, float *output)
{
    if (freq_adjust_num > freq_adjust_den) {
        /* Take a reciprocal of the resampling ratio and convert it to a 0.32
         * fixed point. Round down to prevent output buffer overflow. */
        DWORD freq_adjust_fixed_den = ((LONG64)freq_adjust_den << FREQ_ADJUST_SHIFT)
                / freq_adjust_num;
        /* Convert the subsample position to a 0.32 fixed point. Round up to
         * prevent output buffer overflow. */
        DWORD freq_acc_fixed_start = ((LONG64)freq_acc_start * freq_adjust_fixed_den
                + freq_adjust_den - 1) / freq_adjust_den;

        memset(output, 0, count * sizeof(float));
        downsample(freq_adjust_fixed_den, freq_acc_fixed_start, firgain, required_input, input,
                output);
    } else {
        /* Convert the resampling ratio to a 0.32 fixed point. Round down to
         * prevent input buffer overflow. */
        DWORD freq_adjust_fixed_num = ((LONG64)freq_adjust_num << FREQ_ADJUST_SHIFT)
                / freq_adjust_den;
        /* Convert the subsample position to a 0.32 fixed point. Round down to
         * prevent input buffer overflow. */
        DWORD freq_acc_fixed_start = ((LONG64)freq_acc_start << FREQ_ADJUST_SHIFT)
                / freq_adjust_den;

        upsample(freq_adjust_fixed_num, freq_acc_fixed_start, count, input, output);
    }
}

static UINT cp_fields_resample(IDirectSoundBufferImpl *dsb, UINT count, DWORD *freqAccNum)
{
    UINT i, channel;
    UINT istride = dsb->pwfx->nBlockAlign;
    UINT ostride = dsb->device->pwfx->nChannels * sizeof(float);
    UINT committed_samples = 0;

    LONG64 freqAcc_start = *freqAccNum;
    LONG64 freqAcc_end = freqAcc_start + count * dsb->freqAdjustNum;
    UINT channels = dsb->mix_channels;
    UINT max_ipos = (freqAcc_start + count * dsb->freqAdjustNum) / dsb->freqAdjustDen;

    UINT required_input = max(
            (freqAcc_start + (count - 1) * dsb->freqAdjustNum) / dsb->freqAdjustDen + FIR_WIDTH,
            (freqAcc_start + (count - 1 + FIR_WIDTH) * dsb->freqAdjustNum) / dsb->freqAdjustDen);
    float *intermediate, *output, *itmp;

    DWORD len = required_input * channels;
    /* Allocate an output buffer for each channel with padding on both ends as
     * required by the resample function. Padding at the end of one channel
     * buffer is reused as a start padding for the next channel buffer. */
    len += FIR_WIDTH - 1 + (count + FIR_WIDTH - 1) * channels;
    len *= sizeof(float);

    *freqAccNum = freqAcc_end % dsb->freqAdjustDen;

    if (!secondarybuffer_is_audible(dsb))
        return max_ipos;

    if (!dsb->device->cp_buffer) {
        dsb->device->cp_buffer = malloc(len);
        dsb->device->cp_buffer_len = len;
    } else if (len > dsb->device->cp_buffer_len) {
        dsb->device->cp_buffer = realloc(dsb->device->cp_buffer, len);
        dsb->device->cp_buffer_len = len;
    }

    intermediate = dsb->device->cp_buffer;
    output = intermediate + required_input * channels + FIR_WIDTH - 1;

    if(dsb->use_committed) {
        committed_samples = (dsb->writelead - dsb->committed_mixpos) / istride;
        committed_samples = committed_samples <= required_input ? committed_samples : required_input;
    }

    /* Important: this buffer MUST be non-interleaved
     * if you want -msse3 to have any effect.
     * This is good for CPU cache effects, too.
     */
    itmp = intermediate;
    for (channel = 0; channel < channels; channel++) {
        for (i = 0; i < committed_samples; i++)
            *(itmp++) = get_current_sample(dsb, dsb->committedbuff,
                dsb->writelead, dsb->committed_mixpos + i * istride, channel);
        for (; i < required_input; i++)
            *(itmp++) = get_current_sample(dsb, dsb->buffer->memory,
                    dsb->buflen, dsb->sec_mixpos + i * istride, channel);
    }

    for (channel = 0; channel < channels; channel++)
        resample(dsb->freqAdjustNum, dsb->freqAdjustDen, freqAcc_start, dsb->firgain,
                required_input, count, intermediate + channel * required_input,
                output + channel * (FIR_WIDTH - 1 + count));

    for(i = 0; i < count; ++i)
        for (channel = 0; channel < channels; channel++)
            dsb->put(dsb, i * ostride, channel, output[channel * (FIR_WIDTH - 1 + count) + i]);

    return max_ipos;
}

static UINT cp_fields_noresample(IDirectSoundBufferImpl *dsb, UINT count)
{
    UINT istride = dsb->pwfx->nBlockAlign;
    UINT ostride = dsb->device->pwfx->nChannels * sizeof(float);
    UINT committed_samples = 0;
    DWORD channel, i;

    if (!secondarybuffer_is_audible(dsb))
        return count;

    if(dsb->use_committed) {
        committed_samples = (dsb->writelead - dsb->committed_mixpos) / istride;
        committed_samples = committed_samples <= count ? committed_samples : count;
    }

    for (i = 0; i < committed_samples; i++)
        for (channel = 0; channel < dsb->mix_channels; channel++)
            dsb->put(dsb, i * ostride, channel, get_current_sample(dsb, dsb->committedbuff,
                dsb->writelead, dsb->committed_mixpos + i * istride, channel));

    for (; i < count; i++)
        for (channel = 0; channel < dsb->mix_channels; channel++)
            dsb->put(dsb, i * ostride, channel, get_current_sample(dsb, dsb->buffer->memory,
                dsb->buflen, dsb->sec_mixpos + i * istride, channel));

    return count;
}

static void cp_fields(IDirectSoundBufferImpl *dsb, UINT count, DWORD *freqAccNum)
{
    DWORD ipos, adv;

    if (dsb->freqAdjustNum == dsb->freqAdjustDen)
        adv = cp_fields_noresample(dsb, count); /* *freqAccNum is unmodified */
    else
        adv = cp_fields_resample(dsb, count, freqAccNum);

    ipos = dsb->sec_mixpos + adv * dsb->pwfx->nBlockAlign;
    if (ipos >= dsb->buflen) {
        if (dsb->playflags & DSBPLAY_LOOPING)
            ipos %= dsb->buflen;
        else {
            ipos = 0;
            dsb->state = STATE_STOPPED;
        }
    }

    dsb->sec_mixpos = ipos;

    if(dsb->use_committed) {
        dsb->committed_mixpos += adv * dsb->pwfx->nBlockAlign;
        if(dsb->committed_mixpos >= dsb->writelead)
            dsb->use_committed = FALSE;
    }
}

/**
 * Calculate the distance between two buffer offsets, taking wraparound
 * into account.
 */
static inline DWORD DSOUND_BufPtrDiff(DWORD buflen, DWORD ptr1, DWORD ptr2)
{
/* If these asserts fail, the problem is not here, but in the underlying code */
	assert(ptr1 < buflen);
	assert(ptr2 < buflen);
	if (ptr1 >= ptr2) {
		return ptr1 - ptr2;
	} else {
		return buflen + ptr1 - ptr2;
	}
}
/**
 * Mix at most the given amount of data into the allocated temporary buffer
 * of the given secondary buffer, starting from the dsb's first currently
 * unsampled frame (writepos), translating frequency (pitch), stereo/mono
 * and bits-per-sample so that it is ideal for the primary buffer.
 * Doesn't perform any mixing - this is a straight copy/convert operation.
 *
 * dsb = the secondary buffer
 * writepos = Starting position of changed buffer
 * len = number of bytes to resample from writepos
 *
 * NOTE: writepos + len <= buflen. When called by mixer, MixOne makes sure of this.
 */
static void DSOUND_MixToTemporary(IDirectSoundBufferImpl *dsb, DWORD frames)
{
	UINT size_bytes = frames * sizeof(float) * dsb->device->pwfx->nChannels;
	HRESULT hr;
	int i;

	if (dsb->device->tmp_buffer_len < size_bytes || !dsb->device->tmp_buffer)
	{
		dsb->device->tmp_buffer_len = size_bytes;
		dsb->device->tmp_buffer = realloc(dsb->device->tmp_buffer, size_bytes);
	}
	if(dsb->put_aux == putieee32_sum)
		memset(dsb->device->tmp_buffer, 0, dsb->device->tmp_buffer_len);

	cp_fields(dsb, frames, &dsb->freqAccNum);

	if (size_bytes > 0) {
		for (i = 0; i < dsb->num_filters; i++) {
			if (dsb->filters[i].inplace) {
				hr = IMediaObjectInPlace_Process(dsb->filters[i].inplace, size_bytes, (BYTE*)dsb->device->tmp_buffer, 0, DMO_INPLACE_NORMAL);

				if (FAILED(hr))
					WARN("IMediaObjectInPlace_Process failed for filter %u\n", i);
			} else
				WARN("filter %u has no inplace object - unsupported\n", i);
		}
	}
}

static void DSOUND_MixerVol(const IDirectSoundBufferImpl *dsb, INT frames)
{
	INT	i;
	float vols[DS_MAX_CHANNELS];
	UINT channels = dsb->device->pwfx->nChannels, chan;

	TRACE("(%p,%d)\n",dsb,frames);
	TRACE("left = %lx, right = %lx\n", dsb->volpan.dwTotalAmpFactor[0],
		dsb->volpan.dwTotalAmpFactor[1]);

	if ((!(dsb->dsbd.dwFlags & DSBCAPS_CTRLPAN) || (dsb->volpan.lPan == 0)) &&
	    (!(dsb->dsbd.dwFlags & DSBCAPS_CTRLVOLUME) || (dsb->volpan.lVolume == 0)) &&
	     !(dsb->dsbd.dwFlags & DSBCAPS_CTRL3D))
		return; /* Nothing to do */

	if (channels > DS_MAX_CHANNELS)
	{
		FIXME("There is no support for %u channels\n", channels);
		return;
	}

	for (i = 0; i < channels; ++i)
		vols[i] = dsb->volpan.dwTotalAmpFactor[i] / ((float)0xFFFF);

	for(i = 0; i < frames; ++i){
		for(chan = 0; chan < channels; ++chan){
			dsb->device->tmp_buffer[i * channels + chan] *= vols[chan];
		}
	}
}

/**
 * Mix (at most) the given number of bytes into the given position of the
 * device buffer, from the secondary buffer "dsb" (starting at the current
 * mix position for that buffer).
 *
 * Returns the number of bytes actually mixed into the device buffer. This
 * will match fraglen unless the end of the secondary buffer is reached
 * (and it is not looping).
 *
 * dsb  = the secondary buffer to mix from
 * fraglen = number of bytes to mix
 */
static DWORD DSOUND_MixInBuffer(IDirectSoundBufferImpl *dsb, float *mix_buffer, DWORD frames)
{
	float *ibuf;
	DWORD oldpos;

	TRACE("sec_mixpos=%ld/%ld\n", dsb->sec_mixpos, dsb->buflen);
	TRACE("(%p, frames=%ld)\n",dsb,frames);

	/* Resample buffer to temporary buffer specifically allocated for this purpose, if needed */
	oldpos = dsb->sec_mixpos;
	DSOUND_MixToTemporary(dsb, frames);
	ibuf = dsb->device->tmp_buffer;

	if (secondarybuffer_is_audible(dsb)) {
		/* Apply volume if needed */
		DSOUND_MixerVol(dsb, frames);

		mixieee32(ibuf, mix_buffer, frames * dsb->device->pwfx->nChannels);
	}

	/* check for notification positions */
	if (dsb->dsbd.dwFlags & DSBCAPS_CTRLPOSITIONNOTIFY) {
		INT ilen = DSOUND_BufPtrDiff(dsb->buflen, dsb->sec_mixpos, oldpos);
		DSOUND_CheckEvent(dsb, oldpos, ilen);
	}

	return frames;
}

/**
 * Mix some frames from the given secondary buffer "dsb" into the device
 * primary buffer.
 *
 * dsb = the secondary buffer
 * playpos = the current play position in the device buffer (primary buffer)
 * frames = the maximum number of frames in the primary buffer to mix, from the
 *          current writepos.
 *
 * Returns: the number of frames beyond the writepos that were mixed.
 */
static DWORD DSOUND_MixOne(IDirectSoundBufferImpl *dsb, float *mix_buffer, DWORD frames)
{
	DWORD primary_done = 0;

	TRACE("(%p, frames=%ld)\n",dsb,frames);
	TRACE("looping=%ld\n", dsb->playflags);

	/* First try to mix to the end of the buffer if possible
	 * Theoretically it would allow for better optimization
	*/
	primary_done += DSOUND_MixInBuffer(dsb, mix_buffer, frames);

	TRACE("total mixed data=%ld\n", primary_done);

	/* Report back the total prebuffered amount for this buffer */
	return primary_done;
}

static void DSOUND_MixToPrimary(const DirectSoundDevice *device, float *mix_buffer, DWORD frames)
{
	INT i;
	IDirectSoundBufferImpl	*dsb;

	TRACE("(frames %ld)\n", frames);
	for (i = 0; i < device->nrofbuffers; i++) {
		dsb = device->buffers[i];

		TRACE("MixToPrimary for %p, state=%ld\n", dsb, dsb->state);

		if (dsb->buflen && dsb->state) {
			TRACE("Checking %p, frames=%ld\n", dsb, frames);
			AcquireSRWLockShared(&dsb->lock);
			if (dsb->state != STATE_STOPPED) {

				/* if the buffer was starting, it must be playing now */
				if (dsb->state == STATE_STARTING)
					dsb->state = STATE_PLAYING;

				/* mix next buffer into the main buffer */
				DSOUND_MixOne(dsb, mix_buffer, frames);
			}
			ReleaseSRWLockShared(&dsb->lock);
		}
	}
}

/**
 * Add buffers to the emulated wave device system.
 *
 * device = The current dsound playback device
 * force = If TRUE, the function will buffer up as many frags as possible,
 *         even though and will ignore the actual state of the primary buffer.
 *
 * Returns:  None
 */

static void DSOUND_WaveQueue(DirectSoundDevice *device, LPBYTE pos, DWORD bytes)
{
	BYTE *buffer;
	HRESULT hr;

	TRACE("(%p)\n", device);

	hr = IAudioRenderClient_GetBuffer(device->render, bytes / device->pwfx->nBlockAlign, &buffer);
	if(FAILED(hr)){
		WARN("GetBuffer failed: %08lx\n", hr);
		return;
	}

	memcpy(buffer, pos, bytes);

	hr = IAudioRenderClient_ReleaseBuffer(device->render, bytes / device->pwfx->nBlockAlign, 0);
	if(FAILED(hr)) {
		ERR("ReleaseBuffer failed: %08lx\n", hr);
		IAudioRenderClient_ReleaseBuffer(device->render, 0, 0);
		return;
	}

	device->pad += bytes;
}

/**
 * Perform mixing for a Direct Sound device. That is, go through all the
 * secondary buffers (the sound bites currently playing) and mix them in
 * to the primary buffer (the device buffer).
 *
 * The mixing procedure goes:
 *
 * secondary->buffer (secondary format)
 *   =[Resample]=> device->tmp_buffer (float format)
 *   =[Volume]=> device->tmp_buffer (float format)
 *   =[Reformat]=> device->buffer (device format, skipped on float)
 */
static void DSOUND_PerformMix(DirectSoundDevice *device)
{
	DWORD block, pad_bytes, frames;
	UINT32 pad_frames;
	HRESULT hr;

	TRACE("(%p)\n", device);

	/* **** */
	EnterCriticalSection(&device->mixlock);

	hr = IAudioClient_GetCurrentPadding(device->client, &pad_frames);
	if(FAILED(hr)){
		WARN("GetCurrentPadding failed: %08lx\n", hr);
		LeaveCriticalSection(&device->mixlock);
		return;
	}
	block = device->pwfx->nBlockAlign;
	pad_bytes = pad_frames * block;
	device->playpos += device->pad - pad_bytes;
	device->playpos %= device->buflen;
	device->pad = pad_bytes;

	frames = device->ac_frames - pad_frames;
	if(!frames){
		/* nothing to do! */
		LeaveCriticalSection(&device->mixlock);
		return;
	}
	if (frames > device->frag_frames * 3)
		frames = device->frag_frames * 3;

	if (device->priolevel != DSSCL_WRITEPRIMARY) {
		int nfiller;
		void *buffer = NULL;
		LARGE_INTEGER mix_t0, mix_t1, qpf;
		float *d0_src;
		double mix_us;

		/* the sound of silence */
		nfiller = device->pwfx->wBitsPerSample == 8 ? 128 : 0;

		/* check for underrun. underrun occurs when the write position passes the mix position
		 * also wipe out just-played sound data */
		if (!pad_frames)
			WARN("Probable buffer underrun\n");

		hr = IAudioRenderClient_GetBuffer(device->render, frames, (BYTE **)&buffer);
		if(FAILED(hr)){
			WARN("GetBuffer failed: %08lx\n", hr);
			LeaveCriticalSection(&device->mixlock);
			return;
		}

		memset(buffer, nfiller, frames * block);

		d0_src = NULL;
		mix_us = 0;
		QueryPerformanceFrequency(&qpf);
		QueryPerformanceCounter(&mix_t0);
		if (!device->normfunction) {
			DSOUND_MixToPrimary(device, buffer, frames);
			d0_src = buffer;
		} else {
			memset(device->buffer, nfiller, device->buflen);

			/* do the mixing */
			DSOUND_MixToPrimary(device, (float*)device->buffer, frames);
			d0_src = (float*)device->buffer;
		}
		QueryPerformanceCounter(&mix_t1);
		if (qpf.QuadPart)
			mix_us = (double)(mix_t1.QuadPart - mix_t0.QuadPart) * 1e6 / (double)qpf.QuadPart;
		DSOUND_D0_AfterMix(device, d0_src, frames, mix_us);
		if (device->normfunction)
			device->normfunction(device->buffer, buffer, frames * device->pwfx->nChannels);

		hr = IAudioRenderClient_ReleaseBuffer(device->render, frames, 0);
		if(FAILED(hr))
			ERR("ReleaseBuffer failed: %08lx\n", hr);

		device->pad += frames * block;
	} else if (!device->stopped) {
                DWORD writepos = (device->playpos + pad_bytes) % device->buflen;
                DWORD bytes = frames * block;

		if (bytes > device->buflen)
			bytes = device->buflen;
		if (writepos + bytes > device->buflen) {
			DSOUND_WaveQueue(device, device->buffer + writepos, device->buflen - writepos);
			DSOUND_WaveQueue(device, device->buffer, writepos + bytes - device->buflen);
		} else
			DSOUND_WaveQueue(device, device->buffer + writepos, bytes);
	}

	LeaveCriticalSection(&(device->mixlock));
	/* **** */
}

DWORD CALLBACK DSOUND_mixthread(void *p)
{
	DirectSoundDevice *dev = p;

	TRACE("(%p)\n", dev);
	SetThreadDescription(GetCurrentThread(), L"wine_dsound_mixer");
        _controlfp_s(NULL, _DN_FLUSH, _MCW_DN);

        for (;;)
        {
            DWORD ret = WaitForSingleObject(dev->sleepev, dev->sleeptime);
            if (ret == WAIT_FAILED)
                WARN("wait returned error %lu %08lx!\n", GetLastError(), GetLastError());
            else if (ret != WAIT_OBJECT_0)
                WARN("wait returned %08lx!\n", ret);
            if (dev->terminated)
                break;

            AcquireSRWLockShared(&dev->buffer_list_lock);
            DSOUND_PerformMix(dev);
            ReleaseSRWLockShared(&dev->buffer_list_lock);
        }
	return 0;
}
