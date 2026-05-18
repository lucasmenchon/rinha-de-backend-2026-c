// Servidor epoll edge-triggered. Aceita conexoes em um Unix socket, processa
// requests com keep-alive e escreve respostas pre-formatadas.

#ifndef RNH_SCORE_SERVER_H
#define RNH_SCORE_SERVER_H

#include <stdint.h>

#include "service.h"

typedef struct {
    int                 listen_fd;
    const rnh_service_t *svc;
    uint32_t            max_conns;
} rnh_server_t;

// Bloqueia executando o loop ate receber SIGTERM/SIGINT.
int rnh_server_run(rnh_server_t *srv);

#endif
