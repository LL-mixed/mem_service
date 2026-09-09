#ifndef MEM_SERVICE_CLIENT_H
#define MEM_SERVICE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem_service_wire.h"
#include "mem_service_wire_client.h"

#define MEM_SERVICE_CLIENT_API_VERSION 1U
#define MEM_SERVICE_CLIENT_ABI_VERSION 1U
#define MEM_SERVICE_CLIENT_API_COMPATIBILITY "source-compatible-within-v1"
#define MEM_SERVICE_CLIENT_ABI_COMPATIBILITY \
    "wire-header-and-client-record-layout-stable-within-v1"
#define MEM_SERVICE_CLIENT_RECORD_ABI_SIZE 808U
#define MEM_SERVICE_CLIENT_KEY_LEN 96U
#define MEM_SERVICE_CLIENT_ID_LEN 64U
#define MEM_SERVICE_CLIENT_STATE_LEN 32U
#define MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND "training-step-commit"
#define MEM_SERVICE_CLIENT_PAYLOAD_KIND_SEALED_LOCAL_BLOCK 64U
#define MEM_SERVICE_CLIENT_PAYLOAD_KIND_SEALED_CHUNKED_BLOCK 65U
#define MEM_SERVICE_CLIENT_PAYLOAD_KIND_TRANSPORT_LOOPBACK_BLOCK 66U
#define MEM_SERVICE_CLIENT_PAYLOAD_KIND_TRANSPORT_TCP_BLOCK 67U
#define MEM_SERVICE_CLIENT_PAYLOAD_KIND_UB_SSD_GSVA_BLOCK 68U
#define MEM_SERVICE_CLIENT_OBJECT_BACKEND_OBMM_POOL 0U
#define MEM_SERVICE_CLIENT_OBJECT_BACKEND_LEGACY_PAYLOAD \
    MEM_SERVICE_CLIENT_OBJECT_BACKEND_OBMM_POOL
#define MEM_SERVICE_CLIENT_OBJECT_BACKEND_UB_SSD_GSVA 1U

struct mem_service_client {
    const char *connect_spec;
    struct mem_service_wire_client_options wire_options;
};

struct mem_service_client_record {
    char key[MEM_SERVICE_CLIENT_KEY_LEN];
    uint32_t kind;
    char request_id[MEM_SERVICE_CLIENT_ID_LEN];
    char prefix_group[MEM_SERVICE_CLIENT_ID_LEN];
    char group_id[MEM_SERVICE_CLIENT_ID_LEN];
    char session_id[MEM_SERVICE_CLIENT_ID_LEN];
    char model_key[MEM_SERVICE_CLIENT_ID_LEN];
    char artifact_kind[MEM_SERVICE_CLIENT_ID_LEN];
    char artifact_id[MEM_SERVICE_CLIENT_ID_LEN];
    char block_hash[MEM_SERVICE_CLIENT_KEY_LEN];
    uint32_t placement_node;
    uint32_t placement_level;
    uint64_t hot_segment_id;
    char state[MEM_SERVICE_CLIENT_STATE_LEN];
    uint64_t version;
    uint64_t last_result_segment;
    uint32_t object_owner_node;
    uint32_t object_payload_kind;
    uint64_t object_backing_offset;
    uint64_t object_backing_len;
    uint64_t object_payload_checksum;
    uint32_t object_backend_kind;
    uint32_t object_backend_node;
    uint32_t object_backend_device_cna;
    uint32_t object_backend_flags;
    uint64_t object_backend_block_hi;
    uint64_t object_backend_block_lo;
    uint64_t object_backend_block_version;
    uint64_t object_backend_block_offset;
    uint64_t object_backend_block_bytes;
    uint64_t object_backend_block_checksum;
};

typedef char mem_service_client_record_size_must_match_abi[
    (sizeof(struct mem_service_client_record) ==
     MEM_SERVICE_CLIENT_RECORD_ABI_SIZE)
        ? 1
        : -1];

struct mem_service_client_object {
    const char *key;
    const char *idempotency_key;
    bool has_owner;
    uint32_t owner;
    bool has_payload_kind;
    uint32_t payload_kind;
    bool has_backing_offset;
    uint64_t backing_offset;
    bool has_backing_len;
    uint64_t backing_len;
    bool has_checksum;
    uint64_t checksum;
    bool has_version;
    uint64_t version;
    bool has_backend_kind;
    uint32_t backend_kind;
    bool has_backend_node;
    uint32_t backend_node;
    bool has_backend_device_cna;
    uint32_t backend_device_cna;
    bool has_backend_block_hi;
    uint64_t backend_block_hi;
    bool has_backend_block_lo;
    uint64_t backend_block_lo;
    bool has_backend_block_version;
    uint64_t backend_block_version;
    bool has_backend_block_offset;
    uint64_t backend_block_offset;
    bool has_backend_block_bytes;
    uint64_t backend_block_bytes;
    bool has_backend_block_checksum;
    uint64_t backend_block_checksum;
    const char *payload_inline;
    const char *payload_path;
};

