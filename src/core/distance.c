#include "distance.h"

#if defined(__AVX2__)
#include <immintrin.h>

int64_t rnh_dist_l2sq(const rnh_qvec_t a, const rnh_qvec_t b) {
    // 16 int16 por vetor cabem em um YMM. Lanes 14/15 sao zero nos dois lados,
    // entao contribuem 0 na soma e nao precisam de mascara.
    __m256i va = _mm256_loadu_si256((const __m256i *)a);
    __m256i vb = _mm256_loadu_si256((const __m256i *)b);
    __m256i d  = _mm256_sub_epi16(va, vb);
    // madd_epi16 multiplica pares e ja soma em int32 (8 lanes).
    __m256i sq = _mm256_madd_epi16(d, d);
    // Reducao horizontal das 8 lanes int32.
    __m128i lo = _mm256_castsi256_si128(sq);
    __m128i hi = _mm256_extracti128_si256(sq, 1);
    __m128i s  = _mm_add_epi32(lo, hi);
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    return (int64_t)(int32_t)_mm_cvtsi128_si32(s);
}

int64_t rnh_dist_lb_bbox(const rnh_qvec_t q, const rnh_qvec_t mn, const rnh_qvec_t mx) {
    __m256i vq = _mm256_loadu_si256((const __m256i *)q);
    __m256i vm = _mm256_loadu_si256((const __m256i *)mn);
    __m256i vM = _mm256_loadu_si256((const __m256i *)mx);
    // delta = max(mn - q, 0) + max(q - mx, 0)  (uma das parcelas e sempre 0)
    __m256i below = _mm256_max_epi16(_mm256_sub_epi16(vm, vq), _mm256_setzero_si256());
    __m256i above = _mm256_max_epi16(_mm256_sub_epi16(vq, vM), _mm256_setzero_si256());
    __m256i d     = _mm256_add_epi16(below, above);
    __m256i sq    = _mm256_madd_epi16(d, d);
    __m128i lo = _mm256_castsi256_si128(sq);
    __m128i hi = _mm256_extracti128_si256(sq, 1);
    __m128i s  = _mm_add_epi32(lo, hi);
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    return (int64_t)(int32_t)_mm_cvtsi128_si32(s);
}

#else  // fallback escalar para testes em maquinas sem AVX2

int64_t rnh_dist_l2sq(const rnh_qvec_t a, const rnh_qvec_t b) {
    int64_t s = 0;
    for (int i = 0; i < RNH_LANES; i++) {
        int32_t d = (int32_t)a[i] - (int32_t)b[i];
        s += (int64_t)d * d;
    }
    return s;
}

int64_t rnh_dist_lb_bbox(const rnh_qvec_t q, const rnh_qvec_t mn, const rnh_qvec_t mx) {
    int64_t s = 0;
    for (int i = 0; i < RNH_LANES; i++) {
        int32_t d = 0;
        if (q[i] < mn[i]) d = (int32_t)mn[i] - q[i];
        else if (q[i] > mx[i]) d = (int32_t)q[i] - mx[i];
        s += (int64_t)d * d;
    }
    return s;
}

#endif
