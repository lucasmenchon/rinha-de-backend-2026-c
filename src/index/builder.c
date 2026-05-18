#define _GNU_SOURCE
#include "builder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "../core/distance.h"
#include "../core/vector.h"
#include "format.h"

// ----------------------------------------------------------------------------
// CRC32C (mesma implementacao do reader, duplicada para nao acoplar).
// ----------------------------------------------------------------------------
static uint32_t crc32c_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            uint32_t m = (uint32_t)-(int32_t)(c & 1u);
            c = (c >> 1) ^ (0x82F63B78u & m);
        }
    }
    return ~c;
}

// ----------------------------------------------------------------------------
// RNG xorshift64* deterministico, para que builds reproduzam o mesmo indice.
// ----------------------------------------------------------------------------
static uint64_t xs_state;
static void     xs_seed(uint64_t s) { xs_state = s ? s : 0xC0FFEEu; }
static uint64_t xs_next(void) {
    uint64_t x = xs_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    xs_state = x;
    return x * 2685821657736338717ULL;
}

// ----------------------------------------------------------------------------
// Descompressao integral do gzip para um buffer em RAM.
// ----------------------------------------------------------------------------
static int slurp_gz(const char *path, uint8_t **out, size_t *out_len) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) return -1;
    size_t cap = 1u << 24;   // 16 MiB iniciais
    size_t len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { gzclose(gz); errno = ENOMEM; return -1; }
    for (;;) {
        if (len + (1u << 20) > cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); gzclose(gz); errno = ENOMEM; return -1; }
            buf = nb;
        }
        int n = gzread(gz, buf + len, (unsigned)((1u << 20)));
        if (n < 0) { free(buf); gzclose(gz); errno = EIO; return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    gzclose(gz);
    *out = buf;
    *out_len = len;
    return 0;
}

// ----------------------------------------------------------------------------
// Parser JSON minimalista. Aceita: [ { "vector":[..14 numbers..], "label":"fraud"|"legit" }, ... ]
// E lenente a chaves em qualquer ordem; ignora campos extras.
// ----------------------------------------------------------------------------
typedef struct {
    const char *p;
    const char *end;
} jp_t;

static void jp_skipws(jp_t *s) {
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') s->p++;
        else break;
    }
}

static int jp_eat(jp_t *s, char c) {
    jp_skipws(s);
    if (s->p < s->end && *s->p == c) { s->p++; return 1; }
    return 0;
}

static int jp_peek(jp_t *s) {
    jp_skipws(s);
    return s->p < s->end ? (unsigned char)*s->p : -1;
}

static int jp_number(jp_t *s, float *out) {
    jp_skipws(s);
    char *e = NULL;
    double v = strtod(s->p, &e);
    if (e == s->p) return -1;
    s->p = e;
    *out = (float)v;
    return 0;
}

static int jp_string(jp_t *s, const char **start, size_t *len) {
    jp_skipws(s);
    if (s->p >= s->end || *s->p != '"') return -1;
    s->p++;
    const char *b = s->p;
    while (s->p < s->end && *s->p != '"') {
        if (*s->p == '\\' && s->p + 1 < s->end) s->p += 2;
        else s->p++;
    }
    if (s->p >= s->end) return -1;
    *start = b;
    *len   = (size_t)(s->p - b);
    s->p++;  // consome o "
    return 0;
}

static int jp_skip_value(jp_t *s); // forward

static int jp_skip_array(jp_t *s) {
    if (!jp_eat(s, '[')) return -1;
    while (jp_peek(s) != ']') {
        if (jp_skip_value(s) != 0) return -1;
        jp_eat(s, ',');
    }
    return jp_eat(s, ']') ? 0 : -1;
}

static int jp_skip_object(jp_t *s) {
    if (!jp_eat(s, '{')) return -1;
    while (jp_peek(s) != '}') {
        const char *k; size_t kl;
        if (jp_string(s, &k, &kl) != 0) return -1;
        if (!jp_eat(s, ':')) return -1;
        if (jp_skip_value(s) != 0) return -1;
        jp_eat(s, ',');
    }
    return jp_eat(s, '}') ? 0 : -1;
}

static int jp_skip_value(jp_t *s) {
    int c = jp_peek(s);
    if (c == '"') { const char *x; size_t y; return jp_string(s, &x, &y); }
    if (c == '{') return jp_skip_object(s);
    if (c == '[') return jp_skip_array(s);
    if (c == 't' || c == 'f' || c == 'n') {
        while (s->p < s->end && ((unsigned)*s->p >= 'a' && (unsigned)*s->p <= 'z')) s->p++;
        return 0;
    }
    float dummy;
    return jp_number(s, &dummy);
}

