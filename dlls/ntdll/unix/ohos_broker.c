/*
 * WineHua Process Broker client implementation
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(process);

/* Resolve the broker socket path. */
static const char *broker_socket_path(void)
{
    const char *path = getenv("PROCESSBROKER");
    return path ? path : "/data/storage/el2/base/files/.wine_broker";
}

/* Send a SPAWN request over an already-connected broker socket.
 * entry_params may be NULL (treated as empty).
 * Returns 0 on success (sets *child_pid), -1 on failure. */
static int broker_send_spawn(int broker_fd, const char *entry_params,
                             const char **fd_names, const int *fds, int n_fds,
                             int *child_pid)
{
    char req_hdr[] = "SPAWN\n";
    size_t ep_len = entry_params ? strlen(entry_params) : 0;
    char fds_line[512];
    int fl_len, i;
    struct iovec iov_parts[3];
    struct msghdr msg;
    union { char buf[CMSG_SPACE(sizeof(int) * 16)]; struct cmsghdr align; } ctrl;
    ssize_t received;
    int32_t response[2];

    if (n_fds > 16) n_fds = 16;  /* ctrl buffer limit, aligned with broker side */

    /* Build "\nFDS:name0,name1,...\n" (just "\n" when no fds) */
    if (n_fds > 0)
    {
        fl_len = snprintf(fds_line, sizeof(fds_line), "\nFDS:");
        for (i = 0; i < n_fds; i++)
            fl_len += snprintf(fds_line + fl_len, sizeof(fds_line) - fl_len,
                               "%s%s", i ? "," : "", fd_names[i]);
        fl_len += snprintf(fds_line + fl_len, sizeof(fds_line) - fl_len, "\n");
    }
    else fl_len = snprintf(fds_line, sizeof(fds_line), "\n");

    iov_parts[0].iov_base = req_hdr;
    iov_parts[0].iov_len  = sizeof(req_hdr) - 1;
    iov_parts[1].iov_base = (void *)(entry_params ? entry_params : "");
    iov_parts[1].iov_len  = ep_len;
    iov_parts[2].iov_base = fds_line;
    iov_parts[2].iov_len  = fl_len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov_parts;
    msg.msg_iovlen = 3;
    if (n_fds > 0)
    {
        struct cmsghdr *cmsg;
        msg.msg_control = ctrl.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * n_fds);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int) * n_fds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * n_fds);
        msg.msg_controllen = cmsg->cmsg_len;
    }

    if (sendmsg(broker_fd, &msg, MSG_NOSIGNAL) < 0) return -1;

    received = recv(broker_fd, response, sizeof(response), MSG_WAITALL);
    if (received != sizeof(response)) return -1;
    if (response[1] != 0 || response[0] <= 0) return -1;
    *child_pid = response[0];
    return 0;
}

/***********************************************************************
 *           ohos_broker_spawn
 *
 * Connect to the Process Broker and send a SPAWN request.
 *
 * The broker manages child process creation on OHOS (where fork/exec
 * are unavailable).  Callers construct entry_params with the target
 * binary and arguments, and optionally pass file descriptors to be
 * inherited by the child via SCM_RIGHTS.
 *
 * Protocol (over Unix socket):
 *   send:  "SPAWN\n{entry_params}\n[FDS:name0,...]\n" [+ cmsg{fds}]
 *   recv:  int32_t[2] = {child_pid, error_status}
 */
