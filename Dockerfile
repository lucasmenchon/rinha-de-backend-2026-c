# Build em duas etapas. A primeira compila tudo e ja gera o indice (RNH4)
# a partir do dump de referencias. A segunda copia apenas os binarios + indice.
#
# A imagem final nao tem compilador, nem libz dinamica de mais nada alem do
# minimo necessario. Pesa < 15 MB.

FROM alpine:3.20 AS builder
RUN apk add --no-cache build-base zlib-dev

WORKDIR /src
COPY Makefile ./
COPY src/   src/
COPY resources/references.json.gz resources/references.json.gz

RUN make -j$(nproc) all
RUN mkdir -p /out && bin/forge resources/references.json.gz /out/index.bin

# Em alpine, bin/score e bin/edge sao dinamicamente linkados contra musl.
# Mantemos imagem final tambem em alpine para portabilidade do binario.
FROM alpine:3.20
RUN apk add --no-cache libgcc
RUN mkdir -p /app /run/rnh && chown 1000:1000 /run/rnh
WORKDIR /app
COPY --from=builder /src/bin/score /app/score
COPY --from=builder /src/bin/edge  /app/edge
COPY --from=builder /out/index.bin /app/index.bin

ENV RNH_INDEX=/app/index.bin
EXPOSE 9999
USER 1000:1000
# Default cmd e o score; o servico edge sobrescreve no compose.
CMD ["/app/score"]
