#include "prism/contig.h"
#include "prism/text.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NO_HOLE (-1)

bool contig_is_reserved(const char *name) {
    return name[0] == CONTIG_RESERVED;
}

void contig_init(contig_memory_t *mem, int size) {
    memset(mem, 0, sizeof(*mem));
    mem->size = size;
    mem->num_blocks = 1;
    mem->blocks[0] = (contig_block_t){0, size, false, ""};
}

void contig_compute_stats(const contig_memory_t *mem, contig_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    for (int i = 0; i < mem->num_blocks; i++) {
        const contig_block_t *b = &mem->blocks[i];
        if (b->used) continue;
        stats->free_total += b->size;
        stats->num_holes++;
        if (b->size > stats->largest_hole) stats->largest_hole = b->size;
    }
    if (stats->free_total > 0) {
        stats->external_fragmentation = 1.0 - (double)stats->largest_hole / (double)stats->free_total;
    }
}

static bool fits(const contig_block_t *b, int size) {
    return !b->used && b->size >= size;
}

static int first_fit(const contig_memory_t *mem, int size, int rover) {
    (void)rover;
    for (int i = 0; i < mem->num_blocks; i++) {
        if (fits(&mem->blocks[i], size)) return i;
    }
    return NO_HOLE;
}

static int best_fit(const contig_memory_t *mem, int size, int rover) {
    (void)rover;
    int best = NO_HOLE;
    for (int i = 0; i < mem->num_blocks; i++) {
        if (fits(&mem->blocks[i], size) && (best == NO_HOLE || mem->blocks[i].size < mem->blocks[best].size)) best = i;
    }
    return best;
}

static int worst_fit(const contig_memory_t *mem, int size, int rover) {
    (void)rover;
    int worst = NO_HOLE;
    for (int i = 0; i < mem->num_blocks; i++) {
        if (fits(&mem->blocks[i], size) && (worst == NO_HOLE || mem->blocks[i].size > mem->blocks[worst].size)) worst = i;
    }
    return worst;
}

static int next_fit(const contig_memory_t *mem, int size, int rover) {
    for (int i = 0; i < mem->num_blocks; i++) {
        if (mem->blocks[i].start >= rover && fits(&mem->blocks[i], size)) return i;
    }
    for (int i = 0; i < mem->num_blocks; i++) {
        if (mem->blocks[i].start < rover && fits(&mem->blocks[i], size)) return i;
    }
    return NO_HOLE;
}

static const contig_ops_t CONTIG_POLICIES[CONTIG_COUNT] = {
    [CONTIG_FIRST_FIT] = {CONTIG_FIRST_FIT, "First fit", "first", first_fit},
    [CONTIG_BEST_FIT]  = {CONTIG_BEST_FIT, "Best fit", "best", best_fit},
    [CONTIG_WORST_FIT] = {CONTIG_WORST_FIT, "Worst fit", "worst", worst_fit},
    [CONTIG_NEXT_FIT]  = {CONTIG_NEXT_FIT, "Next fit", "next", next_fit},
};

const contig_ops_t *contig_get_ops(contig_policy_t policy) {
    bool known = policy >= 0 && policy < CONTIG_COUNT;
    return &CONTIG_POLICIES[known ? policy : CONTIG_FIRST_FIT];
}

bool contig_lookup(const char *key, contig_policy_t *out) {
    for (int i = 0; i < CONTIG_COUNT; i++) {
        if (text_equals_ignore_case(key, CONTIG_POLICIES[i].key) || text_equals_ignore_case(key, CONTIG_POLICIES[i].name)) {
            *out = (contig_policy_t)i;
            return true;
        }
    }
    return false;
}

const char *contig_strerror(contig_status_t status) {
    switch (status) {
        case CONTIG_OK:              return "ok";
        case CONTIG_ERR_EMPTY:       return "the script is empty";
        case CONTIG_ERR_TOO_MANY:    return "too many operations (max 64)";
        case CONTIG_ERR_SYNTAX:      return "use NAME=SIZE to allocate, free NAME (or -NAME), compact";
        case CONTIG_ERR_SIZE:        return "sizes must be positive";
        case CONTIG_ERR_NAME:        return "names are letters, digits, _ or # (max 11)";
        case CONTIG_ERR_MEMORY_SIZE: return "memory size must be between 1 and 1000000";
    }
    return "unknown error";
}

