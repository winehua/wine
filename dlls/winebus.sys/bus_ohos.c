/*
 * WineHua OHOS Controller Hub bus — WHGP AF_UNIX → HID gamepad
 * (DirectInput + XInput compat) with haptics rumble back to the host.
 *
 * Inactive only when WINEHUA_GAMEPAD_MODE=keyboard_legacy or ENABLE=0.
 * Socket path: WINEHUA_GAMEPAD_SOCKET or $WINEPREFIX/whgp.sock.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ddk/hidtypes.h"
#include "hidusage.h"

#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unix_private.h"
#include "winehua_gamepad_protocol.h"

WINE_DEFAULT_DEBUG_CHANNEL(hid);

struct ohos_device
{
    struct unix_device unix_device;
    BOOL started;
    UINT slot;
};

static struct list event_queue = LIST_INIT(event_queue);
static struct list device_list = LIST_INIT(device_list);
static pthread_mutex_t ohos_cs = PTHREAD_MUTEX_INITIALIZER;
static int sock_fd = -1;
static BOOL quit_requested;
static BOOL logged_connect_fail;
static char socket_path_buf[sizeof(((struct sockaddr_un *)0)->sun_path)];

static const char *resolve_socket_path(void)
{
    const char *path, *prefix, *xdg;

    path = getenv("WINEHUA_GAMEPAD_SOCKET");
    if (path && path[0]) return path;

    prefix = getenv("WINEPREFIX");
    if (prefix && prefix[0])
    {
        snprintf(socket_path_buf, sizeof(socket_path_buf), "%s/whgp.sock", prefix);
        return socket_path_buf;
    }

    xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0])
    {
        snprintf(socket_path_buf, sizeof(socket_path_buf), "%s/whgp.sock", xdg);
        return socket_path_buf;
    }

    return "/data/storage/el2/base/files/.wine/whgp.sock";
}

static inline struct ohos_device *impl_from_unix_device(struct unix_device *iface)
{
    return CONTAINING_RECORD(iface, struct ohos_device, unix_device);
}

static void apply_neutral(struct unix_device *iface)
{
    struct hid_device_state *state = &iface->hid_device_state;
    ULONG i;

    for (i = 0; i < 11; ++i)
        hid_device_set_button(iface, i, FALSE);
    hid_device_set_abs_axis(iface, 0, 0);
    hid_device_set_abs_axis(iface, 1, 0);
    hid_device_set_abs_axis(iface, 2, 0);
    hid_device_set_abs_axis(iface, 3, 0);
    hid_device_set_abs_axis(iface, 4, 0);
    hid_device_set_abs_axis(iface, 5, 0);
    hid_device_set_hatswitch_x(iface, 0, 0);
    hid_device_set_hatswitch_y(iface, 0, 0);
    bus_event_queue_input_report(&event_queue, iface, state->report_buf, state->report_len);
}

static void apply_state(struct unix_device *iface, const struct whgp_state_v2 *body)
{
    struct hid_device_state *state = &iface->hid_device_state;
    ULONG i;

    for (i = 0; i < 11; ++i)
        hid_device_set_button(iface, i, (body->buttons & (1u << i)) != 0);

    /* WHGP v2 is Canonical Controller Space (+Y = Up), which matches XInput /
     * GIP stick polarity. Pass analog axes through. bus_sdl inverts SDL raw
     * thumbs (up is negative); that adapter rule does not apply to this
     * WHGP canonical sink. */
    hid_device_set_abs_axis(iface, 0, body->lx);
    hid_device_set_abs_axis(iface, 1, whgp_stick_y_to_hid(body->ly));
    hid_device_set_abs_axis(iface, 2, body->rx);
    hid_device_set_abs_axis(iface, 3, whgp_stick_y_to_hid(body->ry));
    hid_device_set_abs_axis(iface, 4, body->lt);
    hid_device_set_abs_axis(iface, 5, body->rt);

    /* HID hatswitch helper is +Y = Down; WHGP hat_y is canonical +Y = Up. */
    hid_device_set_hatswitch_x(iface, 0, body->hat_x);
    hid_device_set_hatswitch_y(iface, 0, whgp_hat_y_to_hid(body->hat_y));

    bus_event_queue_input_report(&event_queue, iface, state->report_buf, state->report_len);
}

