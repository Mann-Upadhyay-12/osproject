#include "prism/paging.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TABLES        (1 + (PAGING_MAX_LEVELS - 1) * PAGING_MAX_ADDRESSES)
#define FRAME_SCATTER     37u
#define FRAME_FIRST       5u
#define MIN_PTE_BYTES     1
#define MAX_PTE_BYTES     16
#define NOT_FOUND         (-1)

typedef struct {
    int      num_children;
    uint64_t index[PAGING_MAX_ADDRESSES];
    uint64_t child[PAGING_MAX_ADDRESSES];
} table_node_t;

typedef struct {
    uint64_t vpn;
    uint64_t frame;
    int      last_used;
} tlb_entry_t;

typedef struct {
    const paging_config_t *config;
    table_node_t           tables[MAX_TABLES];
    int                    num_tables;
    int                    pages_mapped;
    tlb_entry_t            tlb[PAGING_MAX_TLB];
    int                    tlb_count;
    int                    clock;
} walker_t;

const char *paging_strerror(paging_status_t status) {
    switch (status) {
        case PAGING_OK:               return "ok";
        case PAGING_ERR_ADDRESS_BITS: return "virtual address bits must be 8..48";
        case PAGING_ERR_OFFSET_BITS:  return "page size must leave at least one bit per level";
        case PAGING_ERR_LEVELS:       return "levels must be 1..4";
        case PAGING_ERR_SPLIT:        return "level bits must each be >= 1 and add up to the page-number bits";
        case PAGING_ERR_PTE:          return "page table entry size must be 1..16 bytes";
        case PAGING_ERR_PHYSICAL:     return "physical address bits must be page bits + 6 .. 52";
        case PAGING_ERR_TLB:          return "TLB entries must be 0..16";
        case PAGING_ERR_EMPTY:        return "enter at least one virtual address";
        case PAGING_ERR_TOO_MANY:     return "too many addresses (max 64)";
        case PAGING_ERR_SYNTAX:       return "addresses are decimal or 0x hex";
        case PAGING_ERR_RANGE:        return "address does not fit in the virtual address space";
        case PAGING_ERR_MEMORY:       return "out of memory";
    }
    return "unknown error";
}

static int page_number_bits(const paging_config_t *config) {
    return config->address_bits - config->offset_bits;
}

paging_status_t paging_validate(const paging_config_t *c) {
    if (c->address_bits < PAGING_MIN_ADDR_BITS || c->address_bits > PAGING_MAX_ADDR_BITS) return PAGING_ERR_ADDRESS_BITS;
    if (c->levels < 1 || c->levels > PAGING_MAX_LEVELS) return PAGING_ERR_LEVELS;
    if (c->offset_bits < 1 || page_number_bits(c) < c->levels) return PAGING_ERR_OFFSET_BITS;

    int sum = 0;
    for (int l = 0; l < c->levels; l++) {
        if (c->level_bits[l] < 1) return PAGING_ERR_SPLIT;
        sum += c->level_bits[l];
    }
    if (sum != page_number_bits(c)) return PAGING_ERR_SPLIT;

    if (c->pte_bytes < MIN_PTE_BYTES || c->pte_bytes > MAX_PTE_BYTES) return PAGING_ERR_PTE;
    if (c->physical_bits > PAGING_MAX_PHYS_BITS || c->physical_bits - c->offset_bits < PAGING_MIN_FRAME_BITS) {
        return PAGING_ERR_PHYSICAL;
    }
    if (c->tlb_entries < 0 || c->tlb_entries > PAGING_MAX_TLB) return PAGING_ERR_TLB;
    return PAGING_OK;
}

void paging_split_evenly(paging_config_t *config) {
    int levels = config->levels < 1 ? 1 : config->levels > PAGING_MAX_LEVELS ? PAGING_MAX_LEVELS : config->levels;
    int bits = page_number_bits(config);
    if (bits < levels) return;

    int each = bits / levels, extra = bits % levels;
    for (int l = 0; l < PAGING_MAX_LEVELS; l++) {
        config->level_bits[l] = l < levels ? each + (l < extra ? 1 : 0) : 0;
    }
}

