#ifndef PRISM_PAGING_H
#define PRISM_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#define PAGING_MAX_LEVELS     4
#define PAGING_MAX_ADDRESSES  64
#define PAGING_MAX_TLB        16
#define PAGING_MIN_ADDR_BITS  8
#define PAGING_MAX_ADDR_BITS  48
#define PAGING_MAX_PHYS_BITS  52
#define PAGING_MIN_FRAME_BITS 6

typedef struct {
    int address_bits;
    int offset_bits;
    int levels;
    int level_bits[PAGING_MAX_LEVELS];
    int pte_bytes;
    int physical_bits;
    int tlb_entries;
} paging_config_t;

typedef struct {
    uint64_t page_size;
    uint64_t num_pages;
    uint64_t num_frames;
    uint64_t entries[PAGING_MAX_LEVELS];
    uint64_t table_bytes[PAGING_MAX_LEVELS];
    uint64_t tables_when_full[PAGING_MAX_LEVELS];
    uint64_t flat_table_bytes;
    uint64_t full_tree_bytes;
} paging_sizes_t;

typedef struct {
    uint64_t vaddr;
    uint64_t vpn;
    uint64_t offset;
    uint64_t index[PAGING_MAX_LEVELS];
    int      table_id[PAGING_MAX_LEVELS];
    bool     table_created[PAGING_MAX_LEVELS];
    bool     page_created;
    bool     tlb_hit;
    int      tlb_victim_slot;
    uint64_t tlb_victim_vpn;
    uint64_t frame;
    uint64_t paddr;
    int      memory_accesses;
} paging_step_t;

typedef struct {
    paging_config_t config;
    paging_sizes_t  sizes;
    int             num_steps;
    paging_step_t   steps[PAGING_MAX_ADDRESSES];
    int             tlb_hits;
    int             pages_mapped;
    int             tables_per_level[PAGING_MAX_LEVELS];
    uint64_t        table_bytes_used;
    int             memory_accesses;
} paging_result_t;

typedef enum {
    PAGING_OK = 0,
    PAGING_ERR_ADDRESS_BITS,
    PAGING_ERR_OFFSET_BITS,
    PAGING_ERR_LEVELS,
    PAGING_ERR_SPLIT,
    PAGING_ERR_PTE,
    PAGING_ERR_PHYSICAL,
    PAGING_ERR_TLB,
    PAGING_ERR_EMPTY,
    PAGING_ERR_TOO_MANY,
    PAGING_ERR_SYNTAX,
    PAGING_ERR_RANGE,
    PAGING_ERR_MEMORY
} paging_status_t;

const char     *paging_strerror(paging_status_t status);
paging_status_t paging_validate(const paging_config_t *config);
void            paging_split_evenly(paging_config_t *config);
void            paging_compute_sizes(const paging_config_t *config, paging_sizes_t *sizes);
void            paging_split(const paging_config_t *config, uint64_t vaddr, uint64_t index[PAGING_MAX_LEVELS],
                             uint64_t *offset);

uint64_t paging_pages_needed(const paging_config_t *config, uint64_t process_bytes);
uint64_t paging_internal_fragmentation(const paging_config_t *config, uint64_t process_bytes);
double   paging_effective_access_time(int levels, double tlb_hit_ratio, double tlb_ns, double memory_ns);

paging_status_t paging_parse_addresses(const char *text, const paging_config_t *config, uint64_t *addresses,
                                       int max_addresses, int *count, char *bad_token, int bad_size);
paging_status_t paging_translate_all(const paging_config_t *config, const uint64_t *addresses, int count,
                                     paging_result_t *out);

#endif
