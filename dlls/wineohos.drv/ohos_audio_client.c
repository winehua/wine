#pragma makedep unix

#include "ohos_audio_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_close_on_exec(int fd, int enabled)
{
    int flags = fcntl(fd, F_GETFD);

    if (flags < 0) return -errno;
    if (enabled) flags |= FD_CLOEXEC;
    else flags &= ~FD_CLOEXEC;
    if (fcntl(fd, F_SETFD, flags) != 0) return -errno;
    return 0;
}

static ssize_t send_packet(int fd, const void *data, size_t bytes, int send_fd)
{
    struct msghdr msg;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(int))];

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = (void *)data;
    iov.iov_len = bytes;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (send_fd >= 0)
    {
        struct cmsghdr *cmsg;

        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(send_fd));
        msg.msg_controllen = cmsg->cmsg_len;
    }

    while (1)
    {
        ssize_t sent = sendmsg(fd, &msg, 0);
        if (sent >= 0) return sent;
        if (errno != EINTR) return -1;
    }
}

static ssize_t recv_packet(int fd, void *data, size_t bytes, int *recv_fd)
{
    struct msghdr msg;
    struct iovec iov;
    char control[CMSG_SPACE(sizeof(int))];
    ssize_t got;

    if (recv_fd) *recv_fd = -1;
    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = data;
    iov.iov_len = bytes;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    while (1)
    {
        got = recvmsg(fd, &msg, 0);
        if (got >= 0) break;
        if (errno != EINTR) return -1;
    }

    if (got <= 0) return got;
    if (msg.msg_flags & MSG_TRUNC) return -1;
    if (recv_fd)
    {
        struct cmsghdr *cmsg;

        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(int)))
            {
                memcpy(recv_fd, CMSG_DATA(cmsg), sizeof(int));
                break;
            }
        }
    }
    return got;
}

static int parse_env_fd(const char *name, int *out_fd)
{
    const char *value = getenv(name);
    char *end = NULL;
    long parsed;

    if (!value || !value[0] || !out_fd) return -ENOENT;
    parsed = strtol(value, &end, 10);
    if (!end || *end || parsed < 0 || parsed > INT32_MAX) return -EINVAL;
    *out_fd = (int)parsed;
    return 0;
}

static int client_call(OhosAudioClient *client, uint16_t cmd,
                       const void *payload, size_t payload_size,
                       void *out_payload, size_t out_payload_size,
                       int *out_fd)
{
    unsigned char reqbuf[WINEHUA_AUDIO_PACKET_MAX];
    unsigned char respbuf[WINEHUA_AUDIO_PACKET_MAX];
    WinehuaAudioIpcHeader *header = (WinehuaAudioIpcHeader *)reqbuf;
    WinehuaAudioIpcHeader *reply = (WinehuaAudioIpcHeader *)respbuf;
    ssize_t got;
    int recv_fd = -1;
    int ret = -EIO;

    if (!client || sizeof(*header) + payload_size > sizeof(reqbuf) ||
        sizeof(*reply) + out_payload_size > sizeof(respbuf))
        return -EINVAL;
    if (out_fd) *out_fd = -1;

    memset(reqbuf, 0, sizeof(*header));
    header->magic = WINEHUA_AUDIO_CONTROL_MAGIC;
    header->version = WINEHUA_AUDIO_PROTOCOL_VERSION;
    header->cmd = cmd;
    header->seq = ++client->next_seq;
    header->payload_size = payload_size;
    if (payload_size) memcpy(reqbuf + sizeof(*header), payload, payload_size);

    pthread_mutex_lock(&client->lock);

    if (send_packet(client->fd, reqbuf, sizeof(*header) + payload_size, -1) !=
        (ssize_t)(sizeof(*header) + payload_size))
        goto done;

    got = recv_packet(client->fd, respbuf, sizeof(respbuf), &recv_fd);
    if (got < (ssize_t)sizeof(*reply)) goto done;
    if (reply->magic != WINEHUA_AUDIO_CONTROL_MAGIC ||
        reply->version != WINEHUA_AUDIO_PROTOCOL_VERSION ||
        reply->cmd != cmd || reply->seq != header->seq ||
        reply->payload_size != out_payload_size ||
        (size_t)got != sizeof(*reply) + out_payload_size)
        goto done;

    if (out_payload_size) memcpy(out_payload, respbuf + sizeof(*reply), out_payload_size);
    if (out_fd)
    {
        *out_fd = recv_fd;
        recv_fd = -1;
    }
    ret = 0;

done:
    if (recv_fd >= 0) close(recv_fd);
    pthread_mutex_unlock(&client->lock);
    return ret;
}

