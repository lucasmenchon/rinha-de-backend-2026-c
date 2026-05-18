// Leitor do indice RNH4. Mapeia o arquivo em memoria e expoe ponteiros
// para a tabela de listas, vetores e labels. Nenhuma copia.

#ifndef RNH_INDEX_READER_H
#define RNH_INDEX_READER_H

#include <stddef.h>
#include <stdint.h>

#include "format.h"

typedef struct {
    void                    *base;
    size_t                   size;
    const rnh_index_header_t *hdr;
    const rnh_list_slot_t    *lists;
    const int16_t            *vectors;  // [nvectors][16]
    const uint8_t            *labels;   // [nvectors]
} rnh_index_t;

// Abre, valida magic/version/crc, faz mmap RO populando paginas.
// Retorna 0 em sucesso, -1 com errno setado caso contrario.
int  rnh_index_open(const char *path, rnh_index_t *out);
void rnh_index_close(rnh_index_t *idx);

#endif
