// Builder do indice RNH4: le references.json.gz, quantiza, treina k-means
// (Lloyd com amostra), atribui todos os vetores aos clusters e grava o arquivo.
//
// Toda a parte pesada acontece na imagem de build do Docker; em runtime so o
// reader e usado.

#ifndef RNH_INDEX_BUILDER_H
#define RNH_INDEX_BUILDER_H

#include <stdint.h>

typedef struct {
    const char *gz_path;       // entrada: references.json.gz
    const char *out_path;      // saida: refs.idx
    uint32_t    nlists;        // numero de listas IVF (default RNH_NLISTS)
    uint32_t    kmeans_sample; // pontos usados para treinar centroides
    uint32_t    kmeans_iters;  // iteracoes de Lloyd
    uint64_t    seed;          // semente do RNG
} rnh_build_opts_t;

// Roda a pipeline completa. Retorna 0 em sucesso, -1 com errno setado.
int rnh_build_run(const rnh_build_opts_t *opts);

#endif
