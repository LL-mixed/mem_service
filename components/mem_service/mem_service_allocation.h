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
 *      |                     |
 *      |                     +-- retire (cancel) --> RETIRING
 *      |
 *   ACTIVE --retire--> RETIRING --last holder released + backing released-->
 *   RETIRED
 *      |
 *      +-- backing release outcome unconfirmed --> QUARANTINED
 *
 * - ALLOCATING records the operation intent before resources are reserved.
 *   With an in-process backing (fixtures) the reserve is synchronous and
 *   ALLOCATING never persists across a request. With a provider-bound
 *   allocation (M1.2) ALLOCATING persists until the bound home provider
 *   publishes the reserved descriptor. Retire on ALLOCATING waits for home
 *   cancellation confirmation; a late publish remains RETIRING and records
 *   the reservation for cleanup without permitting new references.
 * - ACTIVE accepts new holder references.
 * - RETIRING blocks new references; the last release drives backing
 *   release and the transition to RETIRED. Provider-backed objects wait
 *   in RETIRING for the home provider's reclaim confirmation instead of
 *   an in-process release.
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
 * Backing: reserve/release are delegated either to an in-process backing
 * provider interface (test fixtures) or to the bound home provider
 * process (M1.2): the control plane binds the allocation to
 * (home_node_id, provider_incarnation) at allocate time, the provider
 * reserves backing plus address and publishes the opaque descriptor and
 * address range, and later confirms the reclaim. The control plane never
 * parses descriptor contents and never moves payload bytes. Stats
 * export mapping counts reflect provider-published ACTIVE objects;
 * import mapping counts reflect confirmed mapping transactions. Pending
 * and closing transactions also count as in-flight; no transaction may
 * outlive its holder. Actual process mapping handles remain provider-private.
 */

#define MEM_SERVICE_MANAGED_MAX_ALLOCATIONS 128U
#define MEM_SERVICE_MANAGED_MAX_HOLDERS 8U
#define MEM_SERVICE_MANAGED_MAX_MAPPINGS 128U
#define MEM_SERVICE_MANAGED_KEY_LEN 96U
#define MEM_SERVICE_MANAGED_IDEMPOTENCY_KEY_LEN 96U
#define MEM_SERVICE_MANAGED_SESSION_ID_LEN 64U
#define MEM_SERVICE_MANAGED_NODE_ID_LEN 64U
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
    /* Publish/reclaim named a (node_id, incarnation) other than the
     * provider binding recorded at allocate time. */
    MEM_SERVICE_MANAGED_RESULT_PROVIDER_MISMATCH = 10,
    /* The configured home provider holds no active registration in the
     * provider directory; the allocation was not attempted. */
    MEM_SERVICE_MANAGED_RESULT_PROVIDER_UNAVAILABLE = 11,
    MEM_SERVICE_MANAGED_RESULT_VERSION_CONFLICT = 12,
};

struct mem_service_managed_holder {
    char session_id[MEM_SERVICE_MANAGED_SESSION_ID_LEN];
    uint64_t generation;
};

enum mem_service_managed_mapping_state {
    MEM_SERVICE_MANAGED_MAPPING_NONE = 0,
    MEM_SERVICE_MANAGED_MAPPING_PENDING = 1,
    MEM_SERVICE_MANAGED_MAPPING_ACTIVE = 2,
    MEM_SERVICE_MANAGED_MAPPING_CLOSING = 3,
};

enum mem_service_managed_mapping_action {
    MEM_SERVICE_MANAGED_MAPPING_BEGIN = 1,
    MEM_SERVICE_MANAGED_MAPPING_CONFIRM = 2,
    MEM_SERVICE_MANAGED_MAPPING_CLOSE = 3,
    MEM_SERVICE_MANAGED_MAPPING_FINISH = 4,
    MEM_SERVICE_MANAGED_MAPPING_CANCEL = 5,
    MEM_SERVICE_MANAGED_MAPPING_INSPECT = 6,
};

struct mem_service_managed_mapping {
    enum mem_service_managed_mapping_state state;
    uint64_t id;
    uint64_t generation;
    char key[MEM_SERVICE_MANAGED_KEY_LEN];
    char session_id[MEM_SERVICE_MANAGED_SESSION_ID_LEN];
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
    /* Home provider binding recorded at allocate time (provider-backed
     * objects only; empty for in-process stub backings). */
    char home_node_id[MEM_SERVICE_MANAGED_NODE_ID_LEN];
    /* True when the descriptor/address range came from the bound home
     * provider's publish; reclaim then waits for provider confirmation
     * instead of an in-process release. */
    bool provider_backed;
    uint64_t address;
    uint64_t address_len;
    uint8_t descriptor[MEM_SERVICE_MANAGED_DESCRIPTOR_MAX_LEN];
    uint32_t descriptor_len;
    /* Opt-in V2 content publication. A failed write never revives old refs. */
    bool reference_mode;
    bool content_writing;
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
    uint64_t next_mapping_id;
    struct mem_service_managed_mapping mappings[MEM_SERVICE_MANAGED_MAX_MAPPINGS];
    uint64_t allocate_ok_count;
    uint64_t acquire_ok_count;
    uint64_t release_ok_count;
    uint64_t retire_ok_count;
    uint64_t allocate_rejected_count;
    uint64_t acquire_rejected_count;
    uint64_t release_rejected_count;
    uint64_t retire_rejected_count;
    uint64_t publish_ok_count;
    uint64_t publish_rejected_count;
    uint64_t reclaim_ok_count;
    uint64_t reclaim_rejected_count;
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
    uint64_t publish_ok_count;
    uint64_t publish_rejected_count;
    uint64_t reclaim_ok_count;
    uint64_t reclaim_rejected_count;
    uint64_t quarantine_events;
};