int ohos_broker_spawn(const char *entry_params,
                      const char **fd_names, const int *fds, int n_fds,
                      int *child_pid)
{
    const char *path = broker_socket_path();
    struct sockaddr_un addr;
    int broker_fd;
    int ret = -1;

    broker_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (broker_fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    if (connect(broker_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        ret = broker_send_spawn(broker_fd, entry_params, fd_names, fds, n_fds, child_pid);

    close(broker_fd);
    return ret;
}


/* Resolve binDir from WINEBINDIR env or fallback. */
static const char *resolve_bindir(void)
{
    const char *dir = getenv("WINEBINDIR");
    return dir ? dir : "/data/storage/el2/base/files/wine/bin";
}


/* Check if an env var should be forwarded to the child.
 * Blacklist: per-process fd/handle vars that the child gets via fdList. */
static int env_forwardable(const char *env)
{
    if (strchr(env, '|') || strchr(env, '\n')) return 0;
    if (!strncmp(env, "WINESERVERSOCKET=", 17) ||
        !strncmp(env, "WINE_OHOS_AUDIO_ENABLE=", 23) ||
        !strncmp(env, "WINE_OHOS_AUDIO_BOOTSTRAP_FD=", 29) ||
        !strncmp(env, "WINE_OHOS_AUDIO_PROTOCOL_VERSION=", 33))
        return 0;
    return 1;
}


/***********************************************************************
 *           ohos_broker_spawn_wineserver
 *
 * Convenience wrapper to spawn wineserver via the Process Broker.
 * Resolves binDir and sends entry_params "binDir|wineserver|-f|-p".
 */
int ohos_broker_spawn_wineserver( int *child_pid )
{
    const char *binDir = resolve_bindir();
    char entryParams[1024];
    snprintf(entryParams, sizeof(entryParams), "%s|wineserver|-f|-p", binDir);
    return ohos_broker_spawn(entryParams, NULL, NULL, 0, child_pid);
}


/***********************************************************************
 *           ohos_broker_spawn_child
 *
 * Spawn a Wine child process via the Process Broker.
 * Builds entryParams from argv array + filtered environ, and passes
 * socketfd as "wineserver_sock" via SCM_RIGHTS.
 */
int ohos_broker_spawn_child( char **argv, int socketfd, int *child_pid )
{
    const char *binDir = resolve_bindir();
    extern char **environ;
    char *entryParams = NULL;
    const char *fd_names[4];
    int fds[4];
    int n_send_fds = 0;
    int i, j, len;
    int ret;

    /* Compute entryParams buffer size */
    len = strlen(binDir) + 1;
    if (!getenv("USE_LIBBOX64"))
        len += 5; /* + "|wine" */
    for (i = 0; argv[i]; i++) len += strlen(argv[i]) + 1;
    if (environ) {
        for (j = 0; environ[j]; j++) {
            if (!env_forwardable(environ[j])) continue;
            len += strlen(environ[j]) + 7; /* "|__env=" prefix */
        }
    }

    entryParams = malloc(len + 1);
    if (!entryParams) return -1;

    /* Build entryParams: "binDir[|wine]|arg0|arg1|...|__env=K=V|..." */
    {
        char *p = entryParams;
        if (getenv("USE_LIBBOX64"))
            p += snprintf(p, len + 1, "%s", binDir);
        else
            p += snprintf(p, len + 1, "%s|wine", binDir);
        for (i = 0; argv[i]; i++)
            p += snprintf(p, len + 1 - (p - entryParams), "|%s", argv[i]);
        if (environ) {
            for (j = 0; environ[j]; j++) {
                if (!env_forwardable(environ[j])) continue;
                p += snprintf(p, len + 1 - (p - entryParams), "|__env=%s", environ[j]);
            }
        }
    }

    /* Collect fds */
    fd_names[n_send_fds] = "wineserver_sock";
    fds[n_send_fds] = socketfd;
    n_send_fds++;

    ret = ohos_broker_spawn(entryParams, fd_names, fds, n_send_fds, child_pid);
    free(entryParams);
    return ret;
}


/* Internal: single-shot scan for wineserver socket in sockdir. */
static int scan_socket_dir(const char *sockdir)
{
    struct stat st;
    DIR *d;
    struct dirent *de;
    int found = 0;

    if (stat(sockdir, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    d = opendir(sockdir);
    if (!d) return 0;
    while ((de = readdir(d)))
    {
        if (de->d_name[0] == '.') continue;
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s/socket", sockdir, de->d_name);
        if (stat(sub, &st) == 0 && S_ISSOCK(st.st_mode))
        {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}


/***********************************************************************
 *           ohos_broker_scan_wineserver
 *
 * Scan WINEPREFIX/.wineserver/<host>/socket.
 * - poll_sec == 0: one-shot check
 * - poll_sec > 0 && !vanish_mode: poll until socket appears (200ms steps)
 * - poll_sec > 0 && vanish_mode:  confirm socket does NOT vanish
 *
 * Returns 1 when condition is met, 0 otherwise.
 */
int ohos_broker_scan_wineserver(const char *prefix, int poll_sec, int vanish_mode)
{
    const char *p = prefix ? prefix : "/data/storage/el2/base/files/.wine";
    char sockdir[512];
    int i, found;
    int steps;

    snprintf(sockdir, sizeof(sockdir), "%s/.wineserver", p);

    if (poll_sec <= 0)
        return scan_socket_dir(sockdir);

    steps = poll_sec * 5; /* 200ms intervals */
    if (vanish_mode)
    {
        /* Socket must NOT vanish during poll window */
        for (i = 0; i < steps; i++)
        {
            usleep(200000);
            if (!scan_socket_dir(sockdir))
                return 0;
        }
        return 1;
    }
    else
    {
        /* Wait for socket to appear */
        for (i = 0; i < steps; i++)
        {
            if (scan_socket_dir(sockdir))
                return 1;
            usleep(200000);
        }
        return 0;
    }
}
