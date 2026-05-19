CC ?= cc
BUILD_DIR := build

# jonathanperis common + api sources
COMMON_SRC := src/common/distance.c src/common/fdpass.c src/common/http.c \
              src/common/index.c src/common/net.c src/common/search.c src/common/vectorize.c

CFLAGS_WARN  := -Wall -Wextra -Wshadow -Werror -I src
CFLAGS_ARCH  ?=
CFLAGS_COMMON := -std=c11 -O3 -DNDEBUG $(CFLAGS_ARCH) $(CFLAGS_WARN)

.PHONY: all api lb preprocess

all: api lb preprocess

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# jonathanperis api binary (epoll, IVF search, fd-pass receiver)
$(BUILD_DIR)/api: src/api/main.c $(COMMON_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS_COMMON) $^ -o $@

# MXLange-derived lb binary (TCP accept, round-robin fd-pass sender)
$(BUILD_DIR)/lb: src_lb/lb.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_COMMON) $^ -o $@ -pthread

# Index builder (offline tool, not shipped in final image)
$(BUILD_DIR)/build-index: src/preprocess/build_index.c src/common/distance.c | $(BUILD_DIR)
	$(CC) $(CFLAGS_COMMON) $^ -o $@ -lz

api:      $(BUILD_DIR)/api
lb:       $(BUILD_DIR)/lb
preprocess: $(BUILD_DIR)/build-index
