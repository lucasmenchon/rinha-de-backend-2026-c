// Primitivas de socket: TCP listen (SO_REUSEPORT), Unix listen, connect,
// non-blocking, set keepalive/nodelay. Camada IO baixa.

#ifndef RNH_TRANSPORT_NET_H
#define RNH_TRANSPORT_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// TCP listen em :port, com SO_REUSEPORT (varios processos podem bindar).
// Retorna fd >= 0 ou -1.
int rnh_tcp_listen(uint16_t port, int backlog);

// Unix listen no caminho (remove se existir). chmod 0666 para ser acessivel
// entre containers.
int rnh_unix_listen(const char *path, int backlog);

// Unix connect blocking. Retorna fd ou -1.
int rnh_unix_connect(const char *path);

// Marca fd como non-blocking + CLOEXEC.
int rnh_set_nonblock(int fd);

// TCP_NODELAY on um fd TCP (ignora silenciosamente erro em Unix sockets).
void rnh_tcp_tune(int fd);

// Wrapper para accept4 com flags NONBLOCK|CLOEXEC, retorna -1 com errno em
// caso de EAGAIN tambem (chamador deve checar).
int rnh_accept(int listen_fd);

#endif
