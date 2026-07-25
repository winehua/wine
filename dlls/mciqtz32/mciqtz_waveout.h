/*
 * WaveOut backend for MCI QTZ driver (OHOS)
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

#ifndef __WINE_PRIVATE_MCIQTZ_WAVEOUT_H
#define __WINE_PRIVATE_MCIQTZ_WAVEOUT_H


#include "mciqtz_private.h"

/* WaveOut backend entry points — called from mciqtz.c dispatch points */

DWORD MCIQTZ_waveout_open(WINE_MCIQTZ *wma, DWORD flags,
                          const MCI_DGV_OPEN_PARMSW *params);

DWORD MCIQTZ_waveout_play(WINE_MCIQTZ *wma, DWORD flags,
                          LPMCI_PLAY_PARMS lpParms);

DWORD MCIQTZ_waveout_seek(WINE_MCIQTZ *wma, DWORD flags,
                          LPMCI_SEEK_PARMS lpParms);

void MCIQTZ_waveout_pos_sync(WINE_MCIQTZ *wma);

void MCIQTZ_waveout_state_reset(WINE_MCIQTZ *wma);

void MCIQTZ_waveout_close(WINE_MCIQTZ *wma);

DWORD MCIQTZ_waveout_pause(WINE_MCIQTZ *wma);

DWORD MCIQTZ_waveout_resume(WINE_MCIQTZ *wma);

DWORD MCIQTZ_waveout_length(const WINE_MCIQTZ *wma);

DWORD MCIQTZ_waveout_position(const WINE_MCIQTZ *wma);

DWORD MCIQTZ_waveout_mode(const WINE_MCIQTZ *wma);

DWORD MCIQTZ_waveout_set_volume(WINE_MCIQTZ *wma, DWORD value);

BOOL MCIQTZ_waveout_is_candidate(LPCWSTR path);


#endif /* __WINE_PRIVATE_MCIQTZ_WAVEOUT_H */
