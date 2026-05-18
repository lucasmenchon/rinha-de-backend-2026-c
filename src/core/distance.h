// Distancia L2 quadrada entre vetores quantizados de 16 lanes int16.
// Implementacao escalar portavel + variante AVX2 ativada via __AVX2__.

#ifndef RNH_CORE_DISTANCE_H
#define RNH_CORE_DISTANCE_H

#include <stdint.h>
#include "vector.h"

// L2 quadrada entre a e b. Cabe folgado em int64 mesmo no pior caso teorico.
int64_t rnh_dist_l2sq(const rnh_qvec_t a, const rnh_qvec_t b);

// Limite inferior da distancia entre o vetor q e qualquer ponto contido na
// bounding box [mn, mx]. Usado para podar buckets inteiros sem visitar.
int64_t rnh_dist_lb_bbox(const rnh_qvec_t q,
                         const rnh_qvec_t mn,
                         const rnh_qvec_t mx);

#endif