static void close_socket(void)
{
    if (sock_fd >= 0)
    {
        shutdown(sock_fd, SHUT_RDWR);
        close(sock_fd);
        sock_fd = -1;
    }
}

static BOOL connect_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (!path || !path[0]) return FALSE;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        WARN("WHGP socket() failed errno=%d\n", errno);
        return FALSE;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
    {
        close(fd);
        return FALSE;
    }
    strcpy(addr.sun_path, path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        if (!logged_connect_fail)
        {
            fprintf(stderr, "[winebus-ohos] WHGP connect %s errno=%d (will retry)\n", path, errno);
            fflush(stderr);
            logged_connect_fail = TRUE;
        }
        TRACE("WHGP connect %s failed errno=%d\n", debugstr_a(path), errno);
        close(fd);
        return FALSE;
    }

    sock_fd = fd;
    logged_connect_fail = FALSE;
    fprintf(stderr, "[winebus-ohos] WHGP connected to %s\n", path);
    fflush(stderr);
    TRACE("WHGP connected to %s\n", debugstr_a(path));
    return TRUE;
}

static void ohos_device_destroy(struct unix_device *iface)
{
}

static NTSTATUS ohos_device_start(struct unix_device *iface)
{
    struct ohos_device *impl = impl_from_unix_device(iface);
    pthread_mutex_lock(&ohos_cs);
    impl->started = TRUE;
    pthread_mutex_unlock(&ohos_cs);
    return STATUS_SUCCESS;
}

static void ohos_device_stop(struct unix_device *iface)
{
    struct ohos_device *impl = impl_from_unix_device(iface);
    pthread_mutex_lock(&ohos_cs);
    impl->started = FALSE;
    list_remove(&impl->unix_device.entry);
    pthread_mutex_unlock(&ohos_cs);
}

static USHORT max_ushort(USHORT a, USHORT b)
{
    return a > b ? a : b;
}

static BOOL send_whgp_rumble(USHORT low, USHORT high, UINT duration_ms)
{
    struct whgp_header hdr;
    struct whgp_rumble_v1 body;
    struct iovec iov[2];
    int fd;

    memset(&hdr, 0, sizeof(hdr));
    memset(&body, 0, sizeof(body));
    hdr.magic = WHGP_MAGIC;
    hdr.version = WHGP_VERSION;
    hdr.msg_type = WHGP_MSG_RUMBLE;
    hdr.slot = 0;
    hdr.payload_size = sizeof(body);
    body.low = low;
    body.high = high;
    body.duration_ms = duration_ms;

    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = &body;
    iov[1].iov_len = sizeof(body);

    pthread_mutex_lock(&ohos_cs);
    fd = sock_fd;
    if (fd < 0)
    {
        pthread_mutex_unlock(&ohos_cs);
        return FALSE;
    }
    if (writev(fd, iov, 2) != (ssize_t)(sizeof(hdr) + sizeof(body)))
    {
        WARN("WHGP rumble write failed errno=%d\n", errno);
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == EBADF)
            close_socket();
        pthread_mutex_unlock(&ohos_cs);
        return FALSE;
    }
    pthread_mutex_unlock(&ohos_cs);
    return TRUE;
}

