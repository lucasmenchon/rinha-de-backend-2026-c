// Entrypoint do binario edge (load balancer).
//
// Variaveis de ambiente:
//   RNH_LISTEN     porta TCP a escutar                 (default 9999)
//   RNH_UPSTREAMS  paths Unix separados por virgula    (default /run/rnh/score-a.sock,/run/rnh/score-b.sock)
//   RNH_BACKLOG    backlog do listen                   (default 4096)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proxy.h"

static const char *env_or(const char *k, const char *fb) {
    const char *v = getenv(k);
    return (v && *v) ? v : fb;
}

int main(void) {
    int  port    = atoi(env_or("RNH_LISTEN",  "9999"));
    int  backlog = atoi(env_or("RNH_BACKLOG", "4096"));
    char buf[1024];
    const char *src = env_or("RNH_UPSTREAMS",
                             "/run/rnh/score-a.sock,/run/rnh/score-b.sock");
    size_t sl = strlen(src);
    if (sl >= sizeof(buf)) { fprintf(stderr, "[edge] upstream list too long\n"); return 2; }
    memcpy(buf, src, sl + 1);

    const char *ups[16];
    int nup = 0;
    char *tok = strtok(buf, ",");
    while (tok && nup < (int)(sizeof(ups) / sizeof(ups[0]))) {
        ups[nup++] = tok;
        tok = strtok(NULL, ",");
    }
    if (nup == 0) { fprintf(stderr, "[edge] no upstreams\n"); return 2; }

    fprintf(stderr, "[edge] :%d backlog=%d upstreams=%d\n", port, backlog, nup);
    for (int i = 0; i < nup; i++) fprintf(stderr, "[edge]   - %s\n", ups[i]);

    rnh_proxy_opts_t opts = {
        .port       = (uint16_t)port,
        .backlog    = backlog,
        .upstreams  = ups,
        .nupstreams = nup,
        // Payload tipico = HTTP req com body JSON ~1KB; resposta ~100B.
        // 4KB cobre folgado e melhora cache locality.
        .buf_size   = 4096u,
    };
    return rnh_proxy_run(&opts) == 0 ? 0 : 1;
}
