#define _GNU_SOURCE
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int rnh_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    return 0;
}

void rnh_tcp_tune(int fd) {
    int yes = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

int rnh_tcp_listen(uint16_t port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) goto bad;
    if (listen(fd, backlog) != 0) goto bad;
    return fd;
bad:
    { int e = errno; close(fd); errno = e; return -1; }
}

int rnh_unix_listen(const char *path, int backlog) {
    (void)unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    size_t pl = strlen(path);
    if (pl >= sizeof(sa.sun_path)) { close(fd); errno = ENAMETOOLONG; return -1; }
    memcpy(sa.sun_path, path, pl + 1);
    if (bind(fd, (struct sockaddr *)&sa, (socklen_t)(sizeof(sa.sun_family) + pl + 1)) != 0)
        goto bad;
    (void)chmod(path, 0666);
    if (listen(fd, backlog) != 0) goto bad;
    return fd;
bad:
    { int e = errno; close(fd); errno = e; return -1; }
}

int rnh_unix_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {0};
    sa.sun_family = AF_UNIX;
    size_t pl = strlen(path);
    if (pl >= sizeof(sa.sun_path)) { close(fd); errno = ENAMETOOLONG; return -1; }
    memcpy(sa.sun_path, path, pl + 1);
    if (connect(fd, (struct sockaddr *)&sa, (socklen_t)(sizeof(sa.sun_family) + pl + 1)) != 0) {
        int e = errno; close(fd); errno = e; return -1;
    }
    return fd;
}

int rnh_accept(int listen_fd) {
    return accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
}
