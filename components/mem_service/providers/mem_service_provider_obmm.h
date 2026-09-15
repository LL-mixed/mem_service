#ifndef MEM_SERVICE_PROVIDER_OBMM_H
#define MEM_SERVICE_PROVIDER_OBMM_H

#include "../mem_service_provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS 8U

struct mem_service_provider_obmm_config {
    const char *instance;
    const char *device_path;
    const char *primary_cna_path;
    uint64_t import_region_bytes;
    uint64_t import_pa_bias;
    uint32_t max_remote_mappings;
    uint32_t required_peer_mappings;
    bool force_osync;
};

struct mem_service_provider_obmm_endpoint {
    void *implementation;
};

/* Platform-private attachment to an already mapped strict import. Serialize
 * with all endpoint operations. The fields are borrowed until pin release;
 * callers must never close the fd or unmap this view directly. Initialize
 * *pin_out to NULL; acquire errors return no new pin or usable view. Rights
 * use MEM_SERVICE_MAPPING_FLAG_READ/WRITE, without FIXED_ADDRESS. */
struct mem_service_provider_obmm_mapping_pin;
struct mem_service_provider_obmm_pinned_mapping {
    int obmm_fd;
    uint64_t mem_id;
    void *base;
    uint64_t len;
    uint64_t access_flags;
};
int mem_service_provider_obmm_mapping_pin_acquire(
    const struct mem_service_provider_mapping_binding *binding,
    uint64_t access_flags,
    struct mem_service_provider_obmm_mapping_pin **pin_out,
    struct mem_service_provider_obmm_pinned_mapping *view_out);
/* Release only after execution has drained and registration is unregistered.
 * Success clears *pin; failure retains it. Allowed while endpoint is closing. */
int mem_service_provider_obmm_mapping_pin_release(
    struct mem_service_provider_obmm_mapping_pin **pin);

/* Current endpoint ownership only; serialize with all endpoint operations.
 * Remaining resources after failed cleanup stay counted. Not a kernel census.
 */
struct mem_service_provider_obmm_resources_v1 {
    uint64_t export_handles, export_bytes;
    uint64_t import_handles, import_bytes;
    uint64_t vma_count, vma_bytes;
    uint64_t accessible_views, accessible_bytes;
    uint64_t cleanup_mappings;
    bool closing, control_close_uncertain;
};
int mem_service_provider_obmm_endpoint_resources_v1(
    const struct mem_service_provider_obmm_endpoint *endpoint,
    struct mem_service_provider_obmm_resources_v1 *resources_out);

/* Diagnostic only; serialize with endpoint operations. Succeeds only on an
 * actual EEXIST using an already-owned strict mapping's fd, without import.
 * Unexpected VMAs remain owned until cleanup; cleanup failure closes admission.
 */
int mem_service_provider_obmm_endpoint_probe_conflict(
    struct mem_service_provider_obmm_endpoint *endpoint, uint64_t mapping_handle);

/* Diagnostic only; serialize with endpoint operations. Submit only malformed
 * copies of an owned strict descriptor. Unexpected resources remain owned and
 * close admission. Does not certify rejection of well-formed stale identities.
 */
int mem_service_provider_obmm_endpoint_probe_descriptor(
    struct mem_service_provider_obmm_endpoint *endpoint, uint64_t mapping_handle);

struct obmm_gsva_segment_desc_v1;
struct obmm_cmd_export;
int mem_service_provider_obmm_encode_gsva(
    const struct obmm_gsva_segment_desc_v1 *segment,
    const struct obmm_cmd_export *exported,
    struct mem_service_provider_descriptor *descriptor_out);

int mem_service_provider_obmm_probe_device(const char *device_path,
                                           const char *primary_cna_path,
                                           char *detail,
                                           size_t detail_len);
int mem_service_provider_obmm_endpoint_open(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_obmm_config *config);
int mem_service_provider_obmm_endpoint_registration(
    struct mem_service_provider_obmm_endpoint *endpoint,
    struct mem_service_provider_registration *registration_out);
int mem_service_provider_obmm_endpoint_create_region(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint64_t len,
    struct mem_service_region *region_out);
int mem_service_provider_obmm_endpoint_prepare_canary_region(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint64_t region_len,
    uint64_t visible_len,
    uint8_t seed,
    struct mem_service_region *region_out,
    struct mem_service_provider_remote_region *remote_out,
    uint64_t *checksum_out);
int mem_service_provider_obmm_endpoint_exchange_remote_regions(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint32_t local_node,
    uint32_t node_count,
    uint64_t generation,
    const struct mem_service_provider_remote_region *local,
    struct mem_service_provider_remote_region *regions_out,
    size_t region_capacity);
int mem_service_provider_obmm_endpoint_verify_mapping(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_remote_region *remote,
    uint64_t offset,
    uint64_t len,
    uint64_t expected_checksum,
    uint64_t timeout_ms);
/* Failure retains implementation for cleanup/reconciliation; no new work is
 * admitted once close starts. A NULL/already-closed endpoint succeeds. */
int mem_service_provider_obmm_endpoint_close_checked(
    struct mem_service_provider_obmm_endpoint *endpoint);
void mem_service_provider_obmm_endpoint_close(
    struct mem_service_provider_obmm_endpoint *endpoint);
int mem_service_provider_obmm_run_protocol_fixture(void);
#ifdef MEM_SERVICE_OBMM_MANAGED_WORKER
int mem_service_provider_obmm_serve_allocations(const char *config_path);
int mem_service_provider_obmm_inspect_allocation_state(const char *config_path);
int mem_service_provider_obmm_reconcile_allocation_state(const char *config_path);
#endif

#endif
