/*
 * WineHua Process Broker client
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

#ifndef __WINE_OHOS_BROKER_H
#define __WINE_OHOS_BROKER_H

#ifdef __OHOS__

/* Send a SPAWN request to the Process Broker via Unix socket.
 *
 * The broker protocol:
 *   "SPAWN\n{entry_params}\n[FDS:name0,name1,...]\n" + cmsg{fds}
 * Response: int32_t[2] = {child_pid, status} (status 0 = success)
 *
 * entry_params: "binDir|arg0|arg1|..." may contain |__env=K=V segments
 * fd_names: named file descriptors to pass (NULL if n_fds == 0)
 * fds: file descriptors to send via SCM_RIGHTS (NULL if n_fds == 0)
 * n_fds: number of fds (clamped to 16 max)
 * child_pid: output, receives child process PID
 *
 * Returns 0 on success, -1 on failure.
 */
int ohos_broker_spawn(const char *entry_params,
                      const char **fd_names, const int *fds, int n_fds,
                      int *child_pid);

/* Convenience: spawn wineserver via broker.
 * Resolves binDir from WINEBINDIR env, sends "binDir|wineserver|-f|-p".
 * Returns 0 on success (sets *child_pid), -1 on failure. */
int ohos_broker_spawn_wineserver( int *child_pid );

/* Convenience: spawn a Wine child process via broker.
 * Builds entryParams from argv + environ (blacklist-filtered),
 * and passes socketfd as "wineserver_sock" via SCM_RIGHTS.
 * Returns 0 on success (sets *child_pid), -1 on failure. */
int ohos_broker_spawn_child( char **argv, int socketfd, int *child_pid );

#endif /* __OHOS__ */

#endif /* __WINE_OHOS_BROKER_H */
