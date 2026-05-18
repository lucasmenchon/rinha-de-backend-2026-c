// Parser HTTP minimalista para POST /fraud-score e GET /ready.
// Tudo escrito em cima de buffers de tamanho fixo, sem malloc por request.

#ifndef RNH_TRANSPORT_HTTP_H
#define RNH_TRANSPORT_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RNH_HTTP_BUF_SIZE      8192u
#define RNH_HTTP_MAX_BODY      4096u

typedef enum {
    RNH_HTTP_NEED_MORE = 0,    // ainda nao recebeu request completo
    RNH_HTTP_OK_FRAUD  = 1,    // POST /fraud-score com body completo em `body`
    RNH_HTTP_OK_READY  = 2,    // GET /ready
    RNH_HTTP_BAD       = 3,    // request invalido
} rnh_http_result_t;

typedef struct {
    uint8_t  buf[RNH_HTTP_BUF_SIZE];
    uint32_t fill;             // bytes em buf
    bool     keep_alive;
    // saidas quando RNH_HTTP_OK_FRAUD:
    const uint8_t *body;
    uint32_t       body_len;
    uint32_t       total;      // bytes consumidos pelo request (request line + headers + body)
} rnh_http_req_t;

static inline void rnh_http_req_init(rnh_http_req_t *r) {
    r->fill = 0;
    r->keep_alive = true;
    r->body = NULL;
    r->body_len = 0;
    r->total = 0;
}

// Tenta parsear um request a partir do buf atual. Em caso de OK, body/body_len
// apontam para dentro do buf e total indica quantos bytes consumir.
rnh_http_result_t rnh_http_parse(rnh_http_req_t *r);

// Compacta o buffer apos consumir `total` bytes (drena o request processado).
static inline void rnh_http_req_drain(rnh_http_req_t *r, uint32_t total) {
    if (total >= r->fill) { r->fill = 0; return; }
    memmove(r->buf, r->buf + total, r->fill - total);
    r->fill -= total;
}

// ------------------- Respostas pre-formatadas -------------------
// Existem so 6 valores possiveis de fraud_score (0,0.2,0.4,0.6,0.8,1.0) e cada
// um determina approved unicamente. Mantemos 6 blobs prontos para escrita.

typedef struct {
    const char *bytes;
    uint32_t    len;
} rnh_http_blob_t;

extern const rnh_http_blob_t rnh_http_resp_score[6];     // index = frauds (0..5)
extern const rnh_http_blob_t rnh_http_resp_score_close[6]; // mesma coisa com Connection: close
extern const rnh_http_blob_t rnh_http_resp_ready;
extern const rnh_http_blob_t rnh_http_resp_ready_close;
extern const rnh_http_blob_t rnh_http_resp_bad;

#endif
