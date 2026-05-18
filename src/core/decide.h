// Decisao final: dado o numero de fraudes entre os 5 vizinhos mais proximos,
// produz approved + indice de bucket para resposta pre-formatada.

#ifndef RNH_CORE_DECIDE_H
#define RNH_CORE_DECIDE_H

#include <stdbool.h>

// frauds in [0..5], bucket equivalente.
// fraud_score = frauds/5, approved = score < 0.6  <=>  frauds < 3.
typedef struct {
    int  frauds;
    bool approved;
} rnh_verdict_t;

static inline rnh_verdict_t rnh_decide(int frauds) {
    rnh_verdict_t v;
    v.frauds   = frauds;
    v.approved = (frauds < 3);
    return v;
}

#endif
