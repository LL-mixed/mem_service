#ifndef MEM_SERVICE_REPLAY_HISTORY_H
#define MEM_SERVICE_REPLAY_HISTORY_H

#include "mem_service.h"

/* Internal daemon storage, not an installed SDK contract. Single owner;
 * callers serialize all operations and retain the committed prefix in their
 * durable snapshot before evicting any cached response. */
struct mem_service_replay_history {
    int fd;
    bool failed;
    uint64_t count;
    uint64_t checksum;
    char path[512];
};

/* create=true requires an absent path and a zero checkpoint. Opening an
 * existing file verifies the entire history and the specified prefix. */
int mem_service_replay_history_open(struct mem_service_replay_history *history,
    const char *path, bool create, uint64_t checkpoint_count,
    uint64_t checkpoint_checksum);
int mem_service_replay_history_close(struct mem_service_replay_history *history);

/* Exact duplicates are no-ops; conflicting content for an existing key is
 * rejected. No result can be evicted until append returns zero. */
int mem_service_replay_history_append(struct mem_service_replay_history *history,
    const struct mem_service_idempotency_record *record);

/* Validate all history once for 1..MAX cache records, reject conflicts before
 * writing, then sync new frames as one batch. Exact duplicates are no-ops.
 * Uncertain I/O poisons the handle and leaves its checkpoint unchanged;
 * reopening may recover a complete suffix. Retain the entire caller cache
 * until a snapshot containing the new checkpoint has been persisted. */
int mem_service_replay_history_append_batch(struct mem_service_replay_history *history,
    const struct mem_service_idempotency_record *const *records, size_t count);

/* Returns 1 when found, 0 when absent, -1 on an untrustworthy history.
 * On absence/error the output is unchanged. */
int mem_service_replay_history_find(struct mem_service_replay_history *history,
    const char *key, struct mem_service_idempotency_record *record);

#endif