void paging_compute_sizes(const paging_config_t *c, paging_sizes_t *s) {
    memset(s, 0, sizeof(*s));
    s->page_size = 1ull << c->offset_bits;
    s->num_pages = 1ull << page_number_bits(c);
    s->num_frames = 1ull << (c->physical_bits - c->offset_bits);
    s->flat_table_bytes = s->num_pages * (uint64_t)c->pte_bytes;

    uint64_t tables = 1;
    for (int l = 0; l < c->levels; l++) {
        s->entries[l] = 1ull << c->level_bits[l];
        s->table_bytes[l] = s->entries[l] * (uint64_t)c->pte_bytes;
        s->tables_when_full[l] = tables;
        s->full_tree_bytes += tables * s->table_bytes[l];
        tables *= s->entries[l];
    }
}

void paging_split(const paging_config_t *c, uint64_t vaddr, uint64_t index[PAGING_MAX_LEVELS], uint64_t *offset) {
    *offset = vaddr & ((1ull << c->offset_bits) - 1);
    int shift = c->address_bits;
    for (int l = 0; l < PAGING_MAX_LEVELS; l++) {
        if (l >= c->levels) {
            index[l] = 0;
            continue;
        }
        shift -= c->level_bits[l];
        index[l] = (vaddr >> shift) & ((1ull << c->level_bits[l]) - 1);
    }
}

uint64_t paging_pages_needed(const paging_config_t *c, uint64_t process_bytes) {
    uint64_t page = 1ull << c->offset_bits;
    return (process_bytes + page - 1) / page;
}

uint64_t paging_internal_fragmentation(const paging_config_t *c, uint64_t process_bytes) {
    uint64_t page = 1ull << c->offset_bits;
    return paging_pages_needed(c, process_bytes) * page - process_bytes;
}

double paging_effective_access_time(int levels, double tlb_hit_ratio, double tlb_ns, double memory_ns) {
    return tlb_ns + memory_ns + (1.0 - tlb_hit_ratio) * levels * memory_ns;
}

static const char *next_token(const char *p, char *token, size_t size) {
    while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
    size_t length = 0;
    while (p[length] && !isspace((unsigned char)p[length]) && p[length] != ',' && p[length] != ';') length++;
    snprintf(token, size, "%.*s", (int)(length < size ? length : size - 1), p);
    return p + length;
}

static paging_status_t parse_address(const char *token, const paging_config_t *c, uint64_t *out) {
    if (token[0] == '-' || token[0] == '+') return PAGING_ERR_SYNTAX;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(token, &end, 0);
    if (*end != '\0' || end == token) return PAGING_ERR_SYNTAX;
    if (errno == ERANGE || value >> c->address_bits) return PAGING_ERR_RANGE;
    *out = value;
    return PAGING_OK;
}

paging_status_t paging_parse_addresses(const char *text, const paging_config_t *config, uint64_t *addresses,
                                       int max_addresses, int *count, char *bad_token, int bad_size) {
    *count = 0;
    if (bad_token && bad_size > 0) bad_token[0] = '\0';

    char token[64];
    for (const char *p = next_token(text, token, sizeof(token)); token[0]; p = next_token(p, token, sizeof(token))) {
        if (*count >= max_addresses) return PAGING_ERR_TOO_MANY;
        paging_status_t status = parse_address(token, config, &addresses[*count]);
        if (status != PAGING_OK) {
            if (bad_token) snprintf(bad_token, (size_t)bad_size, "%s", token);
            return status;
        }
        (*count)++;
    }
    return *count > 0 ? PAGING_OK : PAGING_ERR_EMPTY;
}

static int find_child(const table_node_t *table, uint64_t index) {
    for (int i = 0; i < table->num_children; i++) {
        if (table->index[i] == index) return i;
    }
    return NOT_FOUND;
}

static uint64_t scattered_frame(const walker_t *w, int nth_page) {
    uint64_t num_frames = 1ull << (w->config->physical_bits - w->config->offset_bits);
    return ((uint64_t)nth_page * FRAME_SCATTER + FRAME_FIRST) & (num_frames - 1);
}