int ohos_audio_client_connect(OhosAudioClient **out_client)
{
    OhosAudioClient *client = NULL;
    WinehuaAudioBootstrapReq bootstrap_req;
    WinehuaAudioHelloReq hello_req;
    WinehuaAudioHelloResp hello_resp;
    int bootstrap_fd = -1;
    int private_pair[2] = { -1, -1 };
    int ret;
    const char *protocol = getenv("WINE_OHOS_AUDIO_PROTOCOL_VERSION");

    if (!out_client) return -EINVAL;
    if (protocol && protocol[0] && atoi(protocol) != (int)WINEHUA_AUDIO_PROTOCOL_VERSION)
        return -EPROTO;

    ret = parse_env_fd("WINE_OHOS_AUDIO_BOOTSTRAP_FD", &bootstrap_fd);
    if (ret != 0) return ret;

    client = calloc(1, sizeof(*client));
    if (!client) return -ENOMEM;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, private_pair) != 0)
    {
        free(client);
        return -errno;
    }

    set_close_on_exec(private_pair[0], 1);
    set_close_on_exec(private_pair[1], 1);

    memset(&bootstrap_req, 0, sizeof(bootstrap_req));
    bootstrap_req.magic = WINEHUA_AUDIO_BOOTSTRAP_MAGIC;
    bootstrap_req.version = WINEHUA_AUDIO_PROTOCOL_VERSION;
    bootstrap_req.cmd = WINEHUA_AUDIO_BOOTSTRAP_OPEN_CONTROL;
    bootstrap_req.client_pid = (uint32_t)getpid();
    bootstrap_req.client_tid = bootstrap_req.client_pid;
    snprintf(bootstrap_req.process_name, sizeof(bootstrap_req.process_name),
             "wine-%u", bootstrap_req.client_pid);

    if (send_packet(bootstrap_fd, &bootstrap_req, sizeof(bootstrap_req), private_pair[1]) !=
        (ssize_t)sizeof(bootstrap_req))
    {
        close(private_pair[0]);
        close(private_pair[1]);
        free(client);
        return -EIO;
    }

    close(private_pair[1]);
    client->fd = private_pair[0];
    client->next_seq = 0;
    pthread_mutex_init(&client->lock, NULL);

    memset(&hello_req, 0, sizeof(hello_req));
    hello_req.client_pid = bootstrap_req.client_pid;
    hello_req.client_tid = bootstrap_req.client_tid;
    memcpy(hello_req.process_name, bootstrap_req.process_name, sizeof(hello_req.process_name));
    if (client_call(client, WINEHUA_AUDIO_CMD_HELLO, &hello_req, sizeof(hello_req),
                    &hello_resp, sizeof(hello_resp), NULL) != 0 ||
        hello_resp.result != 0)
    {
        int err = hello_resp.result ? hello_resp.result : -EIO;
        ohos_audio_client_disconnect(client);
        return err;
    }

    *out_client = client;
    return 0;
}

void ohos_audio_client_disconnect(OhosAudioClient *client)
{
    if (!client) return;
    if (client->fd >= 0) close(client->fd);
    pthread_mutex_destroy(&client->lock);
    free(client);
}

int ohos_audio_client_open_stream(OhosAudioClient *client,
                                  const WinehuaAudioOpenStreamReq *req,
                                  OhosAudioClientStream *out_stream)
{
    WinehuaAudioOpenStreamResp resp;
    int ring_fd = -1;

    if (!client || !req || !out_stream) return -EINVAL;
    memset(out_stream, 0, sizeof(*out_stream));
    out_stream->ring_fd = -1;

    if (client_call(client, WINEHUA_AUDIO_CMD_OPEN_STREAM, req, sizeof(*req),
                    &resp, sizeof(resp), &ring_fd) != 0)
        return -EIO;
    if (resp.result != 0)
    {
        if (ring_fd >= 0) close(ring_fd);
        return resp.result;
    }
    if (ring_fd < 0) return -EIO;

    out_stream->stream_id = resp.stream_id;
    out_stream->preferred_period_frames = resp.preferred_period_frames;
    out_stream->ring_capacity_frames = resp.ring_capacity_frames;
    out_stream->mix_frame_size = resp.mix_frame_size;
    out_stream->ring_mapping_size = resp.ring_mapping_size;
    out_stream->ring_fd = ring_fd;
    out_stream->ring = mmap(NULL, out_stream->ring_mapping_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, out_stream->ring_fd, 0);
    if (out_stream->ring == MAP_FAILED)
    {
        int err = -errno;
        out_stream->ring = NULL;
        close(out_stream->ring_fd);
        out_stream->ring_fd = -1;
        return err;
    }
    if (out_stream->ring->magic != WINEHUA_AUDIO_RING_MAGIC ||
        out_stream->ring->version != WINEHUA_AUDIO_PROTOCOL_VERSION)
    {
        ohos_audio_client_close_stream(client, out_stream);
        return -EPROTO;
    }
    return 0;
}

