// CLI do forge: builda o indice a partir de references.json.gz.
//
// Uso:
//   forge <input.json.gz> <output.idx> [nlists] [sample] [iters] [seed]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builder.h"
#include "format.h"

static uint32_t parse_u32(const char *s, uint32_t fallback) {
    if (!s || !*s) return fallback;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    return *end ? fallback : (uint32_t)v;
}

static uint64_t parse_u64(const char *s, uint64_t fallback) {
    if (!s || !*s) return fallback;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    return *end ? fallback : (uint64_t)v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.json.gz> <output.idx> [nlists] [sample] [iters] [seed]\n", argv[0]);
        return 2;
    }
    rnh_build_opts_t opts = {
        .gz_path       = argv[1],
        .out_path      = argv[2],
        .nlists        = parse_u32(argc > 3 ? argv[3] : NULL, RNH_NLISTS),
        .kmeans_sample = parse_u32(argc > 4 ? argv[4] : NULL, 200000u),
        .kmeans_iters  = parse_u32(argc > 5 ? argv[5] : NULL, 6u),
        .seed          = parse_u64(argc > 6 ? argv[6] : NULL, 0xC0FFEE1979BEEFULL),
    };
    if (rnh_build_run(&opts) != 0) {
        perror("[forge] failed");
        return 1;
    }
    fprintf(stderr, "[forge] ok\n");
    return 0;
}
