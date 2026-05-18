// Quantizacao e vetorizacao da carga POST /fraud-score.
// Camada PURA: nenhum syscall, nenhuma alocacao dinamica, totalmente testavel.

#ifndef RNH_CORE_VECTOR_H
#define RNH_CORE_VECTOR_H

#include <stdbool.h>
#include <stdint.h>

#define RNH_DIMS         14   // dimensoes uteis do vetor de fraude
#define RNH_LANES        16   // padding para SIMD (16-wide AVX2 int16)
#define RNH_SCALE        10000
#define RNH_SENTINEL_Q   ((int16_t)-RNH_SCALE)  // -1.0 normalizado

// Vetor quantizado, ja com padding de zeros nas lanes 14 e 15.
// Alinhado a 32 bytes para load/store AVX2 sem penalidade.
typedef int16_t rnh_qvec_t[RNH_LANES] __attribute__((aligned(32)));

// Constantes lidas de normalization.json (parsed uma vez na inicializacao).
typedef struct {
    float max_amount;
    float max_installments;
    float amount_vs_avg_ratio;
    float max_minutes;
    float max_km;
    float max_tx_count_24h;
    float max_merchant_avg_amount;
} rnh_norm_t;

// Campos ja extraidos do JSON pelo parser HTTP. Mantem a camada pura
// totalmente desacoplada do formato de entrada.
typedef struct {
    float    amount;
    float    installments;
    float    customer_avg_amount;
    int      hour_utc;            // 0..23
    int      weekday;             // 0=seg .. 6=dom
    bool     has_last_tx;
    float    minutes_since_last;  // valido se has_last_tx
    float    km_from_last;        // valido se has_last_tx
    float    km_from_home;
    float    tx_count_24h;
    bool     is_online;
    bool     card_present;
    bool     unknown_merchant;
    float    mcc_risk;            // ja resolvido (default 0.5 fora da camada pura)
    float    merchant_avg_amount;
} rnh_features_t;

// Quantiza um valor normalizado [0,1] para int16 no intervalo [0, RNH_SCALE].
// Valores < 0 sao tratados como sentinela (-1) e retornam RNH_SENTINEL_Q.
// Valores > 1 saturam em RNH_SCALE.
int16_t rnh_quantize(float v);

// Transforma os features em vetor quantizado de 16 lanes (14 uteis + 2 pad zero).
void rnh_vector_build(const rnh_features_t *f, const rnh_norm_t *n, rnh_qvec_t out);

#endif