static void copy_name(char dst[CONTIG_NAME_LEN], const char *src) {
    size_t length = strlen(src);
    if (length >= CONTIG_NAME_LEN) length = CONTIG_NAME_LEN - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int find_used(const contig_memory_t *mem, const char *name) {
    for (int i = 0; i < mem->num_blocks; i++) {
        if (mem->blocks[i].used && strcmp(mem->blocks[i].name, name) == 0) return i;
    }
    return NO_HOLE;
}

static void insert_block(contig_memory_t *mem, int index, contig_block_t block) {
    memmove(&mem->blocks[index + 1], &mem->blocks[index], (size_t)(mem->num_blocks - index) * sizeof(contig_block_t));
    mem->blocks[index] = block;
    mem->num_blocks++;
}

static void remove_block(contig_memory_t *mem, int index) {
    memmove(&mem->blocks[index], &mem->blocks[index + 1], (size_t)(mem->num_blocks - index - 1) * sizeof(contig_block_t));
    mem->num_blocks--;
}

static int place(contig_memory_t *mem, int hole, int size, const char *name) {
    contig_block_t *h = &mem->blocks[hole];
    int address = h->start;
    contig_block_t used = {address, size, true, ""};
    copy_name(used.name, name);

    if (h->size == size) {
        *h = used;
    } else {
        h->start += size;
        h->size -= size;
        insert_block(mem, hole, used);
    }
    return address;
}

static void merge_holes(contig_memory_t *mem, int index) {
    if (index + 1 < mem->num_blocks && !mem->blocks[index + 1].used) {
        mem->blocks[index].size += mem->blocks[index + 1].size;
        remove_block(mem, index + 1);
    }
    if (index > 0 && !mem->blocks[index - 1].used) {
        mem->blocks[index - 1].size += mem->blocks[index].size;
        remove_block(mem, index);
    }
}

static int compact(contig_memory_t *mem) {
    contig_memory_t packed;
    memset(&packed, 0, sizeof(packed));
    packed.size = mem->size;

    int next_address = 0, moved = 0;
    for (int i = 0; i < mem->num_blocks; i++) {
        contig_block_t block = mem->blocks[i];
        if (!block.used) continue;
        if (block.start != next_address) moved += block.size;
        block.start = next_address;
        next_address += block.size;
        packed.blocks[packed.num_blocks++] = block;
    }
    if (next_address < mem->size) {
        packed.blocks[packed.num_blocks++] = (contig_block_t){next_address, mem->size - next_address, false, ""};
    }
    *mem = packed;
    return moved;
}

static void run_alloc(const contig_ops_t *ops, contig_memory_t *mem, int *rover, contig_step_t *step) {
    bool name_taken = find_used(mem, step->op.name) != NO_HOLE;
    bool table_full = mem->num_blocks >= CONTIG_MAX_BLOCKS;
    int hole = name_taken || table_full ? NO_HOLE : ops->pick_hole(mem, step->op.size, *rover);
    if (hole == NO_HOLE) return;

    step->hole_size = mem->blocks[hole].size;
    step->address = place(mem, hole, step->op.size, step->op.name);
    step->ok = true;
    *rover = step->address + step->op.size;
}

static void run_free(contig_memory_t *mem, contig_step_t *step) {
    int index = find_used(mem, step->op.name);
    if (index == NO_HOLE) return;

    step->address = mem->blocks[index].start;
    mem->blocks[index].used = false;
    mem->blocks[index].name[0] = '\0';
    merge_holes(mem, index);
    step->ok = true;
}

contig_status_t contig_run(contig_policy_t policy, int memory_size, const contig_op_t *ops, int count,
                           contig_result_t *out) {
    memset(out, 0, sizeof(*out));
    out->policy = policy;
    if (memory_size < 1 || memory_size > CONTIG_MAX_MEMORY) return CONTIG_ERR_MEMORY_SIZE;
    if (count <= 0) return CONTIG_ERR_EMPTY;
    if (count > CONTIG_MAX_OPS) return CONTIG_ERR_TOO_MANY;

    const contig_ops_t *policy_ops = contig_get_ops(policy);
    contig_memory_t mem;
    contig_init(&mem, memory_size);
    int rover = 0;

    for (int i = 0; i < count; i++) {
        contig_step_t *step = &out->steps[out->num_steps++];
        step->op = ops[i];
        step->address = -1;

        switch (ops[i].kind) {
            case CONTIG_OP_ALLOC:
                run_alloc(policy_ops, &mem, &rover, step);
                if (!step->ok) out->failed++;
                break;
            case CONTIG_OP_FREE:
                run_free(&mem, step);
                break;
            case CONTIG_OP_COMPACT:
                step->moved = compact(&mem);
                step->ok = true;
                out->compacted_bytes += step->moved;
                rover = 0;
                break;
        }
        step->memory = mem;
        contig_compute_stats(&mem, &step->stats);
    }
    return CONTIG_OK;
}

static bool valid_name(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || length >= CONTIG_NAME_LEN) return false;
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != CONTIG_RESERVED) return false;
    }
    return true;
}

