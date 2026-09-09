#ifndef MEM_SERVICE_ALLOCATION_H
#define MEM_SERVICE_ALLOCATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Managed object allocation control plane (M1.1).
 *
 * This module owns managed object identity, holder references and lifecycle
 * state only. Payload access stays on provider channels; legacy
 * put/materialize record semantics are unchanged. A minimal managed record
 * carries: object key, allocating operation/idempotency identity, owner
 * session, allocation generation, content version, size/alignment/
 * capabilities, holder sessions, lifecycle state, provider instance
 * incarnation and an opaque provider descriptor. Mapping handles belong to
 * client processes and are never part of this record.
 *
 * State machine:
 *
 *   (none) --allocate--> ALLOCATING --backing reserved--> ACTIVE
 *      |                     |
 *      |                     +-- reserve failed --> (slot rolled back)
 *      |
 *   ACTIVE --retire--> RETIRING --last holder released + backing released-->
 *   RETIRED
 *      |
 *      +-- backing release outcome unconfirmed --> QUARANTINED
 *
 * - ALLOCATING records the operation intent before resources are reserved;
 *   it never persists across a request (reserve is synchronous in M1.1).
 * - ACTIVE accepts new holder references.
 * - RETIRING blocks new references; the last release drives backing
 *   release and the transition to RETIRED.
 * - RETIRED is terminal. Allocating the same key again creates a fresh
 *   identity with a new generation (address/backing reuse always produces
 *   a new generation).
 * - QUARANTINED isolates resources whose rollback could not be confirmed;
 *   they are counted separately in stats and never reused.
 *
 * Durability: the table is volatile in M1.1. Durable restore of managed
 * allocations lands together with the provider-backed reserve/release in
 * M1.2; a daemon restart therefore starts with an empty managed table and
 * zero backing (fail-closed for data operations).
 *
 * Backing: reserve/release are delegated to a backing provider interface.
 * The shipped daemon registers no backing in M1.1, so allocate is
 * fail-closed; test fixtures register stub backings to exercise the state
 * machine. Stats export/import mapping counts stay 0 until provider
 * channels report them in M1.2.
 */

#define MEM_SERVICE_MANAGED_MAX_ALLOCATIONS 128U
#define MEM_SERVICE_MANAGED_MAX_HOLDERS 8U
#define MEM_SERVICE_MANAGED_KEY_LEN 96U
#define MEM_SERVICE_MANAGED_IDEMPOTENCY_KEY_LEN 96U
#define MEM_SERVICE_MANAGED_SESSION_ID_LEN 64U
#define MEM_SERVICE_MANAGED_DESCRIPTOR_MAX_LEN 128U

enum mem_service_managed_state {
    MEM_SERVICE_MANAGED_STATE_ALLOCATING = 1,
    MEM_SERVICE_MANAGED_STATE_ACTIVE = 2,
    MEM_SERVICE_MANAGED_STATE_RETIRING = 3,
    MEM_SERVICE_MANAGED_STATE_RETIRED = 4,
    MEM_SERVICE_MANAGED_STATE_QUARANTINED = 5,
};

enum mem_service_managed_capability {
    MEM_SERVICE_MANAGED_CAP_MAP = 1ULL << 0,
    MEM_SERVICE_MANAGED_CAP_BLOCK_IO = 1ULL << 1,
};

#define MEM_SERVICE_MANAGED_CAP_VALID_MASK \
    (MEM_SERVICE_MANAGED_CAP_MAP | MEM_SERVICE_MANAGED_CAP_BLOCK_IO)

enum mem_service_managed_result {
    MEM_SERVICE_MANAGED_RESULT_OK = 0,
    /* Request shape is invalid: missing/empty required field, zero size,
     * non power-of-two alignment or capability bits outside the valid
     * mask. */
    MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST = 1,
    MEM_SERVICE_MANAGED_RESULT_NOT_FOUND = 2,
    /* Key already names a live (allocating/active/retiring) or
     * quarantined object; the conflicting identity is not reused. */
    MEM_SERVICE_MANAGED_RESULT_KEY_CONFLICT = 3,
    /* Caller-provided expected_generation does not match the current
     * allocation generation. */
    MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION = 4,
    /* Allocation table or holder list is full. */
    MEM_SERVICE_MANAGED_RESULT_CAPACITY = 5,
    /* No backing provider is registered; data operations are
     * fail-closed. */
    MEM_SERVICE_MANAGED_RESULT_BACKING_UNAVAILABLE = 6,
    /* Backing reserve failed; the allocation intent was rolled back. */
    MEM_SERVICE_MANAGED_RESULT_BACKING_ERROR = 7,
    /* Release/acquire by a session that does not hold a reference. */
    MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER = 8,
    /* Lifecycle state does not allow the requested transition (for
     * example acquire on a retiring object or retire of an already
     * retired identity). */
    MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT = 9,
};

struct mem_service_managed_holder {
    char session_id[MEM_SERVICE_MANAGED_SESSION_ID_LEN];
    uint64_t generation;
};