static NTSTATUS ohos_device_haptics_start(struct unix_device *iface, UINT duration_ms,
                                          USHORT rumble_intensity, USHORT buzz_intensity,
                                          USHORT left_intensity, USHORT right_intensity)
{
    USHORT low = max_ushort(rumble_intensity, left_intensity);
    USHORT high = max_ushort(buzz_intensity, right_intensity);

    TRACE("iface %p duration %u rumble %u buzz %u left %u right %u\n",
          iface, duration_ms, rumble_intensity, buzz_intensity, left_intensity, right_intensity);

    if (!duration_ms) duration_ms = 250;
    send_whgp_rumble(low, high, duration_ms);
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_device_haptics_stop(struct unix_device *iface)
{
    TRACE("iface %p\n", iface);
    send_whgp_rumble(0, 0, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS ohos_device_physical_device_control(struct unix_device *iface, USAGE control)
{
    TRACE("iface %p control %#x (stub)\n", iface, control);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS ohos_device_physical_device_set_gain(struct unix_device *iface, BYTE percent)
{
    TRACE("iface %p gain %u (stub)\n", iface, percent);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS ohos_device_physical_effect_control(struct unix_device *iface, BYTE index,
                                                    USAGE control, BYTE iterations)
{
    TRACE("iface %p (stub)\n", iface);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS ohos_device_physical_effect_update(struct unix_device *iface, BYTE index,
                                                   struct effect_params *params)
{
    TRACE("iface %p (stub)\n", iface);
    return STATUS_NOT_SUPPORTED;
}

static const struct hid_device_vtbl ohos_device_vtbl =
{
    ohos_device_destroy,
    ohos_device_start,
    ohos_device_stop,
    ohos_device_haptics_start,
    ohos_device_haptics_stop,
    ohos_device_physical_device_control,
    ohos_device_physical_device_set_gain,
    ohos_device_physical_effect_control,
    ohos_device_physical_effect_update,
};

static BOOL create_virtual_gamepad(UINT slot)
{
    static const USAGE_AND_PAGE device_usage =
        {.UsagePage = HID_USAGE_PAGE_GENERIC, .Usage = HID_USAGE_GENERIC_GAMEPAD};
    struct device_desc desc =
    {
        .vid = 0x1209,
        .pid = 0x0001,
        .version = 0x0100,
        .input = -1,
        .uid = 0x57484750, /* 'WHGP' */
        .bus_type = BUS_TYPE_USB,
        .is_gamepad = TRUE, /* XInput compat GUID + DirectInput HID */
        .manufacturer = {'W','i','n','e','H','u','a',0},
        .product = {'W','i','n','e','H','u','a',' ','V','i','r','t','u','a','l',' ','G','a','m','e','p','a','d',0},
        .serialnumber = {'0','0','0','1',0},
    };
    struct ohos_device *impl;

    if (!(impl = hid_device_create(&ohos_device_vtbl, sizeof(*impl))))
        return FALSE;

    impl->slot = slot;
    impl->started = FALSE;
    list_add_tail(&device_list, &impl->unix_device.entry);

    if (!hid_device_begin_report_descriptor(&impl->unix_device, &device_usage) ||
        !hid_device_add_gamepad(&impl->unix_device) ||
        !hid_device_add_haptics(&impl->unix_device) ||
        !hid_device_end_report_descriptor(&impl->unix_device))
    {
        ERR("failed to build gamepad descriptor\n");
        list_remove(&impl->unix_device.entry);
        return FALSE;
    }

    bus_event_queue_device_created(&event_queue, &impl->unix_device, &desc);
    TRACE("created WineHua Virtual Gamepad slot %u\n", slot);
    return TRUE;
}

NTSTATUS ohos_bus_init(void *args)
{
    const char *enable, *mode, *path;

    TRACE("args %p\n", args);
    quit_requested = FALSE;
    logged_connect_fail = FALSE;

    enable = getenv("WINEHUA_GAMEPAD_ENABLE");
    mode = getenv("WINEHUA_GAMEPAD_MODE");
    path = resolve_socket_path();

    /* Only the explicit keyboard fallback opts out. Missing ENABLE used to
     * kill this bus during wineboot (winedevice starts before explorer env). */
    if (mode && !strcmp(mode, "keyboard_legacy"))
    {
        TRACE("keyboard_legacy mode; OHOS bus inactive\n");
        return STATUS_NOT_IMPLEMENTED;
    }
    if (enable && !strcmp(enable, "0"))
    {
        TRACE("WINEHUA_GAMEPAD_ENABLE=0; OHOS bus inactive\n");
        return STATUS_NOT_IMPLEMENTED;
    }

    fprintf(stderr, "[winebus-ohos] init socket=%s enable=%s mode=%s\n",
            path ? path : "(null)", enable ? enable : "(unset)", mode ? mode : "(unset)");
    fflush(stderr);

    pthread_mutex_lock(&ohos_cs);
    if (!create_virtual_gamepad(0))
    {
        pthread_mutex_unlock(&ohos_cs);
        return STATUS_UNSUCCESSFUL;
    }
    connect_socket(path); /* may fail; wait loop retries */
    pthread_mutex_unlock(&ohos_cs);
    return STATUS_SUCCESS;
}

static BOOL read_exact(int fd, void *buf, size_t len)
{
    BYTE *p = buf;
    size_t got = 0;

    while (got < len)
    {
        ssize_t n = read(fd, p + got, len - got);
        if (n == 0) return FALSE;
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return FALSE;
        }
        got += (size_t)n;
    }
    return TRUE;
}

static void drop_socket_if(int fd)
{
    pthread_mutex_lock(&ohos_cs);
    if (sock_fd == fd) close_socket();
    pthread_mutex_unlock(&ohos_cs);
}

static void process_one_message(struct unix_device *iface, int fd)
{
    struct whgp_header hdr;
    struct whgp_state_v2 body;
    BYTE discard[256];
    UINT left;

    if (fd < 0) return;

    if (!read_exact(fd, &hdr, sizeof(hdr)))
    {
        WARN("WHGP socket disconnected\n");
        drop_socket_if(fd);
        apply_neutral(iface);
        return;
    }
    if (hdr.magic != WHGP_MAGIC)
    {
        ERR("WHGP bad magic %08x\n", hdr.magic);
        drop_socket_if(fd);
        apply_neutral(iface);
        return;
    }
    if (!whgp_version_matches(hdr.version))
    {
        ERR("WHGP protocol mismatch: peer=%u expected=2\n", hdr.version);
        drop_socket_if(fd);
        apply_neutral(iface);
        return;
    }

    if (hdr.msg_type == WHGP_MSG_STATE && hdr.payload_size == sizeof(body) && hdr.slot == 0)
    {
        if (!read_exact(fd, &body, sizeof(body)))
        {
            drop_socket_if(fd);
            apply_neutral(iface);
            return;
        }
        apply_state(iface, &body);
        return;
    }

    left = hdr.payload_size;
    while (left)
    {
        UINT chunk = left > sizeof(discard) ? sizeof(discard) : left;
        if (!read_exact(fd, discard, chunk))
        {
            drop_socket_if(fd);
            apply_neutral(iface);
            return;
        }
        left -= chunk;
    }
    if (hdr.msg_type == WHGP_MSG_RESET)
        apply_neutral(iface);
}

NTSTATUS ohos_bus_wait(void *args)
{
    struct bus_event *result = args;
    struct ohos_device *impl;
    struct unix_device *dev;
    const char *path;
    struct pollfd pfd;
    int fd;

    bus_event_cleanup(result);
    path = resolve_socket_path();

    do
    {
        if (bus_event_queue_pop(&event_queue, result))
            return STATUS_PENDING;

        pthread_mutex_lock(&ohos_cs);
        if (quit_requested)
        {
            pthread_mutex_unlock(&ohos_cs);
            break;
        }
        if (sock_fd < 0 && path)
            connect_socket(path);
        fd = sock_fd;
        dev = NULL;
        if (!list_empty(&device_list))
        {
            impl = LIST_ENTRY(list_head(&device_list), struct ohos_device, unix_device.entry);
            dev = &impl->unix_device;
        }
        pthread_mutex_unlock(&ohos_cs);

        if (fd >= 0)
        {
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 20) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) && dev)
                process_one_message(dev, fd);
        }
        else
            usleep(50000);
    } while (!quit_requested);

    TRACE("OHOS bus main loop exiting\n");
    pthread_mutex_lock(&ohos_cs);
    close_socket();
    bus_event_queue_destroy(&event_queue);
    pthread_mutex_unlock(&ohos_cs);
    return STATUS_SUCCESS;
}

NTSTATUS ohos_bus_stop(void *args)
{
    TRACE("args %p\n", args);
    pthread_mutex_lock(&ohos_cs);
    quit_requested = TRUE;
    close_socket();
    pthread_mutex_unlock(&ohos_cs);
    return STATUS_SUCCESS;
}