int ohos_audio_client_start(OhosAudioClient *client, uint32_t stream_id)
{
    WinehuaAudioStreamCmdReq req = { stream_id };
    WinehuaAudioStreamCmdResp resp;

    if (!client) return -EINVAL;
    if (client_call(client, WINEHUA_AUDIO_CMD_START, &req, sizeof(req), &resp, sizeof(resp), NULL) != 0)
        return -EIO;
    return resp.result;
}

int ohos_audio_client_stop(OhosAudioClient *client, uint32_t stream_id)
{
    WinehuaAudioStreamCmdReq req = { stream_id };
    WinehuaAudioStreamCmdResp resp;

    if (!client) return -EINVAL;
    if (client_call(client, WINEHUA_AUDIO_CMD_STOP, &req, sizeof(req), &resp, sizeof(resp), NULL) != 0)
        return -EIO;
    return resp.result;
}

int ohos_audio_client_reset(OhosAudioClient *client, uint32_t stream_id)
{
    WinehuaAudioStreamCmdReq req = { stream_id };
    WinehuaAudioStreamCmdResp resp;

    if (!client) return -EINVAL;
    if (client_call(client, WINEHUA_AUDIO_CMD_RESET, &req, sizeof(req), &resp, sizeof(resp), NULL) != 0)
        return -EIO;
    return resp.result;
}

int ohos_audio_client_close_stream(OhosAudioClient *client, OhosAudioClientStream *stream)
{
    WinehuaAudioStreamCmdReq req;
    WinehuaAudioStreamCmdResp resp;
    int result = 0;

    if (!client || !stream) return -EINVAL;

    req.stream_id = stream->stream_id;
    if (client_call(client, WINEHUA_AUDIO_CMD_CLOSE, &req, sizeof(req), &resp, sizeof(resp), NULL) != 0)
        result = -EIO;
    else
        result = resp.result;

    if (stream->ring && stream->ring != MAP_FAILED)
    {
        munmap(stream->ring, stream->ring_mapping_size);
        stream->ring = NULL;
    }
    if (stream->ring_fd >= 0)
    {
        close(stream->ring_fd);
        stream->ring_fd = -1;
    }
    memset(stream, 0, sizeof(*stream));
    stream->ring_fd = -1;
    return result;
}

int ohos_audio_client_get_status(OhosAudioClient *client,
                                 uint32_t stream_id,
                                 WinehuaAudioGetStatusResp *out_status)
{
    WinehuaAudioGetStatusReq req = { stream_id };

    if (!client || !out_status) return -EINVAL;
    if (client_call(client, WINEHUA_AUDIO_CMD_GET_STATUS, &req, sizeof(req),
                    out_status, sizeof(*out_status), NULL) != 0)
        return -EIO;
    return out_status->result;
}

size_t ohos_audio_client_write_frames(OhosAudioClientStream *stream, const void *data, size_t frames)
{
    size_t written;

    if (!stream || !stream->ring) return 0;
    written = winehua_audio_ring_write_frames(stream->ring, data, frames);
    if (written < frames) winehua_audio_ring_increment_overflow(stream->ring);
    return written;
}

size_t ohos_audio_client_read_frames(OhosAudioClientStream *stream, void *data, size_t frames)
{
    size_t read;

    if (!stream || !stream->ring) return 0;
    read = winehua_audio_ring_read_frames(stream->ring, data, frames);
    if (read < frames) winehua_audio_ring_increment_underrun(stream->ring);
    return read;
}

size_t ohos_audio_client_get_free_frames(const OhosAudioClientStream *stream)
{
    if (!stream || !stream->ring) return 0;
    return winehua_audio_ring_free_frames(stream->ring);
}

size_t ohos_audio_client_get_queued_frames(const OhosAudioClientStream *stream)
{
    if (!stream || !stream->ring) return 0;
    return winehua_audio_ring_used_frames(stream->ring);
}
