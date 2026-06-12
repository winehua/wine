#define _GNU_SOURCE
#include <sys/epoll.h>
#include <errno.h>
__attribute__((weak))
int epoll_pwait2(int fd, struct epoll_event *ev, int n, const struct timespec *ts, const sigset_t *s)
{ (void)fd;(void)ev;(void)n;(void)ts;(void)s; errno=ENOSYS; return -1; }
