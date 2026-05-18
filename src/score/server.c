#define _GNU_SOURCE
#include "server.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "../transport/http.h"
#include "../transport/net.h"

// ----------------------------------------------------------------------------
// Estado por conexao. Pool fixo, indexavel por fd via tabela esparsa.
// ----------------------------------------------------------------------------

#define WRITE_BUF_SIZE   16384u

typedef struct {
    int            fd;
    rnh_http_req_t req;
    // Fila de escrita por conexao: respostas pre-formatadas sao concatenadas.
    uint8_t        wbuf[WRITE_BUF_SIZE];
    uint32_t       wfill;
    uint32_t       woff;
    int            want_close_after_flush;
} conn_t;

static volatile sig_atomic_t s_quit = 0;
static void on_sig(int sig) { (void)sig; s_quit = 1; }

static conn_t *conn_table = NULL;   // indexado por fd
static int     conn_table_cap = 0;

static conn_t *conn_get(int fd) {
    if (fd < 0 || fd >= conn_table_cap) return NULL;
    return &conn_table[fd];
}

static void conn_reset(conn_t *c, int fd) {
    c->fd    = fd;
    c->wfill = 0;
    c->woff  = 0;
    c->want_close_after_flush = 0;
    rnh_http_req_init(&c->req);
}

static void conn_close(int epfd, int fd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    conn_t *c = conn_get(fd);
    if (c) c->fd = -1;
}

static int append_resp(conn_t *c, const rnh_http_blob_t *b) {
    if (c->wfill + b->len > WRITE_BUF_SIZE) return -1;
    memcpy(c->wbuf + c->wfill, b->bytes, b->len);
    c->wfill += b->len;
    return 0;
}

// Tenta drenar wbuf para o socket. Retorna 0 ok (pode ou nao ter terminado),
// -1 erro fatal de conexao.
static int try_flush(conn_t *c) {
    while (c->woff < c->wfill) {
        ssize_t w = write(c->fd, c->wbuf + c->woff, c->wfill - c->woff);
        if (w > 0) { c->woff += (uint32_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    c->wfill = c->woff = 0;
    return 0;
}

// Processa todos os requests completos no buffer. Retorna 0 ok, -1 erro fatal.
static int handle_requests(const rnh_service_t *svc, conn_t *c) {
    for (;;) {
        rnh_http_result_t r = rnh_http_parse(&c->req);
        if (r == RNH_HTTP_NEED_MORE) return 0;

        if (r == RNH_HTTP_BAD) {
            (void)append_resp(c, &rnh_http_resp_bad);
            c->want_close_after_flush = 1;
            return 0;
        }

        const rnh_http_blob_t *resp = NULL;
        int close = !c->req.keep_alive;

        if (r == RNH_HTTP_OK_READY) {
            resp = close ? &rnh_http_resp_ready_close : &rnh_http_resp_ready;
        } else { // RNH_HTTP_OK_FRAUD
            int frauds = 0;
            if (rnh_service_score(svc, c->req.body, c->req.body_len, &frauds) != 0) {
                resp = &rnh_http_resp_bad;
                close = 1;
            } else {
                if (frauds < 0) frauds = 0; if (frauds > 5) frauds = 5;
                resp = close ? &rnh_http_resp_score_close[frauds] : &rnh_http_resp_score[frauds];
            }
        }
        if (append_resp(c, resp) != 0) {
            // sem espaco para enfileirar; flush antes
            if (try_flush(c) != 0) return -1;
            if (append_resp(c, resp) != 0) return -1;
        }
        if (close) c->want_close_after_flush = 1;

        rnh_http_req_drain(&c->req, c->req.total);
        if (c->want_close_after_flush) return 0;
    }
}

int rnh_server_run(rnh_server_t *srv) {
    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGPIPE, SIG_IGN);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return -1; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, srv->listen_fd, &ev) != 0) {
        perror("epoll_ctl listen"); close(epfd); return -1;
    }

    // Estimativa do fd max: ulimit nofile. Defaultamos generoso.
    conn_table_cap = 65536;
    conn_table = calloc(conn_table_cap, sizeof(conn_t));
    if (!conn_table) { perror("calloc conns"); close(epfd); return -1; }
    for (int i = 0; i < conn_table_cap; i++) conn_table[i].fd = -1;

    struct epoll_event events[256];
    while (!s_quit) {
        int n = epoll_wait(epfd, events, 256, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t ev_mask = events[i].events;

            if (fd == srv->listen_fd) {
                for (;;) {
                    int nfd = rnh_accept(srv->listen_fd);
                    if (nfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (nfd >= conn_table_cap) { close(nfd); continue; }
                    conn_t *c = &conn_table[nfd];
                    conn_reset(c, nfd);
                    struct epoll_event cev = { .events = EPOLLIN | EPOLLET, .data.fd = nfd };
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, nfd, &cev) != 0) {
                        close(nfd); c->fd = -1; continue;
                    }
                }
                continue;
            }

            conn_t *c = conn_get(fd);
            if (!c || c->fd != fd) { close(fd); continue; }

            if (ev_mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                conn_close(epfd, fd);
                continue;
            }

            if (ev_mask & EPOLLIN) {
                int fatal = 0;
                for (;;) {
                    if (c->req.fill >= RNH_HTTP_BUF_SIZE) { fatal = 1; break; }
                    ssize_t rd = read(fd, c->req.buf + c->req.fill,
                                      RNH_HTTP_BUF_SIZE - c->req.fill);
                    if (rd > 0) { c->req.fill += (uint32_t)rd; continue; }
                    if (rd == 0) { fatal = 1; break; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    fatal = 1; break;
                }
                if (handle_requests(srv->svc, c) != 0) fatal = 1;
                if (fatal) { conn_close(epfd, fd); continue; }
            }

            if (try_flush(c) != 0) { conn_close(epfd, fd); continue; }

            // Se precisa fechar e flush terminou, fecha.
            if (c->want_close_after_flush && c->wfill == 0) {
                conn_close(epfd, fd);
                continue;
            }
            // Garante EPOLLOUT enquanto wbuf nao drenou totalmente.
            uint32_t want = EPOLLIN | EPOLLET;
            if (c->wfill > 0) want |= EPOLLOUT;
            struct epoll_event mev = { .events = want, .data.fd = fd };
            (void)epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &mev);
        }
    }

    free(conn_table);
    close(epfd);
    return 0;
}
