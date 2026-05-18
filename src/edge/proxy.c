#define _GNU_SOURCE
#include "proxy.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../transport/net.h"

// ----------------------------------------------------------------------------
// Estado por par cliente <-> upstream. Indexavel por fd (qualquer dos dois lados).
// ----------------------------------------------------------------------------

typedef struct conn_s {
    int       fd;        // este lado
    int       peer_fd;   // o outro lado
    uint8_t  *buf;       // dados saindo deste lado para o peer
    uint32_t  fill;
    uint32_t  off;
    int       closed;
} conn_t;

static int       table_cap = 0;
static conn_t   *table = NULL;
static uint32_t  g_bufsize = 16384u;

static volatile sig_atomic_t s_quit = 0;
static void on_sig(int sig) { (void)sig; s_quit = 1; }

static conn_t *slot(int fd) {
    if (fd < 0 || fd >= table_cap) return NULL;
    return &table[fd];
}

static void pair_drop(int epfd, int fd) {
    conn_t *a = slot(fd);
    if (!a || a->fd != fd) return;
    int peer = a->peer_fd;
    conn_t *b = slot(peer);

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    free(a->buf); a->buf = NULL;
    a->fd = -1; a->peer_fd = -1; a->fill = 0; a->off = 0; a->closed = 1;

    if (b && b->fd == peer) {
        // tenta drenar o que ainda esta no buf do peer antes de fechar
        epoll_ctl(epfd, EPOLL_CTL_DEL, peer, NULL);
        close(peer);
        free(b->buf); b->buf = NULL;
        b->fd = -1; b->peer_fd = -1; b->fill = 0; b->off = 0; b->closed = 1;
    }
}

// Wakes peer's EPOLLOUT when this side gets fresh bytes to forward.
static int arm_peer_out(int epfd, conn_t *peer, int want_in) {
    if (!peer || peer->fd < 0) return 0;
    uint32_t ev = EPOLLET | (want_in ? EPOLLIN : 0);
    if (peer->fill > peer->off) ev |= EPOLLOUT;
    struct epoll_event e = { .events = ev, .data.fd = peer->fd };
    return epoll_ctl(epfd, EPOLL_CTL_MOD, peer->fd, &e);
}

// Le bytes do fd ate EAGAIN; armazena no buf do peer (que sera enviado para o peer).
static int pump_in(int epfd, conn_t *c) {
    conn_t *peer = slot(c->peer_fd);
    if (!peer) return -1;
    if (!peer->buf) return -1;
    for (;;) {
        if (peer->fill >= g_bufsize) {
            // buffer cheio: parar de ler ate peer drenar
            return 0;
        }
        ssize_t r = read(c->fd, peer->buf + peer->fill, g_bufsize - peer->fill);
        if (r > 0) { peer->fill += (uint32_t)r; continue; }
        if (r == 0) return -1;  // EOF
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

// Drena buf deste lado escrevendo no socket deste lado.
static int pump_out(conn_t *c) {
    while (c->off < c->fill) {
        ssize_t w = write(c->fd, c->buf + c->off, c->fill - c->off);
        if (w > 0) { c->off += (uint32_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    c->fill = c->off = 0;
    return 0;
}

int rnh_proxy_run(const rnh_proxy_opts_t *opts) {
    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGPIPE, SIG_IGN);

    g_bufsize = opts->buf_size ? opts->buf_size : 16384u;

    int lfd = rnh_tcp_listen(opts->port, opts->backlog ? opts->backlog : 1024);
    if (lfd < 0) { perror("tcp_listen"); return -1; }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); close(lfd); return -1; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    table_cap = 65536;
    table = calloc(table_cap, sizeof(conn_t));
    if (!table) { perror("calloc"); close(epfd); close(lfd); return -1; }
    for (int i = 0; i < table_cap; i++) table[i].fd = -1;

    uint32_t rr = 0;
    struct epoll_event events[256];

    while (!s_quit) {
        int n = epoll_wait(epfd, events, 256, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t em = events[i].events;

            if (fd == lfd) {
                for (;;) {
                    int cfd = rnh_accept(lfd);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }
                    rnh_tcp_tune(cfd);
                    const char *target = opts->upstreams[rr % (uint32_t)opts->nupstreams];
                    rr++;
                    int ufd = rnh_unix_connect(target);
                    if (ufd < 0) { close(cfd); continue; }
                    rnh_set_nonblock(ufd);

                    if (cfd >= table_cap || ufd >= table_cap) {
                        close(cfd); close(ufd); continue;
                    }
                    conn_t *c = &table[cfd];
                    conn_t *u = &table[ufd];
                    c->fd = cfd; c->peer_fd = ufd; c->fill = 0; c->off = 0;
                    c->buf = malloc(g_bufsize); c->closed = 0;
                    u->fd = ufd; u->peer_fd = cfd; u->fill = 0; u->off = 0;
                    u->buf = malloc(g_bufsize); u->closed = 0;
                    if (!c->buf || !u->buf) {
                        free(c->buf); free(u->buf);
                        close(cfd); close(ufd);
                        c->fd = -1; u->fd = -1;
                        continue;
                    }
                    struct epoll_event ce = { .events = EPOLLIN | EPOLLET, .data.fd = cfd };
                    struct epoll_event ue = { .events = EPOLLIN | EPOLLET, .data.fd = ufd };
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ce) != 0 ||
                        epoll_ctl(epfd, EPOLL_CTL_ADD, ufd, &ue) != 0) {
                        free(c->buf); free(u->buf);
                        close(cfd); close(ufd);
                        c->fd = -1; u->fd = -1;
                        continue;
                    }
                }
                continue;
            }

            conn_t *c = slot(fd);
            if (!c || c->fd != fd) { close(fd); continue; }

            int fatal = 0;
            if (em & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) fatal = 1;
            if (!fatal && (em & EPOLLIN))  { if (pump_in(epfd, c) != 0) fatal = 1; }
            if (!fatal && (em & EPOLLOUT)) { if (pump_out(c)        != 0) fatal = 1; }

            // Sempre tenta drenar este lado tambem (pode haver dados acumulados).
            if (!fatal && c->fill > c->off) { if (pump_out(c) != 0) fatal = 1; }

            if (fatal) { pair_drop(epfd, fd); continue; }

            // Atualiza interesse: este lado le se buf do peer tem espaco;
            // este lado escreve se ha bytes pendentes.
            conn_t *peer = slot(c->peer_fd);
            uint32_t ev_self = EPOLLET;
            if (peer && peer->fill < g_bufsize) ev_self |= EPOLLIN;
            if (c->fill > c->off)               ev_self |= EPOLLOUT;
            struct epoll_event se = { .events = ev_self, .data.fd = fd };
            (void)epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &se);
            (void)arm_peer_out(epfd, peer, peer && peer->fill < g_bufsize);
        }
    }

    for (int i = 0; i < table_cap; i++) {
        if (table[i].fd >= 0) close(table[i].fd);
        free(table[i].buf);
    }
    free(table);
    close(epfd);
    close(lfd);
    return 0;
}
