# Makefile do candidato. Tudo C11, build estatico-ish (libc + libz), AVX2.
# Targets:
#   make           -> bin/score bin/edge bin/forge
#   make forge     -> apenas o builder de indice
#   make index     -> roda forge sobre resources/references.json.gz -> resources/index.bin
#   make clean     -> apaga bin/ e obj/

CC      ?= gcc
CFLAGS  ?= -std=c11 -O3 -flto -march=haswell -mavx2 -mfma \
           -Wall -Wextra -Wshadow -Wpointer-arith -Wno-unused-parameter \
           -D_GNU_SOURCE -fno-plt -fvisibility=hidden
LDFLAGS ?= -flto -Wl,--gc-sections
LIBS    ?= -lz

SRC_CORE      := src/core/vector.c src/core/distance.c
SRC_INDEX     := src/index/reader.c src/index/builder.c
SRC_TRANSPORT := src/transport/net.c src/transport/http.c
SRC_SCORE     := src/score/service.c src/score/server.c src/score/score_main.c
SRC_EDGE      := src/edge/proxy.c src/edge/edge_main.c
SRC_FORGE     := src/index/forge_main.c

OBJDIR := obj
BINDIR := bin

OBJS_CORE      := $(SRC_CORE:src/%.c=$(OBJDIR)/%.o)
OBJS_INDEX     := $(SRC_INDEX:src/%.c=$(OBJDIR)/%.o)
OBJS_TRANSPORT := $(SRC_TRANSPORT:src/%.c=$(OBJDIR)/%.o)
OBJS_SCORE     := $(SRC_SCORE:src/%.c=$(OBJDIR)/%.o)
OBJS_EDGE      := $(SRC_EDGE:src/%.c=$(OBJDIR)/%.o)
OBJS_FORGE     := $(SRC_FORGE:src/%.c=$(OBJDIR)/%.o)

.PHONY: all clean index forge score edge dirs

all: score edge forge

dirs:
	@mkdir -p $(BINDIR) $(OBJDIR)/core $(OBJDIR)/index $(OBJDIR)/transport \
	    $(OBJDIR)/score $(OBJDIR)/edge

$(OBJDIR)/%.o: src/%.c | dirs
	$(CC) $(CFLAGS) -c $< -o $@

score: $(BINDIR)/score
edge:  $(BINDIR)/edge
forge: $(BINDIR)/forge

$(BINDIR)/score: $(OBJS_CORE) $(OBJS_INDEX) $(OBJS_TRANSPORT) $(OBJS_SCORE) | dirs
	$(CC) $(LDFLAGS) -o $@ $^

$(BINDIR)/edge: $(OBJS_TRANSPORT) $(OBJS_EDGE) | dirs
	$(CC) $(LDFLAGS) -o $@ $^

$(BINDIR)/forge: $(OBJS_CORE) $(OBJS_INDEX) $(OBJS_FORGE) | dirs
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# Conveniencia: gera o indice usando os dados em resources/.
index: $(BINDIR)/forge
	$(BINDIR)/forge resources/references.json.gz resources/index.bin

clean:
	rm -rf $(BINDIR) $(OBJDIR)
