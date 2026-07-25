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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