static uint64_t create_child(walker_t *w, int level, paging_step_t *step) {
    bool last_level = level == w->config->levels - 1;
    if (last_level) {
        step->page_created = true;
        return scattered_frame(w, w->pages_mapped++);
    }
    step->table_created[level + 1] = true;
    return (uint64_t)w->num_tables++;
}

static uint64_t walk_tables(walker_t *w, paging_step_t *step) {
    int table = 0;
    uint64_t entry = 0;
    for (int l = 0; l < w->config->levels; l++) {
        step->table_id[l] = table;
        table_node_t *node = &w->tables[table];
        int slot = find_child(node, step->index[l]);
        if (slot == NOT_FOUND) {
            slot = node->num_children++;
            node->index[slot] = step->index[l];
            node->child[slot] = create_child(w, l, step);
        }
        entry = node->child[slot];
        table = (int)entry;
    }
    return entry;
}

static int tlb_find(const walker_t *w, uint64_t vpn) {
    for (int i = 0; i < w->tlb_count; i++) {
        if (w->tlb[i].vpn == vpn) return i;
    }
    return NOT_FOUND;
}

static void tlb_insert(walker_t *w, uint64_t vpn, uint64_t frame, paging_step_t *step) {
    int slot;
    if (w->tlb_count < w->config->tlb_entries) {
        slot = w->tlb_count++;
    } else {
        slot = 0;
        for (int i = 1; i < w->tlb_count; i++) {
            if (w->tlb[i].last_used < w->tlb[slot].last_used) slot = i;
        }
        step->tlb_victim_slot = slot;
        step->tlb_victim_vpn = w->tlb[slot].vpn;
    }
    w->tlb[slot] = (tlb_entry_t){vpn, frame, w->clock};
}

static void translate(walker_t *w, uint64_t vaddr, paging_step_t *step) {
    const paging_config_t *c = w->config;
    memset(step, 0, sizeof(*step));
    step->vaddr = vaddr;
    step->vpn = vaddr >> c->offset_bits;
    step->tlb_victim_slot = NOT_FOUND;
    paging_split(c, vaddr, step->index, &step->offset);
    w->clock++;

    int hit = c->tlb_entries > 0 ? tlb_find(w, step->vpn) : NOT_FOUND;
    if (hit != NOT_FOUND) {
        step->tlb_hit = true;
        step->frame = w->tlb[hit].frame;
        w->tlb[hit].last_used = w->clock;
        step->memory_accesses = 1;
    } else {
        step->frame = walk_tables(w, step);
        if (c->tlb_entries > 0) tlb_insert(w, step->vpn, step->frame, step);
        step->memory_accesses = c->levels + 1;
    }
    step->paddr = (step->frame << c->offset_bits) | step->offset;
}

static void count_tables(const walker_t *w, paging_result_t *out) {
    out->tables_per_level[0] = 1;
    for (int i = 0; i < out->num_steps; i++) {
        for (int l = 1; l < w->config->levels; l++) out->tables_per_level[l] += out->steps[i].table_created[l];
    }
    for (int l = 0; l < w->config->levels; l++) {
        out->table_bytes_used += (uint64_t)out->tables_per_level[l] * out->sizes.table_bytes[l];
    }
}

paging_status_t paging_translate_all(const paging_config_t *config, const uint64_t *addresses, int count,
                                     paging_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->config = *config;
    paging_status_t status = paging_validate(config);
    if (status != PAGING_OK) return status;
    if (count <= 0) return PAGING_ERR_EMPTY;
    if (count > PAGING_MAX_ADDRESSES) return PAGING_ERR_TOO_MANY;
    for (int i = 0; i < count; i++) {
        if (addresses[i] >> config->address_bits) return PAGING_ERR_RANGE;
    }

    walker_t *w = calloc(1, sizeof(walker_t));
    if (!w) return PAGING_ERR_MEMORY;
    w->config = config;
    w->num_tables = 1;

    paging_compute_sizes(config, &out->sizes);
    for (int i = 0; i < count; i++) {
        paging_step_t *step = &out->steps[out->num_steps++];
        translate(w, addresses[i], step);
        out->tlb_hits += step->tlb_hit;
        out->memory_accesses += step->memory_accesses;
    }
    out->pages_mapped = w->pages_mapped;
    count_tables(w, out);
    free(w);
    return PAGING_OK;
}
