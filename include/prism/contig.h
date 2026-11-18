#ifndef PRISM_CONTIG_H
#define PRISM_CONTIG_H

#include <stdbool.h>
#include <stdint.h>

#define CONTIG_MAX_BLOCKS  64
#define CONTIG_MAX_OPS     64
#define CONTIG_NAME_LEN    12
#define CONTIG_MAX_MEMORY  1000000
#define CONTIG_RESERVED    '#'

typedef enum {
    CONTIG_FIRST_FIT = 0,
    CONTIG_BEST_FIT,
    CONTIG_WORST_FIT,
    CONTIG_NEXT_FIT,
    CONTIG_COUNT
} contig_policy_t;

typedef enum {
    CONTIG_OP_ALLOC = 0,
    CONTIG_OP_FREE,
    CONTIG_OP_COMPACT
} contig_op_kind_t;

typedef struct {
    contig_op_kind_t kind;
    char             name[CONTIG_NAME_LEN];
    int              size;
} contig_op_t;

typedef struct {
    int  start;
    int  size;
    bool used;
    char name[CONTIG_NAME_LEN];
} contig_block_t;

typedef struct {
    int            size;
    int            num_blocks;
    contig_block_t blocks[CONTIG_MAX_BLOCKS];
} contig_memory_t;

typedef struct {
    int    free_total;
    int    largest_hole;
    int    num_holes;
    double external_fragmentation;
} contig_stats_t;

typedef struct {
    contig_op_t     op;
    bool            ok;
    int             address;
    int             hole_size;
    int             moved;
    contig_memory_t memory;
    contig_stats_t  stats;
} contig_step_t;

typedef struct {
    contig_policy_t policy;
    int             num_steps;
    int             failed;
    int             compacted_bytes;
    contig_step_t   steps[CONTIG_MAX_OPS];
} contig_result_t;

typedef struct contig_ops {
    contig_policy_t policy;
    const char     *name;
    const char     *key;
    int           (*pick_hole)(const contig_memory_t *mem, int size, int rover);
} contig_ops_t;

typedef enum {
    CONTIG_OK = 0,
    CONTIG_ERR_EMPTY,
    CONTIG_ERR_TOO_MANY,
    CONTIG_ERR_SYNTAX,
    CONTIG_ERR_SIZE,
    CONTIG_ERR_NAME,
    CONTIG_ERR_MEMORY_SIZE
} contig_status_t;

const contig_ops_t *contig_get_ops(contig_policy_t policy);
bool                contig_lookup(const char *key, contig_policy_t *out);
const char         *contig_strerror(contig_status_t status);

contig_status_t contig_parse_script(const char *text, contig_op_t *ops, int max_ops, int *count, char *bad_token,
                                    int bad_size);
contig_status_t contig_run(contig_policy_t policy, int memory_size, const contig_op_t *ops, int count,
                           contig_result_t *out);

void contig_init(contig_memory_t *mem, int size);
void contig_compute_stats(const contig_memory_t *mem, contig_stats_t *stats);
bool contig_is_reserved(const char *name);

#endif
