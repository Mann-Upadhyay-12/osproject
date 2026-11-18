#include "prism/sim.h"

const char *event_type_str(event_type_t type) {
    switch (type) {
        case EVENT_PROC_ARRIVE:         return "ARRIVE";
        case EVENT_PROC_DISPATCH:       return "DISPATCH";
        case EVENT_PROC_PREEMPT:        return "PREEMPT";
        case EVENT_PROC_BLOCK_IO:       return "BLOCK_IO";
        case EVENT_PROC_BLOCK_FAULT:    return "BLOCK_FAULT";
        case EVENT_PROC_WAKE_IO:        return "WAKE_IO";
        case EVENT_PROC_WAKE_FAULT:     return "WAKE_FAULT";
        case EVENT_PROC_TERMINATE:      return "TERMINATE";
        case EVENT_PAGE_ACCESS:         return "PAGE_ACCESS";
        case EVENT_PAGE_FAULT:          return "PAGE_FAULT";
        case EVENT_PAGE_EVICT:          return "PAGE_EVICT";
        case EVENT_RAM_SHRUNK:          return "RAM_SHRUNK";
        case EVENT_PROC_KILLED:         return "PROC_KILLED";
        case EVENT_RESOURCE_REQUEST:    return "RES_REQUEST";
        case EVENT_RESOURCE_ACQUIRED:   return "RES_ACQUIRED";
        case EVENT_RESOURCE_RELEASED:   return "RES_RELEASED";
        case EVENT_RESOURCE_BLOCKED:    return "RES_BLOCKED";
        case EVENT_DEADLOCK_DETECTED:   return "DEADLOCK_FOUND";
        case EVENT_DEADLOCK_RESOLVED:   return "DEADLOCK_RESOLVED";
        case EVENT_IPC_SEND:            return "IPC_SEND";
        case EVENT_IPC_RECV:            return "IPC_RECV";
        case EVENT_IPC_BLOCKED:         return "IPC_BLOCKED";
        case EVENT_CHECKPOINT_SAVED:    return "CKPT_SAVED";
        case EVENT_CHECKPOINT_RESTORED: return "CKPT_RESTORED";
        case EVENT_FAULT_INJECTED:      return "FAULT_INJECTED";
        case EVENT_SEM_WAIT:            return "SEM_WAIT";
        case EVENT_SEM_BLOCKED:         return "SEM_BLOCKED";
        case EVENT_SEM_SIGNAL:          return "SEM_SIGNAL";
        case EVENT_SEM_WAKE:            return "SEM_WAKE";
        case EVENT_STALLED:             return "STALLED";
        case EVENT_JOB_RELEASE:         return "JOB_RELEASE";
        case EVENT_JOB_COMPLETE:        return "JOB_DONE";
        case EVENT_DEADLINE_MISS:       return "DEADLINE_MISS";
        case EVENT_MIGRATE:             return "MIGRATE";
        case EVENT_FORK:                return "FORK";
        case EVENT_WAIT_BLOCKED:        return "WAIT_BLOCKED";
        case EVENT_ZOMBIE:              return "ZOMBIE";
        case EVENT_REAP:                return "REAP";
        case EVENT_ORPHAN:              return "ORPHAN";
        default:                        return "UNKNOWN";
    }
}

const char *proc_state_str(proc_state_t state) {
    switch (state) {
        case PROC_NEW:              return "NEW";
        case PROC_READY:            return "READY";
        case PROC_RUNNING:          return "RUNNING";
        case PROC_BLOCKED_IO:       return "BLOCK_IO";
        case PROC_BLOCKED_FAULT:    return "BLOCK_VM";
        case PROC_BLOCKED_RESOURCE: return "BLOCK_RES";
        case PROC_BLOCKED_IPC:      return "BLOCK_IPC";
        case PROC_BLOCKED_SEM:      return "BLOCK_SEM";
        case PROC_SLEEPING:         return "SLEEP";
        case PROC_BLOCKED_WAIT:     return "BLOCK_WAIT";
        case PROC_TERMINATED:       return "TERM";
        default:                    return "UNUSED";
    }
}
