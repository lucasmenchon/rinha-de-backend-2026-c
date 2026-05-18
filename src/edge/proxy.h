// Proxy TCP edge: aceita em :9999, distribui round-robin entre upstreams
// Unix-socket. Cada conexao cliente e mantida vinculada a um upstream.

#ifndef RNH_EDGE_PROXY_H
#define RNH_EDGE_PROXY_H

#include <stdint.h>

typedef struct {
    uint16_t   port;
    int        backlog;
    const char *const *upstreams;  // array NULL-terminated de paths Unix
    int        nupstreams;
    uint32_t   buf_size;           // tamanho do buffer por sentido (default 16K)
} rnh_proxy_opts_t;

int rnh_proxy_run(const rnh_proxy_opts_t *opts);

#endif
