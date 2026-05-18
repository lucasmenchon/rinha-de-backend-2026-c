// Servico de scoring: orquestracao pura entre parser de payload, vetorizacao,
// busca IVF e decisao. Nao faz IO de rede; apenas le do indice ja aberto.

#ifndef RNH_SCORE_SERVICE_H
#define RNH_SCORE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/vector.h"
#include "../index/reader.h"

typedef struct {
    const rnh_index_t *idx;
    rnh_norm_t         norm;       // constantes ja carregadas
    uint32_t           nprobe;     // numero de listas a visitar
} rnh_service_t;

void rnh_service_init(rnh_service_t *svc, const rnh_index_t *idx, uint32_t nprobe);

// Processa um body JSON da rota POST /fraud-score. Em sucesso retorna 0 e
// preenche *frauds_out (0..5). Em erro retorna -1.
int rnh_service_score(const rnh_service_t *svc,
                      const uint8_t *body, uint32_t body_len,
                      int *frauds_out);

#endif
