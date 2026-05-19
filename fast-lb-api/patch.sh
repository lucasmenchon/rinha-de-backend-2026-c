#!/bin/sh
# Applies two targeted patches to jonathanperis source copied into /src:
#   1. api/main.c: replace byte-by-byte find_header_end with memmem
#   2. common/search.c: replace scan_unscanned_lists with sorted+bitmask version
#      - bitmask replaces O(scanned_count) list_was_scanned linear scan
#      - collects candidates sorted by list_lower_bound (ascending)
#      - scans in lb order with early termination when lb >= current worst_dist
set -e

API_MAIN="src/api/main.c"
SEARCH_SRC="src/common/search.c"

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
/^static size_t find_header_end/ {
    print "static size_t find_header_end(const char *data, size_t len) {"
    print "    if (len < 4) return (size_t)-1;"
    print "    const void *p = memmem(data, len, \"\\r\\n\\r\\n\", 4);"
    print "    if (p == NULL) return (size_t)-1;"
    print "    return (size_t)((const char *)p - data);"
    print "}"
    skip=1
    depth=1
    next
}
skip {
    depth += gsub(/\{/, "{")
    depth -= gsub(/\}/, "}")
    if (depth == 0) skip=0
    next
}
{ print }
' "$API_MAIN" > "$API_MAIN.tmp" && mv "$API_MAIN.tmp" "$API_MAIN"

echo "Patch 1 applied: memmem in find_header_end"

# ---------------------------------------------------------------------------
# Patch 1b: api/main.c — drain client fd until EAGAIN.
#
# Requests are small, and the LB hands the accepted fd to the API after the
# client has already started writing. Draining available bytes avoids an extra
# epoll turn when header/body arrive split across reads.
# ---------------------------------------------------------------------------
awk '
/^static int read_conn\(/ { skip=1; depth=0 }
skip && /\{/ { depth++ }
skip && /\}/ {
    depth--
    if (depth == 0) {
        print "static int read_conn(conn_t *conn, const rinha_index_t *index, int close_after_response) {"
        print "    for (;;) {"
        print "        if (conn->have == sizeof(conn->buf)) return 0;"
        print "        ssize_t n = read(conn->fd, conn->buf + conn->have, sizeof(conn->buf) - conn->have);"
        print "        if (n > 0) {"
        print "            conn->have += (size_t)n;"
        print "            if (!process_buffer(conn, index, close_after_response)) return 0;"
        print "            continue;"
        print "        }"
        print "        if (n == 0) return 0;"
        print "        if (errno == EINTR) continue;"
        print "        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;"
        print "        return 0;"
        print "    }"
        print "}"
        skip=0
        next
    }
}
skip { next }
{ print }
' "$API_MAIN" > "$API_MAIN.tmp" && mv "$API_MAIN.tmp" "$API_MAIN"

echo "Patch 1b applied: drain read_conn until EAGAIN"

# ---------------------------------------------------------------------------
# Patch 2: common/search.c — replace scan_unscanned_lists
#
# Problem: the original function calls list_was_scanned() for every list,
# which is an O(scanned_count=24) linear scan → ~97K comparisons wasted.
# It also scans unscanned lists in list-id order (arbitrary), not by
# proximity, so each scan barely tightens worst_dist before moving to
# the next list.
#
# Fix: 
#   a) Build a 4096-bit bitmask from probes[] → O(1) scanned check
#   b) Collect candidate lists with their list_lower_bound values
#   c) Sort candidates by lb ascending (qsort, ~400 items ≈ 6µs)
#   d) Scan in order; after each scan worst_dist tightens → early break
#
# Result: exact_fallback scans ~5-50 lists instead of ~400, saving ~1ms
# for the slowest 1% of requests (p99 target: ≤1ms).
# ---------------------------------------------------------------------------
# list_was_scanned becomes dead code after the bitmask replacement; remove it
# so the build stays clean with -Werror.
awk '
/^static int list_was_scanned\(/ { skip=1; depth=1; next }
skip {
    depth += gsub(/\{/, "{")
    depth -= gsub(/\}/, "}")
    if (depth == 0) skip=0
    next
}
{ print }
' "$SEARCH_SRC" > "$SEARCH_SRC.tmp" && mv "$SEARCH_SRC.tmp" "$SEARCH_SRC"

awk '
/^static void scan_unscanned_lists\(/ { skip=1; depth=0 }
skip && /\{/ { depth++ }
skip && /\}/ {
    depth--
    if (depth == 0) {
        print "static int cmp_cand(const void *a, const void *b) {"
        print "    const uint64_t *ca = (const uint64_t *)a;"
        print "    const uint64_t *cb = (const uint64_t *)b;"
        print "    return (int)(ca[0] > cb[0]) - (int)(ca[0] < cb[0]);"
        print "}"
        print ""
        print "static void scan_unscanned_lists(const rinha_index_t *index, const int16_t query[RINHA_DIMS], const probe_t probes[RINHA_MAX_PROBES], uint32_t scanned_count, rinha_top5_t *top TRACE_PARAMS) {"
        print "    uint64_t scanned_mask[64] = {0};"
        print "    for (uint32_t i = 0; i < scanned_count; ++i)"
        print "        scanned_mask[probes[i].list >> 6U] |= (uint64_t)1U << (probes[i].list & 63U);"
        print "    uint64_t cands[4096][2];"
        print "    uint32_t ncands = 0;"
        print "    uint64_t worst = rinha_top5_worst_dist(top);"
        print "    for (uint32_t list = 0; list < index->list_count; ++list) {"
        print "        if (scanned_mask[list >> 6U] & ((uint64_t)1U << (list & 63U))) continue;"
        print "        uint64_t lb = 0;"
        print "        if (index->bounds_min != 0 && index->bounds_max != 0) {"
        print "            lb = list_lower_bound(index, query, list);"
        print "            if (lb >= worst) continue;"
        print "        }"
        print "        cands[ncands][0] = lb;"
        print "        cands[ncands][1] = (uint64_t)list;"
        print "        ++ncands;"
        print "    }"
        print "    qsort(cands, ncands, sizeof(cands[0]), cmp_cand);"
        print "    for (uint32_t i = 0; i < ncands; ++i) {"
        print "        if (cands[i][0] >= rinha_top5_worst_dist(top)) break;"
        print "        scan_list(index, query, (uint32_t)cands[i][1], top TRACE_ARGS(trace, phase));"
        print "    }"
        print "}"
        skip=0
        next
    }
}
skip { next }
{ print }
' "$SEARCH_SRC" > "$SEARCH_SRC.tmp" && mv "$SEARCH_SRC.tmp" "$SEARCH_SRC"

echo "Patch 2 applied: sorted scan_unscanned_lists with bitmask + early termination"
