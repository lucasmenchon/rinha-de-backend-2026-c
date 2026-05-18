// Entrypoint do binario score: abre o indice, bind no Unix socket, roda o
// servidor.
//
// Variaveis de ambiente:
//   RNH_INDEX    caminho do refs.idx                   (default /app/data/refs.idx)
//   RNH_SOCK     caminho do Unix socket de escuta      (default /run/rnh/score.sock)
//   RNH_NPROBE   numero de listas IVF a visitar        (default RNH_NPROBE)

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../index/format.h"
#include "../index/reader.h"
#include "../transport/net.h"
#include "server.h"
#include "service.h"

static const char *env_or(const char *k, const char *fb) {
    const char *v = getenv(k);
    return (v && *v) ? v : fb;
}

int main(void) {
    const char *idx_path = env_or("RNH_INDEX", "/app/data/refs.idx");
    const char *sock     = env_or("RNH_SOCK",  "/run/rnh/score.sock");
    uint32_t    nprobe   = (uint32_t)atoi(env_or("RNH_NPROBE", "0"));

    rnh_index_t idx;
    if (rnh_index_open(idx_path, &idx) != 0) {
        fprintf(stderr, "[score] index open %s: %s\n", idx_path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "[score] index %s loaded: nlists=%u nvectors=%u\n",
            idx_path, idx.hdr->nlists, idx.hdr->nvectors);

    // Trava todas as paginas do indice em RAM. Evita page-fault no p99
    // depois de qualquer pico de pressao de memoria. Best-effort: se o
    // RLIMIT_MEMLOCK for baixo, segue a vida (madvise WILLNEED ja popula).
    if (mlock(idx.base, idx.size) != 0) {
        // fallback: tranca pelo menos a tabela de listas + labels (hot path).
        (void)mlock(idx.lists, (size_t)idx.hdr->nlists * RNH_LIST_SLOT_SIZE);
        (void)mlock(idx.labels, (size_t)idx.hdr->nvectors);
        fprintf(stderr, "[score] mlock(all)=%s (fallback parcial aplicado)\n",
                strerror(errno));
    }

    rnh_service_t svc;
    rnh_service_init(&svc, &idx, nprobe);

    int lfd = rnh_unix_listen(sock, 1024);
    if (lfd < 0) {
        fprintf(stderr, "[score] unix_listen %s: %s\n", sock, strerror(errno));
        rnh_index_close(&idx);
        return 1;
    }
    fprintf(stderr, "[score] listening on %s, nprobe=%u\n", sock, svc.nprobe);

    rnh_server_t srv = { .listen_fd = lfd, .svc = &svc, .max_conns = 65536 };
    int rc = rnh_server_run(&srv);

    close(lfd);
    unlink(sock);
    rnh_index_close(&idx);
    return rc == 0 ? 0 : 1;
}
