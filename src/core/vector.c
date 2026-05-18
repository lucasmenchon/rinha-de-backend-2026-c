#include "vector.h"

#include <string.h>

int16_t rnh_quantize(float v) {
    if (v < 0.0f) return RNH_SENTINEL_Q;          // sentinela -1
    if (v >= 1.0f) return (int16_t)RNH_SCALE;     // satura no teto
    // arredondamento para mais proximo, evita truncamento sistematico.
    return (int16_t)(v * (float)RNH_SCALE + 0.5f);
}

// helper local: divisao normalizada com saturacao implicita via rnh_quantize.
static inline float ratio(float num, float den) {
    return den > 0.0f ? (num / den) : 0.0f;
}

void rnh_vector_build(const rnh_features_t *f, const rnh_norm_t *n, rnh_qvec_t out) {
    // Zera tudo de uma vez (inclui as duas lanes de padding finais).
    memset(out, 0, sizeof(rnh_qvec_t));

    // 0: amount
    out[0]  = rnh_quantize(ratio(f->amount, n->max_amount));
    // 1: installments
    out[1]  = rnh_quantize(ratio(f->installments, n->max_installments));
    // 2: amount_vs_avg = (amount / customer_avg) / amount_vs_avg_ratio
    float r2 = (f->customer_avg_amount > 0.0f)
        ? (f->amount / f->customer_avg_amount) / n->amount_vs_avg_ratio
        : 0.0f;
    out[2]  = rnh_quantize(r2);
    // 3: hour_of_day / 23  (resultado ja em [0,1])
    out[3]  = (int16_t)((f->hour_utc * RNH_SCALE + 11) / 23);  // /23 com round
    // 4: day_of_week / 6
    out[4]  = (int16_t)((f->weekday * RNH_SCALE + 3) / 6);
    // 5,6: sentinela quando nao ha transacao anterior
    if (!f->has_last_tx) {
        out[5] = RNH_SENTINEL_Q;
        out[6] = RNH_SENTINEL_Q;
    } else {
        out[5] = rnh_quantize(ratio(f->minutes_since_last, n->max_minutes));
        out[6] = rnh_quantize(ratio(f->km_from_last, n->max_km));
    }
    // 7: km_from_home
    out[7]  = rnh_quantize(ratio(f->km_from_home, n->max_km));
    // 8: tx_count_24h
    out[8]  = rnh_quantize(ratio(f->tx_count_24h, n->max_tx_count_24h));
    // 9..11: flags binarias
    out[9]  = (int16_t)(f->is_online        ? RNH_SCALE : 0);
    out[10] = (int16_t)(f->card_present     ? RNH_SCALE : 0);
    out[11] = (int16_t)(f->unknown_merchant ? RNH_SCALE : 0);
    // 12: mcc_risk (ja em [0,1])
    out[12] = rnh_quantize(f->mcc_risk);
    // 13: merchant_avg_amount
    out[13] = rnh_quantize(ratio(f->merchant_avg_amount, n->max_merchant_avg_amount));
    // 14,15 permanecem zero (padding SIMD)
}
