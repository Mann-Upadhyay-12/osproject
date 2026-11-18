#ifndef PRISM_DISK_H
#define PRISM_DISK_H

#include <stdbool.h>
#include <stdint.h>

#define DISK_MAX_REQUESTS  64
#define DISK_MAX_STOPS     (DISK_MAX_REQUESTS + 4)
#define DISK_MAX_CYLINDERS 100000

typedef enum {
    DISK_FCFS = 0,
    DISK_SSTF,
    DISK_SCAN,
    DISK_CSCAN,
    DISK_LOOK,
    DISK_CLOOK,
    DISK_COUNT
} disk_policy_t;

typedef enum {
    DISK_TOWARD_LOW = 0,
    DISK_TOWARD_HIGH
} disk_direction_t;

typedef enum {
    DISK_STOP_REQUEST = 0,
    DISK_STOP_EDGE,
    DISK_STOP_JUMP
} disk_stop_kind_t;

typedef struct {
    int              cylinders;
    int              head;
    disk_direction_t direction;
    int              num_requests;
    int              requests[DISK_MAX_REQUESTS];
} disk_input_t;

typedef struct {
    int              cylinder;
    disk_stop_kind_t kind;
    int              request_index;
} disk_stop_t;

typedef struct {
    disk_policy_t policy;
    int           start;
    int           num_stops;
    disk_stop_t   stops[DISK_MAX_STOPS];
    int           total_movement;
    int           return_movement;
} disk_result_t;

typedef struct disk_ops {
    disk_policy_t policy;
    const char   *name;
    const char   *key;
    void        (*schedule)(const disk_input_t *in, disk_result_t *out);
} disk_ops_t;

const disk_ops_t *disk_get_ops(disk_policy_t policy);
bool              disk_lookup(const char *key, disk_policy_t *out);

typedef enum {
    DISK_OK = 0,
    DISK_ERR_EMPTY,
    DISK_ERR_TOO_MANY,
    DISK_ERR_SYNTAX,
    DISK_ERR_RANGE,
    DISK_ERR_HEAD,
    DISK_ERR_CYLINDERS
} disk_status_t;

const char   *disk_strerror(disk_status_t status);
disk_status_t disk_parse_requests(const char *text, disk_input_t *in);
disk_status_t disk_validate(const disk_input_t *in);
disk_status_t disk_schedule(disk_policy_t policy, const disk_input_t *in, disk_result_t *out);

int disk_distance_moved(const disk_result_t *result, int stop_count);

#endif
