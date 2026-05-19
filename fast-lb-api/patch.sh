#!/bin/sh
# Applies two targeted patches to jonathanperis source copied into /src:
#   1. api/main.c: replace byte-by-byte find_header_end with memmem
#   2. common/search.c: add __builtin_prefetch in scan_list_block16 inner loop
set -e

# ---------------------------------------------------------------------------
# Patch 1: api/main.c — memmem for \r\n\r\n header scan
# Replace the entire find_header_end function body.
# The original function:
#   static size_t find_header_end(const char *data, size_t len) {
#       if (len < 4) return (size_t)-1;
#       for (size_t i = 0; i <= len - 4; ++i) { ... }
#       return (size_t)-1;
#   }
# ---------------------------------------------------------------------------
awk '
/^static size_t find_header_end/ { skip=1; next }
skip && /^}$/ {
    print "static size_t find_header_end(const char *data, size_t len) {"
    print "    if (len < 4) return (size_t)-1;"
    print "    const void *p = memmem(data, len, \"\\r\\n\\r\\n\", 4);"
    print "    if (p == NULL) return (size_t)-1;"
    print "    return (size_t)((const char *)p - data);"
    print "}"
    skip=0
    next
}
skip { next }
{ print }
' api/main.c > api/main.c.tmp && mv api/main.c.tmp api/main.c

echo "Patch 1 applied: memmem in find_header_end"

# ---------------------------------------------------------------------------
# Patch 2: common/search.c — prefetch next block in scan_list_block16
# Inserts two lines BEFORE the unique line that reads the block pointer:
#   const int16_t *block = index->vectors + (size_t)block_index * RINHA_DIMS * 16U;
# This pattern only appears in scan_list_block16 (block8 uses slot*RINHA_DIMS).
# ---------------------------------------------------------------------------
awk '
/        const int16_t \*block = index->vectors \+ \(size_t\)block_index \* RINHA_DIMS \* 16U;/ {
    print "        if (block_index + 1U < end_block)"
    print "            __builtin_prefetch(index->vectors + (size_t)(block_index + 1U) * RINHA_DIMS * 16U, 0, 1);"
}
{ print }
' common/search.c > common/search.c.tmp && mv common/search.c.tmp common/search.c

echo "Patch 2 applied: prefetch in scan_list_block16"
