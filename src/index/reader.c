#include "reader.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Polinomio CRC32C (Castagnoli). Implementacao escalar simples; o calculo
// roda apenas na abertura, portanto nao precisa de hardware crc32 instruction.
static uint32_t crc32c_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            uint32_t mask = (uint32_t)-(int32_t)(c & 1u);
            c = (c >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return ~c;
}

// CRC do header com header_crc32 zerado.
static uint32_t header_crc(const rnh_index_header_t *h) {
    rnh_index_header_t copy = *h;
    copy.header_crc32 = 0;
    return crc32c_buf((const uint8_t *)&copy, sizeof(copy));
}

int rnh_index_open(const char *path, rnh_index_t *out) {
    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) { int e = errno; close(fd); errno = e; return -1; }
    size_t sz = (size_t)st.st_size;
    if (sz < sizeof(rnh_index_header_t)) { close(fd); errno = EINVAL; return -1; }

    void *base = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return -1;

    // Pre-popula paginas e tenta huge pages (sem falhar se kernel recusar).
    (void)madvise(base, sz, MADV_WILLNEED);
#ifdef MADV_HUGEPAGE
    (void)madvise(base, sz, MADV_HUGEPAGE);
#endif
#ifdef MADV_POPULATE_READ
    (void)madvise(base, sz, MADV_POPULATE_READ);
#endif

    const rnh_index_header_t *h = (const rnh_index_header_t *)base;

    if (memcmp(h->magic, RNH_INDEX_MAGIC, 4) != 0) {
        munmap(base, sz); errno = EILSEQ; return -1;
    }
    if (h->version != RNH_INDEX_VERSION) {
        munmap(base, sz); errno = ENOTSUP; return -1;
    }
    if (h->file_size != sz) {
        munmap(base, sz); errno = EINVAL; return -1;
    }
    if (header_crc(h) != h->header_crc32) {
        munmap(base, sz); errno = EIO; return -1;
    }
    if (h->lists_off  + (uint64_t)h->nlists  * RNH_LIST_SLOT_SIZE > sz ||
        h->vecs_off   + (uint64_t)h->nvectors * 32u              > sz ||
        h->labels_off + (uint64_t)h->nvectors                    > sz) {
        munmap(base, sz); errno = ERANGE; return -1;
    }

    out->base    = base;
    out->size    = sz;
    out->hdr     = h;
    out->lists   = (const rnh_list_slot_t *)((const uint8_t *)base + h->lists_off);
    out->vectors = (const int16_t *)((const uint8_t *)base + h->vecs_off);
    out->labels  = (const uint8_t *)base + h->labels_off;
    return 0;
}

void rnh_index_close(rnh_index_t *idx) {
    if (idx->base) {
        munmap(idx->base, idx->size);
        idx->base = NULL;
    }
}