struct mem_service_managed_allocation {
    bool in_use;
    enum mem_service_managed_state state;
    char key[MEM_SERVICE_MANAGED_KEY_LEN];
    char allocate_idempotency_key[MEM_SERVICE_MANAGED_IDEMPOTENCY_KEY_LEN];
    char owner_session[MEM_SERVICE_MANAGED_SESSION_ID_LEN];
    uint64_t generation;
    uint64_t version;
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint64_t capabilities;
    uint32_t holder_count;
    struct mem_service_managed_holder holders[MEM_SERVICE_MANAGED_MAX_HOLDERS];
    uint64_t provider_incarnation;
    uint8_t descriptor[MEM_SERVICE_MANAGED_DESCRIPTOR_MAX_LEN];
    uint32_t descriptor_len;
};

struct mem_service_managed_backing_ops {
    /*
     * Reserve backing and address for one object. On success the opaque
     * provider descriptor is filled and 0 is returned; on failure no
     * resource may remain reserved (the core treats a non-zero return as
     * a fully rolled-back attempt).
     */
    int (*reserve)(void *context,
                   uint64_t size_bytes,
                   uint64_t alignment_bytes,
                   uint64_t capabilities,
                   uint8_t *descriptor_out,
                   uint32_t descriptor_capacity,
                   uint32_t *descriptor_len_out);
    /*
     * Release the backing behind a descriptor. A non-zero return means
     * the release outcome could not be confirmed; the core quarantines
     * the object instead of reusing or forgetting its resources.
     */
    int (*release)(void *context,
                   const uint8_t *descriptor,
                   uint32_t descriptor_len);
};

struct mem_service_managed_table {
    bool backing_registered;
    const struct mem_service_managed_backing_ops *backing_ops;
    void *backing_context;
    uint64_t next_generation;
    uint64_t allocate_ok_count;
    uint64_t acquire_ok_count;
    uint64_t release_ok_count;
    uint64_t retire_ok_count;
    uint64_t allocate_rejected_count;
    uint64_t acquire_rejected_count;
    uint64_t release_rejected_count;
    uint64_t retire_rejected_count;
    uint64_t quarantine_events;
    struct mem_service_managed_allocation
        entries[MEM_SERVICE_MANAGED_MAX_ALLOCATIONS];
};

struct mem_service_managed_stats {
    uint64_t backing_registered;
    uint64_t live_objects;
    uint64_t backing_allocated_bytes;
    uint64_t address_reserved_bytes;
    uint64_t export_mappings;
    uint64_t import_mappings;
    uint64_t live_refs;
    uint64_t in_flight;
    uint64_t quarantined_objects;
    uint64_t quarantined_bytes;
    uint64_t allocate_ok_count;
    uint64_t acquire_ok_count;
    uint64_t release_ok_count;
    uint64_t retire_ok_count;
    uint64_t allocate_rejected_count;
    uint64_t acquire_rejected_count;
    uint64_t release_rejected_count;
    uint64_t retire_rejected_count;
    uint64_t quarantine_events;
};

struct mem_service_managed_request {
    const char *key;
    const char *idempotency_key;
    const char *session_id;
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint64_t capabilities;
};

struct mem_service_managed_view {
    enum mem_service_managed_state state;
    char key[MEM_SERVICE_MANAGED_KEY_LEN];
    char owner_session[MEM_SERVICE_MANAGED_SESSION_ID_LEN];
    uint64_t generation;
    uint64_t version;
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint64_t capabilities;
    uint32_t holder_count;
    struct mem_service_managed_holder holders[MEM_SERVICE_MANAGED_MAX_HOLDERS];
    uint64_t provider_incarnation;
    uint8_t descriptor[MEM_SERVICE_MANAGED_DESCRIPTOR_MAX_LEN];
    uint32_t descriptor_len;
};

const char *mem_service_managed_state_name(enum mem_service_managed_state state);
const char *mem_service_managed_result_name(enum mem_service_managed_result result);

void mem_service_managed_table_init(struct mem_service_managed_table *table);
int mem_service_managed_table_register_backing(
    struct mem_service_managed_table *table,
    const struct mem_service_managed_backing_ops *ops,
    void *context);
void mem_service_managed_table_unregister_backing(
    struct mem_service_managed_table *table);

enum mem_service_managed_result mem_service_managed_allocate(
    struct mem_service_managed_table *table,
    const struct mem_service_managed_request *request,
    struct mem_service_managed_view *view_out);
enum mem_service_managed_result mem_service_managed_acquire(
    struct mem_service_managed_table *table,
    const char *key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_managed_view *view_out);
enum mem_service_managed_result mem_service_managed_release(
    struct mem_service_managed_table *table,
    const char *key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_managed_view *view_out);
enum mem_service_managed_result mem_service_managed_retire(
    struct mem_service_managed_table *table,
    const char *key,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_managed_view *view_out);
enum mem_service_managed_result mem_service_managed_inspect(
    const struct mem_service_managed_table *table,
    const char *key,
    struct mem_service_managed_view *view_out);
void mem_service_managed_stats_snapshot(
    const struct mem_service_managed_table *table,
    struct mem_service_managed_stats *stats_out);

#endif
