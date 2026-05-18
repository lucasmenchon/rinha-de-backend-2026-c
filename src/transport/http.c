#include "http.h"

#include <string.h>

// ----------------------------------------------------------------------------
// Parser de headers minimalista. Procura "\r\n\r\n" e captura Content-Length
// e Connection: close. Tudo case-insensitive nos nomes de header.
// ----------------------------------------------------------------------------

static int ci_starts_with(const uint8_t *p, const uint8_t *end, const char *needle) {
    size_t n = strlen(needle);
    if ((size_t)(end - p) < n) return 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t a = p[i], b = (uint8_t)needle[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

rnh_http_result_t rnh_http_parse(rnh_http_req_t *r) {
    uint8_t *buf = r->buf;
    uint32_t fill = r->fill;

    if (fill < 4) return RNH_HTTP_NEED_MORE;

    // Detecta metodo + path imediatamente.
    int is_get_ready = 0, is_post_fraud = 0;
    if (fill >= 9 && memcmp(buf, "GET /read", 9) == 0) is_get_ready = 1;
    else if (fill >= 17 && memcmp(buf, "POST /fraud-score", 17) == 0) is_post_fraud = 1;

    if (!is_get_ready && !is_post_fraud) {
        // Pode ser apenas request line incompleta; espera mais bytes ate certo limite.
        if (fill > 256) return RNH_HTTP_BAD;
        return RNH_HTTP_NEED_MORE;
    }

    // Procura fim dos headers (memmem do glibc usa SIMD em buffers pequenos).
    uint8_t *eoh = NULL;
    {
        void *p = memmem(buf, fill, "\r\n\r\n", 4);
        if (p) eoh = (uint8_t *)p + 4;
    }
    if (!eoh) {
        if (fill >= RNH_HTTP_BUF_SIZE) return RNH_HTTP_BAD;
        return RNH_HTTP_NEED_MORE;
    }

    // Varre headers para Content-Length e Connection.
    uint32_t cl = 0;
    int      keep = 1;  // HTTP/1.1 default
    uint8_t *cur = buf;
    // pula request line ate \r\n
    while (cur + 1 < eoh - 2 && !(cur[0] == '\r' && cur[1] == '\n')) cur++;
    if (cur + 1 < eoh) cur += 2; // skip CRLF

    while (cur < eoh - 2) {
        uint8_t *line_end = cur;
        while (line_end + 1 < eoh && !(line_end[0] == '\r' && line_end[1] == '\n')) line_end++;
        if (ci_starts_with(cur, line_end, "content-length:")) {
            uint8_t *p = cur + strlen("content-length:");
            while (p < line_end && (*p == ' ' || *p == '\t')) p++;
            uint32_t v = 0;
            while (p < line_end && *p >= '0' && *p <= '9') { v = v*10u + (uint32_t)(*p - '0'); p++; }
            cl = v;
        } else if (ci_starts_with(cur, line_end, "connection:")) {
            uint8_t *p = cur + strlen("connection:");
            while (p < line_end && (*p == ' ' || *p == '\t')) p++;
            if (line_end - p >= 5 && (p[0]=='c'||p[0]=='C') && (p[1]=='l'||p[1]=='L'))
                keep = 0;  // "close"
        }
        cur = line_end + 2;
    }
    r->keep_alive = (keep != 0);

    if (is_get_ready) {
        r->total    = (uint32_t)(eoh - buf);
        r->body     = NULL;
        r->body_len = 0;
        return RNH_HTTP_OK_READY;
    }

    // POST /fraud-score: precisa body completo.
    if (cl > RNH_HTTP_MAX_BODY) return RNH_HTTP_BAD;
    uint32_t header_len = (uint32_t)(eoh - buf);
    if (header_len + cl > fill) {
        if (header_len + cl > RNH_HTTP_BUF_SIZE) return RNH_HTTP_BAD;
        return RNH_HTTP_NEED_MORE;
    }
    r->body     = eoh;
    r->body_len = cl;
    r->total    = header_len + cl;
    return RNH_HTTP_OK_FRAUD;
}

// ----------------------------------------------------------------------------
// Respostas pre-formatadas. Como score so pode ser 0/0.2/0.4/0.6/0.8/1.0,
// armazenamos os 6 corpos prontos para socket.
// ----------------------------------------------------------------------------

#define BODY0 "{\"approved\":true,\"fraud_score\":0.0}"   /* 35 */
#define BODY1 "{\"approved\":true,\"fraud_score\":0.2}"   /* 35 */
#define BODY2 "{\"approved\":true,\"fraud_score\":0.4}"   /* 35 */
#define BODY3 "{\"approved\":false,\"fraud_score\":0.6}"  /* 36 */
#define BODY4 "{\"approved\":false,\"fraud_score\":0.8}"  /* 36 */
#define BODY5 "{\"approved\":false,\"fraud_score\":1.0}"  /* 36 */

#define RESP_KA(clen, body) \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: application/json\r\n" \
    "Content-Length: " clen "\r\n" \
    "\r\n" body

#define RESP_CL(clen, body) \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: application/json\r\n" \
    "Content-Length: " clen "\r\n" \
    "Connection: close\r\n" \
    "\r\n" body

static const char R0KA[] = RESP_KA("35", BODY0);
static const char R1KA[] = RESP_KA("35", BODY1);
static const char R2KA[] = RESP_KA("35", BODY2);
static const char R3KA[] = RESP_KA("36", BODY3);
static const char R4KA[] = RESP_KA("36", BODY4);
static const char R5KA[] = RESP_KA("36", BODY5);

static const char R0CL[] = RESP_CL("35", BODY0);
static const char R1CL[] = RESP_CL("35", BODY1);
static const char R2CL[] = RESP_CL("35", BODY2);
static const char R3CL[] = RESP_CL("36", BODY3);
static const char R4CL[] = RESP_CL("36", BODY4);
static const char R5CL[] = RESP_CL("36", BODY5);

const rnh_http_blob_t rnh_http_resp_score[6] = {
    { R0KA, sizeof(R0KA) - 1 },
    { R1KA, sizeof(R1KA) - 1 },
    { R2KA, sizeof(R2KA) - 1 },
    { R3KA, sizeof(R3KA) - 1 },
    { R4KA, sizeof(R4KA) - 1 },
    { R5KA, sizeof(R5KA) - 1 },
};
const rnh_http_blob_t rnh_http_resp_score_close[6] = {
    { R0CL, sizeof(R0CL) - 1 },
    { R1CL, sizeof(R1CL) - 1 },
    { R2CL, sizeof(R2CL) - 1 },
    { R3CL, sizeof(R3CL) - 1 },
    { R4CL, sizeof(R4CL) - 1 },
    { R5CL, sizeof(R5CL) - 1 },
};

static const char READY_KA[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "\r\n"
    "ok";

static const char READY_CL[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "ok";

static const char BAD[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

const rnh_http_blob_t rnh_http_resp_ready       = { READY_KA, sizeof(READY_KA) - 1 };
const rnh_http_blob_t rnh_http_resp_ready_close = { READY_CL, sizeof(READY_CL) - 1 };
const rnh_http_blob_t rnh_http_resp_bad         = { BAD,      sizeof(BAD)      - 1 };
