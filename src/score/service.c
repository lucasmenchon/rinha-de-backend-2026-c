#include "service.h"

#include <stdlib.h>
#include <string.h>

#include "../core/distance.h"
#include "../core/knn.h"

// ----------------------------------------------------------------------------
// Tabela de risco MCC. O arquivo mcc_risk.json e fixo no desafio; embutimos.
// ----------------------------------------------------------------------------
typedef struct { const char *mcc; float risk; } mcc_entry_t;
static const mcc_entry_t MCC_TABLE[] = {
    { "5411", 0.15f }, { "5812", 0.30f }, { "5912", 0.20f }, { "5944", 0.45f },
    { "7801", 0.80f }, { "7802", 0.75f }, { "7995", 0.85f }, { "4511", 0.35f },
    { "5311", 0.25f }, { "5999", 0.50f },
};
#define MCC_TABLE_LEN (sizeof(MCC_TABLE) / sizeof(MCC_TABLE[0]))
#define MCC_DEFAULT   0.5f

static float mcc_lookup(const char *p, size_t n) {
    if (n != 4) return MCC_DEFAULT;
    for (size_t i = 0; i < MCC_TABLE_LEN; i++) {
        if (memcmp(p, MCC_TABLE[i].mcc, 4) == 0) return MCC_TABLE[i].risk;
    }
    return MCC_DEFAULT;
}

void rnh_service_init(rnh_service_t *svc, const rnh_index_t *idx, uint32_t nprobe) {
    svc->idx    = idx;
    svc->nprobe = nprobe ? nprobe : RNH_NPROBE;
    // Constantes do desafio. Sao fixas pela spec.
    svc->norm.max_amount              = 10000.0f;
    svc->norm.max_installments        = 12.0f;
    svc->norm.amount_vs_avg_ratio     = 10.0f;
    svc->norm.max_minutes             = 1440.0f;
    svc->norm.max_km                  = 1000.0f;
    svc->norm.max_tx_count_24h        = 20.0f;
    svc->norm.max_merchant_avg_amount = 10000.0f;
}

// ----------------------------------------------------------------------------
// Parser JSON especifico do payload de transacao.
// ----------------------------------------------------------------------------

typedef struct { const char *p; const char *end; } pj_t;

static void pj_ws(pj_t *s) {
    while (s->p < s->end) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') s->p++;
        else break;
    }
}
static int pj_eat(pj_t *s, char c) { pj_ws(s); if (s->p < s->end && *s->p == c) { s->p++; return 1; } return 0; }
static int pj_peek(pj_t *s) { pj_ws(s); return s->p < s->end ? (unsigned char)*s->p : -1; }

static int pj_string(pj_t *s, const char **start, size_t *len) {
    pj_ws(s);
    if (s->p >= s->end || *s->p != '"') return -1;
    s->p++;
    const char *b = s->p;
    while (s->p < s->end && *s->p != '"') {
        if (*s->p == '\\' && s->p + 1 < s->end) s->p += 2;
        else s->p++;
    }
    if (s->p >= s->end) return -1;
    *start = b; *len = (size_t)(s->p - b);
    s->p++;
    return 0;
}

static int pj_number(pj_t *s, double *out) {
    pj_ws(s);
    char *e;
    double v = strtod(s->p, &e);
    if (e == s->p) return -1;
    s->p = e; *out = v; return 0;
}

static int pj_bool(pj_t *s, int *out) {
    pj_ws(s);
    if (s->end - s->p >= 4 && memcmp(s->p, "true", 4) == 0) { *out = 1; s->p += 4; return 0; }
    if (s->end - s->p >= 5 && memcmp(s->p, "false", 5) == 0) { *out = 0; s->p += 5; return 0; }
    return -1;
}

static int pj_is_null(pj_t *s) {
    pj_ws(s);
    if (s->end - s->p >= 4 && memcmp(s->p, "null", 4) == 0) { s->p += 4; return 1; }
    return 0;
}

static int pj_skip(pj_t *s);

static int pj_skip_obj(pj_t *s) {
    if (!pj_eat(s, '{')) return -1;
    while (pj_peek(s) != '}') {
        const char *k; size_t kl;
        if (pj_string(s, &k, &kl) != 0) return -1;
        if (!pj_eat(s, ':')) return -1;
        if (pj_skip(s) != 0) return -1;
        pj_eat(s, ',');
    }
    return pj_eat(s, '}') ? 0 : -1;
}