struct mem_service_client_block_entry {
    const char *request_id;
    const char *prefix_group;
    const char *group_id;
    const char *block_hash;
    const char *idempotency_key;
    bool has_placement_node;
    uint32_t placement_node;
    bool has_placement_level;
    uint32_t placement_level;
    bool has_hot_segment_id;
    uint64_t hot_segment_id;
    const char *state;
    bool has_result_segment_id;
    uint64_t result_segment_id;
};

struct mem_service_client_kv_selector {
    const char *key;
    const char *block_hash;
};

struct mem_service_client_artifact {
    const char *key;
    const char *idempotency_key;
    const char *session_id;
    const char *request_id;
    const char *model_key;
    const char *artifact_kind;
    const char *artifact_id;
    bool has_owner;
    uint32_t owner;
    bool has_payload_kind;
    uint32_t payload_kind;
    bool has_backing_offset;
    uint64_t backing_offset;
    bool has_backing_len;
    uint64_t backing_len;
    bool has_checksum;
    uint64_t checksum;
    bool has_version;
    uint64_t version;
    const char *payload_inline;
    const char *payload_path;
};

struct mem_service_client_artifact_query {
    const char *key;
    const char *expected_session_id;
    const char *expected_model_key;
    const char *expected_artifact_kind;
    const char *expected_artifact_id;
    bool has_expected_owner;
    uint32_t expected_owner;
    bool has_expected_version;
    uint64_t expected_version;
    bool has_expected_checksum;
    uint64_t expected_checksum;
};

struct mem_service_client_training_ref {
    const char *key;
    const char *idempotency_key;
    const char *session_id;
    const char *request_id;
    const char *model_key;
    const char *artifact_id;
    bool has_owner;
    uint32_t owner;
    bool has_payload_kind;
    uint32_t payload_kind;
    bool has_backing_offset;
    uint64_t backing_offset;
    bool has_backing_len;
    uint64_t backing_len;
    bool has_checksum;
    uint64_t checksum;
    bool has_version;
    uint64_t version;
    const char *payload_inline;
    const char *payload_path;
};

struct mem_service_client_training_ref_query {
    const char *key;
    const char *expected_session_id;
    const char *expected_model_key;
    const char *expected_artifact_id;
    bool has_expected_owner;
    uint32_t expected_owner;
    bool has_expected_version;
    uint64_t expected_version;
    bool has_expected_checksum;
    uint64_t expected_checksum;
};

void mem_service_client_init(struct mem_service_client *client,
                             const char *connect_spec);
void mem_service_client_init_with_options(
    struct mem_service_client *client,
    const char *connect_spec,
    const struct mem_service_wire_client_options *options);

int mem_service_client_health(const struct mem_service_client *client,
                              enum mem_service_wire_status *status_out);
int mem_service_client_ready(const struct mem_service_client *client,
                             enum mem_service_wire_status *status_out);
int mem_service_client_status(const struct mem_service_client *client,
                              char *payload_out,
                              size_t payload_out_len,
                              enum mem_service_wire_status *status_out);
int mem_service_client_list_records(const struct mem_service_client *client,
                                    char *payload_out,
                                    size_t payload_out_len,
                                    enum mem_service_wire_status *status_out);
int mem_service_client_export_snapshot(const struct mem_service_client *client,
                                       char *payload_out,
                                       size_t payload_out_len,
                                       enum mem_service_wire_status *status_out);
int mem_service_client_export_snapshot_page(const struct mem_service_client *client,
                                            uint64_t start_index,
                                            uint64_t max_records,
                                            char *payload_out,
                                            size_t payload_out_len,
                                            enum mem_service_wire_status *status_out);
int mem_service_client_restore_snapshot(const struct mem_service_client *client,
                                        const char *snapshot_payload,
                                        char *payload_out,
                                        size_t payload_out_len,
                                        enum mem_service_wire_status *status_out);
int mem_service_client_restore_snapshot_page(const struct mem_service_client *client,
                                             const char *page_payload,
                                             char *payload_out,
                                             size_t payload_out_len,
                                             enum mem_service_wire_status *status_out);

int mem_service_client_put_object(const struct mem_service_client *client,
                                  const struct mem_service_client_object *object,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out);