// Le um registro { "vector":[14 floats], "label":"..." }; preenche out (16 int16)
// e label_out (0/1). Retorna 1 em sucesso, 0 em fim de array, -1 em erro.
static int read_record(jp_t *s, int16_t *out, uint8_t *label_out) {
    jp_skipws(s);
    if (jp_peek(s) == ']') return 0;
    if (!jp_eat(s, '{')) return -1;

    int got_vec = 0, got_lab = 0;
    memset(out, 0, 16 * sizeof(int16_t));

    while (jp_peek(s) != '}') {
        const char *k; size_t kl;
        if (jp_string(s, &k, &kl) != 0) return -1;
        if (!jp_eat(s, ':')) return -1;

        if (kl == 6 && memcmp(k, "vector", 6) == 0) {
            if (!jp_eat(s, '[')) return -1;
            for (int i = 0; i < RNH_DIMS; i++) {
                float v;
                if (jp_number(s, &v) != 0) return -1;
                out[i] = rnh_quantize(v);
                if (i + 1 < RNH_DIMS && !jp_eat(s, ',')) return -1;
            }
            if (!jp_eat(s, ']')) return -1;
            got_vec = 1;
        } else if (kl == 5 && memcmp(k, "label", 5) == 0) {
            const char *v; size_t vl;
            if (jp_string(s, &v, &vl) != 0) return -1;
            *label_out = (vl == 5 && memcmp(v, "fraud", 5) == 0) ? 1u : 0u;
            got_lab = 1;
        } else {
            if (jp_skip_value(s) != 0) return -1;
        }
        jp_eat(s, ',');
    }
    if (!jp_eat(s, '}')) return -1;
    return (got_vec && got_lab) ? 1 : -1;
}

// ----------------------------------------------------------------------------
// Ingestao: produz arrays packed [N][16] int16 + [N] u8 labels.
// ----------------------------------------------------------------------------
typedef struct {
    int16_t *qv;        // [count*16]
    uint8_t *lab;       // [count]
    uint32_t count;
    uint32_t cap;
} corpus_t;

static int corpus_reserve(corpus_t *c, uint32_t need) {
    if (need <= c->cap) return 0;
    uint32_t cap = c->cap ? c->cap : (1u << 16);
    while (cap < need) cap *= 2;
    int16_t *nv = realloc(c->qv, (size_t)cap * 16 * sizeof(int16_t));
    if (!nv) return -1;
    uint8_t *nl = realloc(c->lab, (size_t)cap);
    if (!nl) { c->qv = nv; return -1; }
    c->qv = nv; c->lab = nl; c->cap = cap;
    return 0;
}

