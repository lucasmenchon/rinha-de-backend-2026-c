// Formato em disco do indice RNH4.
//
// Tudo little-endian, alinhamentos pensados para mmap direto sem copia.
// Layout:
//   [HEADER 64 bytes]
//   [LISTS TABLE  nlists * RNH_LIST_SLOT_SIZE bytes, ja alinhado a 32]
//   [VECTORS BLOB nvectors * 32 bytes, alinhado a 32 (16 int16 por vetor)]
//   [LABELS BLOB  nvectors * 1 byte]
//
// O builder grava os vetores ja agrupados por lista, contiguos. Cada slot da
// lists table contem o offset (em numero de vetores) onde aquela lista comeca,
// quantos vetores ela tem, e seu centroide quantizado.

#ifndef RNH_INDEX_FORMAT_H
#define RNH_INDEX_FORMAT_H

#include <stdint.h>

#include "../core/vector.h"

#define RNH_INDEX_MAGIC      "RNH4"
#define RNH_INDEX_VERSION    1u
#define RNH_HEADER_SIZE      64u
#define RNH_LIST_SLOT_SIZE   64u

// Numero default de listas IVF. Ajustavel em build-time via -DRNH_NLISTS.
#ifndef RNH_NLISTS
#define RNH_NLISTS 256u
#endif

// Numero de listas mais proximas a visitar por consulta. Ajustavel.
#ifndef RNH_NPROBE
#define RNH_NPROBE 12u
#endif

// Header (64 bytes total). Campos LE.
typedef struct __attribute__((packed)) {
    char     magic[4];       // "RNH4"
    uint32_t version;        // RNH_INDEX_VERSION
    uint32_t nlists;
    uint32_t nvectors;
    uint64_t lists_off;
    uint64_t vecs_off;
    uint64_t labels_off;
    uint64_t file_size;
    uint32_t header_crc32;   // CRC32C dos campos acima zerando este campo
    uint8_t  reserved[12];
} rnh_index_header_t;

_Static_assert(sizeof(rnh_index_header_t) == RNH_HEADER_SIZE,
               "header deve ter exatamente 64 bytes");

// Slot de uma lista (64 bytes). Centroide em primeiro lugar para alinhamento
// SIMD natural.
typedef struct __attribute__((aligned(32))) {
    rnh_qvec_t centroid;     // 32 bytes
    uint32_t   vec_offset;   // indice do primeiro vetor da lista no blob
    uint32_t   vec_count;
    uint32_t   fraud_count;  // numero de labels=1 (telemetria + sanity)
    uint32_t   reserved0;
    uint8_t    reserved1[16];
} rnh_list_slot_t;

_Static_assert(sizeof(rnh_list_slot_t) == RNH_LIST_SLOT_SIZE,
               "slot deve ter exatamente 64 bytes");

#endif
