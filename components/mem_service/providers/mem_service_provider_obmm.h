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
void mem_service_provider_obmm_endpoint_close(
    struct mem_service_provider_obmm_endpoint *endpoint);
int mem_service_provider_obmm_run_protocol_fixture(void);

#endif