static int ingest_json(const uint8_t *buf, size_t len, corpus_t *c) {
    jp_t s = { (const char *)buf, (const char *)buf + len };
    if (!jp_eat(&s, '[')) { errno = EINVAL; return -1; }

    int16_t tmp[16];
    for (;;) {
        if (corpus_reserve(c, c->count + 1) != 0) return -1;
        uint8_t lab;
        int r = read_record(&s, tmp, &lab);
        if (r < 0) { errno = EINVAL; return -1; }
        if (r == 0) break;
        memcpy(&c->qv[(size_t)c->count * 16], tmp, sizeof(tmp));
        c->lab[c->count++] = lab;
        jp_eat(&s, ',');
        if (jp_peek(&s) == ']') break;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// K-means Lloyd com amostra. Centroides em int16 (mesmo formato dos pontos).
// ----------------------------------------------------------------------------
static void kmeans_train(const int16_t *qv, uint32_t n,
                         int16_t *centroids, uint32_t k,
                         uint32_t sample, uint32_t iters)
{
    if (sample > n) sample = n;

    // Seed: pick k pontos da amostra como centroides iniciais.
    for (uint32_t c = 0; c < k; c++) {
        uint32_t idx = (uint32_t)(xs_next() % n);
        memcpy(&centroids[c * 16], &qv[(size_t)idx * 16], 32);
    }

    int64_t *sums    = calloc((size_t)k * 16, sizeof(int64_t));
    uint32_t *counts = calloc(k, sizeof(uint32_t));
    if (!sums || !counts) { free(sums); free(counts); return; }

    for (uint32_t it = 0; it < iters; it++) {
        memset(sums, 0, (size_t)k * 16 * sizeof(int64_t));
        memset(counts, 0, k * sizeof(uint32_t));

        // Estrategia: percorre uma amostra estratificada (stride uniforme).
        uint32_t stride = (n > sample) ? (n / sample) : 1;
        for (uint32_t i = 0; i < n; i += stride) {
            const int16_t *p = &qv[(size_t)i * 16];
            int64_t best = INT64_MAX;
            uint32_t bj  = 0;
            for (uint32_t j = 0; j < k; j++) {
                int64_t d = rnh_dist_l2sq(p, &centroids[j * 16]);
                if (d < best) { best = d; bj = j; }
            }
            int64_t *acc = &sums[(size_t)bj * 16];
            for (int d = 0; d < 16; d++) acc[d] += p[d];
            counts[bj]++;
        }

        // Atualiza centroides como media inteira. Centroides sem amostras
        // recebem um ponto aleatorio para evitar listas mortas.
        for (uint32_t j = 0; j < k; j++) {
            if (counts[j] == 0) {
                uint32_t idx = (uint32_t)(xs_next() % n);
                memcpy(&centroids[j * 16], &qv[(size_t)idx * 16], 32);
                continue;
            }
            const int64_t *acc = &sums[(size_t)j * 16];
            for (int d = 0; d < 16; d++) {
                int64_t m = acc[d] / (int64_t)counts[j];
                if (m >  32767) m =  32767;
                if (m < -32768) m = -32768;
                centroids[j * 16 + d] = (int16_t)m;
            }
        }
    }
    free(sums); free(counts);
}

// ----------------------------------------------------------------------------
// Atribui todos os vetores ao centroide mais proximo (assign final, full pass).
// ----------------------------------------------------------------------------
static void assign_all(const int16_t *qv, uint32_t n,
                       const int16_t *centroids, uint32_t k,
                       uint32_t *assign_out)
{
    for (uint32_t i = 0; i < n; i++) {
        const int16_t *p = &qv[(size_t)i * 16];
        int64_t best = INT64_MAX;
        uint32_t bj  = 0;
        for (uint32_t j = 0; j < k; j++) {
            int64_t d = rnh_dist_l2sq(p, &centroids[j * 16]);
            if (d < best) { best = d; bj = j; }
        }
        assign_out[i] = bj;
    }
}

// ----------------------------------------------------------------------------
// Escreve o arquivo final.
// ----------------------------------------------------------------------------
static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

// Pad para alinhamento `align` no fd, preenche com zeros.
static int pad_to(int fd, uint64_t cur, uint64_t target) {
    static const uint8_t zeros[4096] = {0};
    while (cur < target) {
        size_t chunk = target - cur > sizeof(zeros) ? sizeof(zeros) : (size_t)(target - cur);
        if (write_all(fd, zeros, chunk) != 0) return -1;
        cur += chunk;
    }
    return 0;
}

int rnh_build_run(const rnh_build_opts_t *opts) {
    xs_seed(opts->seed);

    fprintf(stderr, "[forge] decompress %s\n", opts->gz_path);
    uint8_t *raw = NULL; size_t raw_len = 0;
    if (slurp_gz(opts->gz_path, &raw, &raw_len) != 0) {
        perror("[forge] gzip"); return -1;
    }
    fprintf(stderr, "[forge] decompressed %zu bytes\n", raw_len);

    corpus_t corp = {0};
    if (ingest_json(raw, raw_len, &corp) != 0) {
        perror("[forge] json"); free(raw); return -1;
    }
    free(raw);
    fprintf(stderr, "[forge] parsed %u vectors\n", corp.count);

    uint32_t k = opts->nlists ? opts->nlists : RNH_NLISTS;
    int16_t *centroids = aligned_alloc(32, (size_t)k * 16 * sizeof(int16_t));
    if (!centroids) { errno = ENOMEM; return -1; }
    memset(centroids, 0, (size_t)k * 16 * sizeof(int16_t));

    fprintf(stderr, "[forge] kmeans k=%u sample=%u iters=%u\n",
            k, opts->kmeans_sample, opts->kmeans_iters);
    kmeans_train(corp.qv, corp.count, centroids, k,
                 opts->kmeans_sample, opts->kmeans_iters);

    uint32_t *assign = malloc((size_t)corp.count * sizeof(uint32_t));
    if (!assign) { free(centroids); free(corp.qv); free(corp.lab); errno = ENOMEM; return -1; }
    fprintf(stderr, "[forge] assign\n");
    assign_all(corp.qv, corp.count, centroids, k, assign);

    // Conta por lista e calcula offsets.
    uint32_t *counts = calloc(k, sizeof(uint32_t));
    uint32_t *frauds = calloc(k, sizeof(uint32_t));
    uint32_t *offs   = calloc(k, sizeof(uint32_t));
    if (!counts || !frauds || !offs) {
        free(centroids); free(corp.qv); free(corp.lab); free(assign);
        free(counts); free(frauds); free(offs);
        errno = ENOMEM; return -1;
    }
    for (uint32_t i = 0; i < corp.count; i++) {
        counts[assign[i]]++;
        if (corp.lab[i]) frauds[assign[i]]++;
    }
    uint32_t running = 0;
    for (uint32_t j = 0; j < k; j++) { offs[j] = running; running += counts[j]; }

    // Reordena vetores e labels para o layout agrupado.
    int16_t *ord_v  = aligned_alloc(32, (size_t)corp.count * 16 * sizeof(int16_t));
    uint8_t *ord_l  = malloc(corp.count);
    uint32_t *cur   = calloc(k, sizeof(uint32_t));
    if (!ord_v || !ord_l || !cur) {
        free(centroids); free(corp.qv); free(corp.lab); free(assign);
        free(counts); free(frauds); free(offs);
        free(ord_v); free(ord_l); free(cur);
        errno = ENOMEM; return -1;
    }
    for (uint32_t i = 0; i < corp.count; i++) {
        uint32_t j = assign[i];
        uint32_t p = offs[j] + cur[j]++;
        memcpy(&ord_v[(size_t)p * 16], &corp.qv[(size_t)i * 16], 32);
        ord_l[p] = corp.lab[i];
    }

    // Calcula layout do arquivo.
    uint64_t lists_off  = RNH_HEADER_SIZE;
    uint64_t vecs_off   = lists_off + (uint64_t)k * RNH_LIST_SLOT_SIZE;
    // alinhar a 32
    vecs_off = (vecs_off + 31u) & ~((uint64_t)31u);
    uint64_t labels_off = vecs_off + (uint64_t)corp.count * 32u;
    uint64_t file_size  = labels_off + (uint64_t)corp.count;

    // Monta header com crc.
    rnh_index_header_t hdr = {0};
    memcpy(hdr.magic, RNH_INDEX_MAGIC, 4);
    hdr.version       = RNH_INDEX_VERSION;
    hdr.nlists        = k;
    hdr.nvectors      = corp.count;
    hdr.lists_off     = lists_off;
    hdr.vecs_off      = vecs_off;
    hdr.labels_off    = labels_off;
    hdr.file_size     = file_size;
    hdr.header_crc32  = 0;
    hdr.header_crc32  = crc32c_buf((const uint8_t *)&hdr, sizeof(hdr));

    fprintf(stderr, "[forge] write %s (%llu bytes, %u lists, %u vectors)\n",
            opts->out_path, (unsigned long long)file_size, k, corp.count);

    int fd = open(opts->out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) goto fail;

    if (write_all(fd, &hdr, sizeof(hdr)) != 0) goto fail_fd;

    // Tabela de listas.
    rnh_list_slot_t slot;
    for (uint32_t j = 0; j < k; j++) {
        memset(&slot, 0, sizeof(slot));
        memcpy(slot.centroid, &centroids[j * 16], 32);
        slot.vec_offset  = offs[j];
        slot.vec_count   = counts[j];
        slot.fraud_count = frauds[j];
        if (write_all(fd, &slot, sizeof(slot)) != 0) goto fail_fd;
    }
    if (pad_to(fd, lists_off + (uint64_t)k * RNH_LIST_SLOT_SIZE, vecs_off) != 0)
        goto fail_fd;
    if (write_all(fd, ord_v, (size_t)corp.count * 32u) != 0) goto fail_fd;
    if (write_all(fd, ord_l, corp.count) != 0) goto fail_fd;

    close(fd);
    free(centroids); free(corp.qv); free(corp.lab); free(assign);
    free(counts); free(frauds); free(offs);
    free(ord_v); free(ord_l); free(cur);
    return 0;

fail_fd:
    close(fd);
    unlink(opts->out_path);
fail:
    free(centroids); free(corp.qv); free(corp.lab); free(assign);
    free(counts); free(frauds); free(offs);
    free(ord_v); free(ord_l); free(cur);
    return -1;
}
