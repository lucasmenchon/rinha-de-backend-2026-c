# Build context: C:\Users\Lucas\source\Bots\rinha  (rinha root)
# Usage: docker build -f candidate/fast-lb-api/Dockerfile .
#
# Combines:
#   lb  — MXLange-derived TCP proxy (modified: TCP_QUICKACK, UPSTREAMS env, SEQPACKET)
#   api — jonathanperis IVF search worker (patched: memmem header scan, prefetch)

FROM alpine:3.20 AS build

RUN apk add --no-cache build-base zlib-dev

WORKDIR /src

# jonathanperis api + common source
COPY refs/jonathanperis-rinha4-back-end-c/src/api/     src/api/
COPY refs/jonathanperis-rinha4-back-end-c/src/common/  src/common/
COPY refs/jonathanperis-rinha4-back-end-c/src/preprocess/ src/preprocess/

# modified lb source
COPY candidate/fast-lb-api/src/lb.c src_lb/lb.c

# combined Makefile
COPY candidate/fast-lb-api/Makefile Makefile

# apply source patches (memmem + prefetch)
COPY candidate/fast-lb-api/patch.sh patch.sh
RUN sh patch.sh

# index reference data (local copy avoids downloading during build)
COPY candidate/resources/references.json.gz resources/references.json.gz

# build lb + api + build-index
RUN make -j$(nproc) all \
    CFLAGS_ARCH="-march=haswell -mtune=haswell -mavx2 -mfma -flto \
                 -fomit-frame-pointer -fno-plt -fno-semantic-interposition \
                 -fno-trapping-math -DRINHA_ASSUME_PASSED_FD_FLAGS"

# generate IVF/block8 index. Reference fraud fastpaths are intentionally
# aggressive: only buckets that are pure fraud in references are materialized.
RUN BUCKET_REFERENCE_FASTPATH1_FRAUD_MIN_COUNT=1 \
    BUCKET_REFERENCE_FASTPATH2_FRAUD_MIN_COUNT=1 \
    ./build/build-index resources/references.json.gz resources/index.bin 4096

RUN strip build/api build/lb

# ── runtime image ──────────────────────────────────────────────────────────
FROM alpine:3.20

RUN adduser -D -H rinha && mkdir -p /app/resources

WORKDIR /app
COPY --from=build /src/build/api          /app/api
COPY --from=build /src/build/lb           /app/lb
COPY --from=build /src/resources/index.bin /app/resources/index.bin

USER rinha
CMD ["/app/api"]
