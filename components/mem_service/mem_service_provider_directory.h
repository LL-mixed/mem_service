#ifndef MEM_SERVICE_PROVIDER_DIRECTORY_H
#define MEM_SERVICE_PROVIDER_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Control-plane provider directory (M1.2).
 *
 * The single control-plane daemon keeps one directory of the per-node
 * provider processes that registered over the wire protocol. A provider
 * registers only after finishing its bootstrap checks and peer-mapping
 * canary, and reports a generation'd readiness plus its capabilities.
 * The control plane aggregates this directory with the configured set of
 * required providers: data operations stay fail-closed until every
 * required provider holds an active (fresh) registration.
 *
 * Identity and lifecycle:
 *
 * - (node_id, incarnation) names one provider process boot. Rebooting a
 *   provider re-registers the same node_id with a new incarnation and
 *   atomically replaces the stale entry; the old incarnation can no
 *   longer refresh or deregister (incarnation_conflict).
 * - Registrations lease: a provider must refresh within lease_ms.
 *   Entries that miss the lease are expired by poll and no longer count
 *   towards readiness; data-plane readiness is revoked on loss.
 * - register/refresh/deregister are naturally idempotent by their
 *   (node_id, incarnation) key; a replayed request reports the current
 *   state without re-executing a mutation.
 *
 * The directory is transport-neutral: it stores control-plane identity
 * and readiness metadata only, never socket addresses, descriptors or
 * payload state. Readiness here does not prove any mapping is valid; it
 * only gates whether data operations may be attempted.
 */

#define MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS 32U
#define MEM_SERVICE_PROVIDER_NODE_ID_LEN 64U
#define MEM_SERVICE_PROVIDER_DIRECTORY_DEFAULT_LEASE_MS 5000U
#define MEM_SERVICE_PROVIDER_DIRECTORY_MIN_LEASE_MS 100U
#define MEM_SERVICE_PROVIDER_DIRECTORY_MAX_LEASE_MS 600000U

/* Provider capability bits mirror the managed object capability space:
 * bit 0 map/share, bit 1 block I/O. */
#define MEM_SERVICE_PROVIDER_CAP_MAP 1ULL
#define MEM_SERVICE_PROVIDER_CAP_BLOCK_IO 2ULL
#define MEM_SERVICE_PROVIDER_CAP_VALID_MASK \
    (MEM_SERVICE_PROVIDER_CAP_MAP | MEM_SERVICE_PROVIDER_CAP_BLOCK_IO)

enum mem_service_provider_directory_result {
    MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_OK = 0,
    /* Missing/empty node_id, zero incarnation, capability bits outside
     * the valid mask. */
    MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_INVALID_REQUEST = 1,
    /* Refresh or deregister named a node with no active registration. */
    MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_NOT_FOUND = 2,
    /* Refresh or deregister arrived from a stale incarnation of the
     * node; the active registration belongs to a newer boot. */
    MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_INCARNATION_CONFLICT = 3,
    /* Directory table is full. */
    MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_CAPACITY = 4,
};

struct mem_service_provider_directory_entry {
    bool in_use;
    char node_id[MEM_SERVICE_PROVIDER_NODE_ID_LEN];
    uint64_t incarnation;
    uint64_t readiness_generation;
    uint64_t capabilities;
    uint64_t registered_ms;
    uint64_t last_refresh_ms;
};

struct mem_service_provider_directory_config {
    char required_nodes[MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS]
                       [MEM_SERVICE_PROVIDER_NODE_ID_LEN];
    size_t required_count;
    uint64_t lease_ms;
};

struct mem_service_provider_directory_stats {
    uint64_t register_ok_count;
    uint64_t register_replace_count;
    uint64_t refresh_ok_count;
    uint64_t deregister_ok_count;
    uint64_t register_rejected_count;
    uint64_t refresh_rejected_count;
    uint64_t deregister_rejected_count;
    uint64_t incarnation_conflict_count;
    uint64_t expired_count;
};

struct mem_service_provider_directory {
    bool config_set;
    struct mem_service_provider_directory_config config;
    uint64_t directory_epoch;
    struct mem_service_provider_directory_entry
        entries[MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS];
    struct mem_service_provider_directory_stats stats;
};

struct mem_service_provider_directory_poll {
    /* True when every configured required provider holds a fresh
     * registration. With zero required providers this is always true so
     * legacy single-process deployments keep their existing behavior. */
    bool ready;
    size_t required_count;
    size_t active_count;
    size_t expired_this_poll;
    /* First required node without a fresh registration, or "". */
    char first_missing[MEM_SERVICE_PROVIDER_NODE_ID_LEN];
};

const char *mem_service_provider_directory_result_name(
    enum mem_service_provider_directory_result result);

void mem_service_provider_directory_init(
    struct mem_service_provider_directory *directory);

/* Validates and applies the required-provider set and lease. Fails on
 * duplicate/empty node identities, an over-long list or an out-of-bounds
 * lease; the directory is left unconfigured on failure. */
int mem_service_provider_directory_configure(
    struct mem_service_provider_directory *directory,
    const struct mem_service_provider_directory_config *config);

/* Registers one provider boot. Same (node_id, incarnation) refreshes
 * generation/capabilities idempotently; a new incarnation for a known
 * node replaces the stale entry and sets replaced_out. */
enum mem_service_provider_directory_result
mem_service_provider_directory_register(
    struct mem_service_provider_directory *directory,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    uint64_t capabilities,
    uint64_t now_ms,
    bool *replaced_out);

enum mem_service_provider_directory_result
mem_service_provider_directory_refresh(
    struct mem_service_provider_directory *directory,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    uint64_t now_ms);

enum mem_service_provider_directory_result
mem_service_provider_directory_deregister(
    struct mem_service_provider_directory *directory,
    const char *node_id,
    uint64_t incarnation,
    uint64_t now_ms);

/* Time-driven maintenance plus readiness aggregation: expires entries
 * that missed their lease (revoking data-plane readiness on loss) and
 * reports whether all required providers are fresh. */
struct mem_service_provider_directory_poll
mem_service_provider_directory_poll(
    struct mem_service_provider_directory *directory,
    uint64_t now_ms);

/* True when managed data operations may be attempted: either no
 * provider set is required (legacy behavior) or the directory poll is
 * ready. */
bool mem_service_provider_directory_data_ops_allowed(
    struct mem_service_provider_directory *directory,
    uint64_t now_ms);

size_t mem_service_provider_directory_active_count(
    const struct mem_service_provider_directory *directory,
    uint64_t now_ms);

uint64_t mem_service_provider_directory_effective_lease_ms(
    const struct mem_service_provider_directory *directory);

const struct mem_service_provider_directory_entry *
mem_service_provider_directory_find(
    const struct mem_service_provider_directory *directory,
    const char *node_id);

#endif
