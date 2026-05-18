// Top-K=5 ascendente por distancia. Implementacao em insercao linear
// (K pequeno, branchless-ish), API minima e header-only para inline agressivo.

#ifndef RNH_CORE_KNN_H
#define RNH_CORE_KNN_H

#include <stdint.h>
#include <stdbool.h>

#define RNH_K 5

typedef struct {
    int64_t  dist[RNH_K];   // ascendente; INT64_MAX = slot vazio
    uint8_t  label[RNH_K];  // 0=legit, 1=fraud
} rnh_topk_t;

static inline void rnh_topk_reset(rnh_topk_t *t) {
    for (int i = 0; i < RNH_K; i++) {
        t->dist[i]  = INT64_MAX;
        t->label[i] = 0;
    }
}

static inline int64_t rnh_topk_worst(const rnh_topk_t *t) {
    return t->dist[RNH_K - 1];
}

// Tenta inserir (d, lab). O caller ja deveria ter checado d < worst,
// mas tambem checamos aqui para seguranca.
static inline void rnh_topk_offer(rnh_topk_t *t, int64_t d, uint8_t lab) {
    if (d >= t->dist[RNH_K - 1]) return;
    int p = RNH_K - 1;
    while (p > 0 && d < t->dist[p - 1]) {
        t->dist[p]  = t->dist[p - 1];
        t->label[p] = t->label[p - 1];
        p--;
    }
    t->dist[p]  = d;
    t->label[p] = lab;
}

static inline int rnh_topk_fraud_count(const rnh_topk_t *t) {
    // labels sao 0/1, soma direta = numero de fraudes.
    return (int)(t->label[0] + t->label[1] + t->label[2] + t->label[3] + t->label[4]);
}

#endif