static int pj_skip_arr(pj_t *s) {
    if (!pj_eat(s, '[')) return -1;
    while (pj_peek(s) != ']') {
        if (pj_skip(s) != 0) return -1;
        pj_eat(s, ',');
    }
    return pj_eat(s, ']') ? 0 : -1;
}

static int pj_skip(pj_t *s) {
    int c = pj_peek(s);
    if (c == '"') { const char *x; size_t y; return pj_string(s, &x, &y); }
    if (c == '{') return pj_skip_obj(s);
    if (c == '[') return pj_skip_arr(s);
    if (c == 't' || c == 'f') { int b; return pj_bool(s, &b); }
    if (c == 'n') return pj_is_null(s) ? 0 : -1;
    double d; return pj_number(s, &d);
}

// ----------------------------------------------------------------------------
// Conversao de timestamp ISO "YYYY-MM-DDTHH:MM:SSZ" -> componentes e epoch.
// ----------------------------------------------------------------------------

typedef struct { int Y, M, D, h, m, s; } ts_t;

static int parse_iso(const char *p, size_t len, ts_t *t) {
    if (len < 19) return -1;
    if (p[4] != '-' || p[7] != '-' || p[10] != 'T' || p[13] != ':' || p[16] != ':') return -1;
    for (int i = 0; i < 19; i++) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (p[i] < '0' || p[i] > '9') return -1;
    }
    t->Y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
    t->M = (p[5]-'0')*10 + (p[6]-'0');
    t->D = (p[8]-'0')*10 + (p[9]-'0');
    t->h = (p[11]-'0')*10 + (p[12]-'0');
    t->m = (p[14]-'0')*10 + (p[15]-'0');
    t->s = (p[17]-'0')*10 + (p[18]-'0');
    return 0;
}

// Dias civis desde 1970-01-01 (algoritmo de Howard Hinnant). Aceita datas
// proleptic gregorianas; nao precisa de tabelas de meses bissextos.
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int64_t epoch_seconds(const ts_t *t) {
    int64_t days = days_from_civil(t->Y, t->M, t->D);
    return days * 86400 + (int64_t)t->h * 3600 + (int64_t)t->m * 60 + (int64_t)t->s;
}

static int weekday_mon0(int y, int m, int d) {
    // dias desde 1970-01-01 (quinta-feira). seg=0..dom=6.
    int64_t days = days_from_civil(y, m, d);
    int w = (int)(((days % 7) + 7 + 3) % 7);  // shift: thu(0->3)->thu(3), need seg=0
    return w;
}

// ----------------------------------------------------------------------------
// Extracao dos features a partir do body.
// ----------------------------------------------------------------------------