int mem_service_client_get_object(const struct mem_service_client *client,
                                  const char *key,
                                  struct mem_service_client_record *record_out,
                                  enum mem_service_wire_status *status_out);
int mem_service_client_inspect_object(const struct mem_service_client *client,
                                      const char *key,
                                      struct mem_service_client_record *record_out,
                                      enum mem_service_wire_status *status_out);
int mem_service_client_materialize_object(
    const struct mem_service_client *client,
    const char *key,
    const char *destination_path,
    bool has_expected_version,
    uint64_t expected_version,
    bool has_expected_checksum,
    uint64_t expected_checksum,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_register_prefix_entry(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_lookup_prefix_entry(
    const struct mem_service_client *client,
    const char *request_id,
    const char *prefix_group,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_block_entry *entry,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_kv_segment(
    const struct mem_service_client *client,
    const struct mem_service_client_kv_selector *selector,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_runtime_handoff(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_register_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_query_execution_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_register_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact *artifact,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_query_training_artifact(
    const struct mem_service_client *client,
    const struct mem_service_client_artifact_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_dataset_shard(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_sample_batch(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_checkpoint(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_gradient_bucket(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_publish_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_optimizer_state(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

int mem_service_client_commit_training_step(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref *ref,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_resolve_training_step(
    const struct mem_service_client *client,
    const struct mem_service_client_training_ref_query *query,
    struct mem_service_client_record *record_out,
    enum mem_service_wire_status *status_out);

/*
 * Managed object allocation control (M1.1, wire ops 0x70-0x75).
 *
 * These structures and functions are deliberately separate from the
 * 808-byte client record layout above: managed allocations carry object
 * identity, owner session, generation, content version, size/alignment/
 * capabilities, holder sessions, lifecycle state, provider incarnation
 * and an opaque descriptor length, none of which belongs to the legacy
 * record ABI. Mapping handles live in client processes and are never
 * part of this view.
 */
#define MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN 96U
#define MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN 64U
#define MEM_SERVICE_CLIENT_ALLOCATION_STATE_LEN 32U
#define MEM_SERVICE_CLIENT_ALLOCATION_MAX_HOLDERS 8U

#define MEM_SERVICE_CLIENT_MANAGED_CAP_MAP (1ULL << 0)
#define MEM_SERVICE_CLIENT_MANAGED_CAP_BLOCK_IO (1ULL << 1)

struct mem_service_client_allocate {
    const char *key;
    const char *idempotency_key;
    const char *session_id;
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint64_t capabilities;
};

struct mem_service_client_allocation_holder {
    char session_id[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    uint64_t generation;
};

struct mem_service_client_allocation {
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    char state[MEM_SERVICE_CLIENT_ALLOCATION_STATE_LEN];
    char owner_session[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    uint64_t generation;
    uint64_t version;
    uint64_t size_bytes;
    uint64_t alignment_bytes;
    uint64_t capabilities;
    uint32_t live_refs;
    uint64_t provider_incarnation;
    uint32_t descriptor_len;
    uint32_t holder_count;
    struct mem_service_client_allocation_holder
        holders[MEM_SERVICE_CLIENT_ALLOCATION_MAX_HOLDERS];
};

struct mem_service_client_allocation_stats {
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

int mem_service_client_allocate_object(
    const struct mem_service_client *client,
    const struct mem_service_client_allocate *request,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_acquire_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_release_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_retire_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_inspect_allocation(
    const struct mem_service_client *client,
    const char *key,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_allocation_stats(
    const struct mem_service_client *client,
    struct mem_service_client_allocation_stats *stats_out,
    enum mem_service_wire_status *status_out);

/*
 * Provider directory client (0x76 segment). A per-node provider process
 * registers its (node_id, incarnation) readiness after its bootstrap
 * canary, refreshes within the lease and deregisters on shutdown. The
 * returned view mirrors the control-plane directory summary; mapping
 * handles and payload bytes never cross these operations.
 */
#define MEM_SERVICE_CLIENT_PROVIDER_NODE_ID_LEN 64U

struct mem_service_client_provider_directory {
    uint64_t directory_epoch;
    uint64_t lease_ms;
    uint64_t required_count;
    uint64_t active_count;
    bool directory_ready;
    bool data_plane_ready;
    bool replaced;
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

int mem_service_client_provider_register(
    const struct mem_service_client *client,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    uint64_t capabilities,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_provider_refresh(
    const struct mem_service_client *client,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_provider_deregister(
    const struct mem_service_client *client,
    const char *node_id,
    uint64_t incarnation,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out);
int mem_service_client_provider_status(
    const struct mem_service_client *client,
    struct mem_service_client_provider_directory *view_out,
    enum mem_service_wire_status *status_out);

#endif