struct mem_service_managed_request {
    const char *key;
    const char *idempotency_key;
    const char *session_id;
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint64_t capabilities;
    /* Provider binding for the M1.2 provider-backed path: when
     * home_node_id is non-NULL the allocation is bound to that home
     * provider boot and persists in ALLOCATING until the provider
     * publishes; when NULL the in-process backing is reserved
     * synchronously. */
    const char *home_node_id;
    uint64_t home_incarnation;
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
    char home_node_id[MEM_SERVICE_MANAGED_NODE_ID_LEN];
    bool provider_backed;
    uint64_t address;
    uint64_t address_len;
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

/* Quarantine unresolved allocations after a confirmed directory loss event.
 * Besides this home instance, retain all provider allocations with holders:
 * holder sessions currently have no authoritative node binding. No resource,
 * reference or mapping is released. Returns newly quarantined entry count.
 * The caller owns serialization and must latch data admission off if nonzero.
 */
size_t mem_service_managed_provider_lost(
    struct mem_service_managed_table *table,
    const char *node_id,
    uint64_t incarnation);

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
/* Read-only scan; repeat from zero at end. No exclusive claim is granted. */
enum mem_service_managed_result mem_service_managed_poll(
    const struct mem_service_managed_table *table,
    const char *node_id,
    uint64_t incarnation,
    uint64_t after_generation,
    struct mem_service_managed_view *view_out);
/*
 * Provider-backed reserve confirmation (M1.2): the bound home provider
 * reserved backing plus address and publishes the opaque descriptor and
 * address range. The control plane stores them verbatim and drives
 * ALLOCATING --> ACTIVE. (key, generation) names the allocation
 * identity; (node_id, incarnation) must equal the binding recorded at
 * allocate time. A replayed publish with identical content on an ACTIVE
 * object is idempotent; conflicting content or a stale provider boot is
 * rejected. Payload bytes never cross this interface.
 */
enum mem_service_managed_result mem_service_managed_publish(
    struct mem_service_managed_table *table,
    const char *key,
    const char *node_id,
    uint64_t incarnation,
    uint64_t generation,
    const uint8_t *descriptor,
    uint32_t descriptor_len,
    uint64_t address,
    uint64_t address_len,
    struct mem_service_managed_view *view_out);
/*
 * Provider-backed release confirmation (M1.2): the bound home provider
 * reports the outcome of the backing release for a drained (RETIRING,
 * zero holders) object. confirmed=true retires the identity;
 * confirmed=false quarantines it (resources isolated, counted
 * separately, never reused). Replays against the terminal state with a
 * matching identity are idempotent.
 */
enum mem_service_managed_result mem_service_managed_reclaim(
    struct mem_service_managed_table *table,
    const char *key,
    const char *node_id,
    uint64_t incarnation,
    uint64_t generation,
    bool confirmed,
    struct mem_service_managed_view *view_out);
void mem_service_managed_stats_snapshot(
    const struct mem_service_managed_table *table,
    struct mem_service_managed_stats *stats_out);

/* BEGIN requires mapping_id=0. Other actions require the exact issued ID.
 * CLOSE must precede provider teardown; FINISH requires confirmed teardown.
 * CANCEL applies only to a pending attempt with no remaining provider resource.
 * Missing/stale IDs fail closed. Wire idempotency handles completed retries. */
enum mem_service_managed_result mem_service_managed_mapping_transition(
    struct mem_service_managed_table *table,
    const char *key,
    const char *session_id,
    uint64_t generation,
    uint64_t mapping_id,
    enum mem_service_managed_mapping_action action,
    struct mem_service_managed_mapping *mapping_out);

/* Serialized core operations. Begin advances version before payload writes;
 * seal requires the caller to have completed provider publication and unmap.
 * Callers retain the sole owner holder throughout the write transaction. */
enum mem_service_managed_result mem_service_managed_mapping_begin_published(
    struct mem_service_managed_table *table, const char *key, const char *session_id,
    uint64_t generation, uint64_t version,
    struct mem_service_managed_mapping *mapping_out);
enum mem_service_managed_result mem_service_managed_content_begin(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t expected_version,
    struct mem_service_managed_view *view_out);
enum mem_service_managed_result mem_service_managed_content_check(
    const struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t version,
    bool writing, struct mem_service_managed_view *view_out);
enum mem_service_managed_result mem_service_managed_content_seal(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t version);
/* Internal authority seam: the object-record layer must first validate the
 * complete registered reference. This is not a raw wire acquire substitute. */
enum mem_service_managed_result mem_service_managed_acquire_published(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t version,
    struct mem_service_managed_view *view_out);

#endif