static int extract_features(const uint8_t *body, uint32_t body_len, rnh_features_t *f) {
    memset(f, 0, sizeof(*f));
    f->mcc_risk = MCC_DEFAULT;
    f->unknown_merchant = true;  // default ate provarem o contrario

    pj_t s = { (const char *)body, (const char *)body + body_len };
    if (!pj_eat(&s, '{')) return -1;

    // Para usar mais tarde:
    const char *merchant_id = NULL; size_t merchant_id_len = 0;
    int     have_tx_ts = 0;  ts_t tx_ts = {0};
    int     have_last  = 0;  ts_t last_ts = {0};  double last_km = 0;
    const char *known_start = NULL; size_t known_len = 0;

    while (pj_peek(&s) != '}') {
        const char *k; size_t kl;
        if (pj_string(&s, &k, &kl) != 0) return -1;
        if (!pj_eat(&s, ':')) return -1;

        if (kl == 11 && memcmp(k, "transaction", 11) == 0) {
            if (!pj_eat(&s, '{')) return -1;
            while (pj_peek(&s) != '}') {
                const char *kk; size_t kkl;
                if (pj_string(&s, &kk, &kkl) != 0) return -1;
                if (!pj_eat(&s, ':')) return -1;
                if (kkl == 6 && memcmp(kk, "amount", 6) == 0) {
                    double v; if (pj_number(&s, &v) != 0) return -1;
                    f->amount = (float)v;
                } else if (kkl == 12 && memcmp(kk, "installments", 12) == 0) {
                    double v; if (pj_number(&s, &v) != 0) return -1;
                    f->installments = (float)v;
                } else if (kkl == 12 && memcmp(kk, "requested_at", 12) == 0) {
                    const char *v; size_t vl;
                    if (pj_string(&s, &v, &vl) != 0) return -1;
                    if (parse_iso(v, vl, &tx_ts) != 0) return -1;
                    have_tx_ts = 1;
                } else {
                    if (pj_skip(&s) != 0) return -1;
                }
                pj_eat(&s, ',');
            }
            if (!pj_eat(&s, '}')) return -1;
        } else if (kl == 8 && memcmp(k, "customer", 8) == 0) {
            if (!pj_eat(&s, '{')) return -1;
            while (pj_peek(&s) != '}') {
                const char *kk; size_t kkl;
                if (pj_string(&s, &kk, &kkl) != 0) return -1;
                if (!pj_eat(&s, ':')) return -1;
                if (kkl == 10 && memcmp(kk, "avg_amount", 10) == 0) {
                    double v; if (pj_number(&s, &v) != 0) return -1;
                    f->customer_avg_amount = (float)v;
                } else if (kkl == 12 && memcmp(kk, "tx_count_24h", 12) == 0) {
                    double v; if (pj_number(&s, &v) != 0) return -1;
                    f->tx_count_24h = (float)v;
                } else if (kkl == 15 && memcmp(kk, "known_merchants", 15) == 0) {
                    // captura o array inteiro como slice para checagem posterior
                    pj_ws(&s);
                    const char *arr_start = s.p;
                    if (pj_skip(&s) != 0) return -1;
                    known_start = arr_start;
                    known_len   = (size_t)(s.p - arr_start);
                } else {
                    if (pj_skip(&s) != 0) return -1;
                }
                pj_eat(&s, ',');
            }
            if (!pj_eat(&s, '}')) return -1;
        } else if (kl == 8 && memcmp(k, "merchant", 8) == 0) {
            if (!pj_eat(&s, '{')) return -1;
            while (pj_peek(&s) != '}') {
                const char *kk; size_t kkl;
                if (pj_string(&s, &kk, &kkl) != 0) return -1;
                if (!pj_eat(&s, ':')) return -1;
                if (kkl == 2 && memcmp(kk, "id", 2) == 0) {
                    if (pj_string(&s, &merchant_id, &merchant_id_len) != 0) return -1;
                } else if (kkl == 3 && memcmp(kk, "mcc", 3) == 0) {
                    const char *v; size_t vl;
                    if (pj_string(&s, &v, &vl) != 0) return -1;
                    f->mcc_risk = mcc_lookup(v, vl);
                } else if (kkl == 10 && memcmp(kk, "avg_amount", 10) == 0) {
                    double v; if (pj_number(&s, &v) != 0) return -1;
                    f->merchant_avg_amount = (float)v;
                } else {
                    if (pj_skip(&s) != 0) return -1;
                }
                pj_eat(&s, ',');
            }
            if (!pj_eat(&s, '}')) return -1;
        } else if (kl == 8 && memcmp(k, "terminal", 8) == 0) {
            if (!pj_eat(&s, '{')) return -1;
            while (pj_peek(&s) != '}') {
                const char *kk; size_t kkl;
                if (pj_string(&s, &kk, &kkl) != 0) return -1;
                if (!pj_eat(&s, ':')) return -1;
                if (kkl == 9 && memcmp(kk, "is_online", 9) == 0) {
                    int b; if (pj_bool(&s, &b) != 0) return -1;
                    f->is_online = (bool)b;
                } else if (kkl == 12 && memcmp(kk, "card_present", 12) == 0) {
                    int b; if (pj_bool(&s, &b) != 0) return -1;
                    f->card_present = (bool)b;
                } else if (kkl == 12 && memcmp(kk, "km_from_home", 12) == 0) {
                    double v; if (pj_number(&s, &v) != 0) return -1;
                    f->km_from_home = (float)v;
                } else {
                    if (pj_skip(&s) != 0) return -1;
                }
                pj_eat(&s, ',');
            }
            if (!pj_eat(&s, '}')) return -1;
        } else if (kl == 16 && memcmp(k, "last_transaction", 16) == 0) {
            if (pj_is_null(&s)) {
                have_last = 0;
            } else {
                if (!pj_eat(&s, '{')) return -1;
                while (pj_peek(&s) != '}') {
                    const char *kk; size_t kkl;
                    if (pj_string(&s, &kk, &kkl) != 0) return -1;
                    if (!pj_eat(&s, ':')) return -1;
                    if (kkl == 9 && memcmp(kk, "timestamp", 9) == 0) {
                        const char *v; size_t vl;
                        if (pj_string(&s, &v, &vl) != 0) return -1;
                        if (parse_iso(v, vl, &last_ts) != 0) return -1;
                        have_last = 1;
                    } else if (kkl == 15 && memcmp(kk, "km_from_current", 15) == 0) {
                        if (pj_number(&s, &last_km) != 0) return -1;
                    } else {
                        if (pj_skip(&s) != 0) return -1;
                    }
                    pj_eat(&s, ',');
                }
                if (!pj_eat(&s, '}')) return -1;
            }
        } else {
            // id, ou campos extras
            if (pj_skip(&s) != 0) return -1;
        }
        pj_eat(&s, ',');
    }

    if (!have_tx_ts) return -1;
    f->hour_utc = tx_ts.h;
    f->weekday  = weekday_mon0(tx_ts.Y, tx_ts.M, tx_ts.D);

    // checagem unknown_merchant: procura merchant_id no slice known_merchants
    if (merchant_id && known_start && known_len > 0) {
        // varredura simples: procura "<merchant_id>" como substring delimitada por aspas
        const char *p = known_start;
        const char *e = known_start + known_len;
        while (p + merchant_id_len + 1 < e) {
            if (*p == '"' &&
                memcmp(p + 1, merchant_id, merchant_id_len) == 0 &&
                p[1 + merchant_id_len] == '"') {
                f->unknown_merchant = false;
                break;
            }
            p++;
        }
    }

    if (have_last) {
        int64_t tx_sec   = epoch_seconds(&tx_ts);
        int64_t last_sec = epoch_seconds(&last_ts);
        int64_t diff_min = (tx_sec - last_sec) / 60;
        if (diff_min < 0) diff_min = 0;
        f->has_last_tx        = true;
        f->minutes_since_last = (float)diff_min;
        f->km_from_last       = (float)last_km;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Busca IVF: rank centroides, varre nprobe listas, top-K=5.
// ----------------------------------------------------------------------------

typedef struct { int64_t d; uint32_t list; } centd_t;

static void rank_centroids(const rnh_qvec_t q, const rnh_list_slot_t *lists,
                           uint32_t nlists, centd_t *out, uint32_t nprobe)
{
    // inicializa nprobe slots com infinito
    for (uint32_t i = 0; i < nprobe; i++) { out[i].d = INT64_MAX; out[i].list = 0; }
    for (uint32_t j = 0; j < nlists; j++) {
        int64_t d = rnh_dist_l2sq(q, lists[j].centroid);
        if (d >= out[nprobe - 1].d) continue;
        // insertion sort O(nprobe)
        int p = (int)nprobe - 1;
        while (p > 0 && d < out[p - 1].d) {
            out[p] = out[p - 1];
            p--;
        }
        out[p].d = d; out[p].list = j;
    }
}

int rnh_service_score(const rnh_service_t *svc,
                      const uint8_t *body, uint32_t body_len,
                      int *frauds_out)
{
    rnh_features_t f;
    if (extract_features(body, body_len, &f) != 0) return -1;

    rnh_qvec_t q;
    rnh_vector_build(&f, &svc->norm, q);

    uint32_t nprobe = svc->nprobe;
    if (nprobe > svc->idx->hdr->nlists) nprobe = svc->idx->hdr->nlists;

    centd_t probes[RNH_NPROBE * 2];  // upper bound de seguranca
    if (nprobe > sizeof(probes) / sizeof(probes[0])) nprobe = sizeof(probes) / sizeof(probes[0]);
    rank_centroids(q, svc->idx->lists, svc->idx->hdr->nlists, probes, nprobe);

    rnh_topk_t tk;
    rnh_topk_reset(&tk);

    const int16_t *vecs   = svc->idx->vectors;
    const uint8_t *labels = svc->idx->labels;

    for (uint32_t i = 0; i < nprobe; i++) {
        const rnh_list_slot_t *L = &svc->idx->lists[probes[i].list];
        const int16_t *vp = &vecs[(size_t)L->vec_offset * 16];
        const uint8_t *lp = &labels[L->vec_offset];
        uint32_t n = L->vec_count;
        for (uint32_t j = 0; j < n; j++) {
            int64_t d = rnh_dist_l2sq(q, &vp[(size_t)j * 16]);
            if (d < rnh_topk_worst(&tk)) {
                rnh_topk_offer(&tk, d, lp[j]);
            }
        }
    }

    *frauds_out = rnh_topk_fraud_count(&tk);
    return 0;
}