static const char *next_token(const char *p, char *token, size_t size) {
    while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
    size_t length = 0;
    while (p[length] && !isspace((unsigned char)p[length]) && p[length] != ',' && p[length] != ';') length++;
    snprintf(token, size, "%.*s", (int)(length < size ? length : size - 1), p);
    return p + length;
}

static contig_status_t parse_alloc(const char *token, contig_op_t *op) {
    const char *equals = strchr(token, '=');
    char name[CONTIG_NAME_LEN + 8];
    snprintf(name, sizeof(name), "%.*s", (int)(equals - token), token);
    if (!valid_name(name)) return CONTIG_ERR_NAME;

    char *end;
    long size = strtol(equals + 1, &end, 10);
    if (*end != '\0' || end == equals + 1) return CONTIG_ERR_SYNTAX;
    if (size <= 0 || size > CONTIG_MAX_MEMORY) return CONTIG_ERR_SIZE;

    op->kind = CONTIG_OP_ALLOC;
    op->size = (int)size;
    copy_name(op->name, name);
    return CONTIG_OK;
}

static contig_status_t parse_free(const char *name, contig_op_t *op) {
    if (!valid_name(name)) return CONTIG_ERR_NAME;
    op->kind = CONTIG_OP_FREE;
    op->size = 0;
    copy_name(op->name, name);
    return CONTIG_OK;
}

contig_status_t contig_parse_script(const char *text, contig_op_t *ops, int max_ops, int *count, char *bad_token,
                                    int bad_size) {
    *count = 0;
    if (bad_token && bad_size > 0) bad_token[0] = '\0';

    char token[64];
    for (const char *p = next_token(text, token, sizeof(token)); token[0]; p = next_token(p, token, sizeof(token))) {
        if (*count >= max_ops) return CONTIG_ERR_TOO_MANY;
        contig_op_t *op = &ops[*count];
        memset(op, 0, sizeof(*op));
        contig_status_t status;

        if (text_equals_ignore_case(token, "compact")) {
            op->kind = CONTIG_OP_COMPACT;
            status = CONTIG_OK;
        } else if (text_equals_ignore_case(token, "free")) {
            p = next_token(p, token, sizeof(token));
            status = parse_free(token, op);
        } else if (token[0] == '-') {
            status = parse_free(token + 1, op);
        } else if (strchr(token, '=')) {
            status = parse_alloc(token, op);
        } else {
            status = CONTIG_ERR_SYNTAX;
        }

        if (status != CONTIG_OK) {
            if (bad_token) snprintf(bad_token, (size_t)bad_size, "%s", token);
            return status;
        }
        (*count)++;
    }
    return *count > 0 ? CONTIG_OK : CONTIG_ERR_EMPTY;
}
