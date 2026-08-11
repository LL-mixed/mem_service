#include "mem_service_provider.h"
#include "mem_service_provider_obmm.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFORMANCE_NODE_COUNT 2U
#define CONFORMANCE_REGION_BYTES (2U * 1024U * 1024U)
#define CONFORMANCE_VISIBLE_BYTES 4096U
#define CONFORMANCE_BARRIER_OFFSET CONFORMANCE_VISIBLE_BYTES
#define CONFORMANCE_TIMEOUT_MS 30000U

struct conformance_config {
    uint32_t node_id;
    uint32_t node_count;
    uint64_t generation;
};

static int parse_u64(const char *text, uint64_t *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || value_out == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static int parse_args(int argc,
                      char **argv,
                      struct conformance_config *config)
{
    uint64_t value;
    int i;

    if (config == NULL) {
        return -1;
    }
    memset(config, 0, sizeof(*config));
    config->node_count = CONFORMANCE_NODE_COUNT;
    config->generation = 1U;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &value) != 0 || value > UINT32_MAX) {
                return -1;
            }
            config->node_id = (uint32_t)value;
        } else if (strcmp(argv[i], "--node-count") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &value) != 0 || value > UINT32_MAX) {
                return -1;
            }
            config->node_count = (uint32_t)value;
        } else if (strcmp(argv[i], "--generation") == 0 && i + 1 < argc) {
            if (parse_u64(argv[++i], &config->generation) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return config->node_count == CONFORMANCE_NODE_COUNT &&
                   config->node_id < config->node_count &&
                   config->generation > 0 && config->generation < UINT64_MAX
               ? 0
               : -1;
}

static void fill_pattern(uint8_t *bytes, size_t len, uint8_t seed)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        bytes[i] = (uint8_t)(seed + i * 29U);
    }
}

static uint64_t pattern_checksum(uint8_t seed)
{
    uint8_t bytes[CONFORMANCE_VISIBLE_BYTES];

    fill_pattern(bytes, sizeof(bytes), seed);
    return mem_service_provider_checksum64(bytes, sizeof(bytes));
}

static int fail(const struct conformance_config *config, const char *stage)
{
    fprintf(stderr,
            "mem_service obmm-provider-conformance: status=fail node=%u "
            "stage=%s\n",
            config != NULL ? config->node_id : UINT32_MAX,
            stage != NULL ? stage : "unknown");
    return 1;
}

int main(int argc, char **argv)
{
    struct conformance_config config;
    struct mem_service_provider_obmm_config provider_config;
    struct mem_service_provider_obmm_endpoint endpoint = {0};
    struct mem_service_provider_registration registration;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_region canary_region;
    struct mem_service_provider_remote_region canary_remote;
    struct mem_service_provider_remote_region canary_regions[2];
    struct mem_service_provider_region_binding local_binding;
    struct mem_service_region_request region_request;
    struct mem_service_provider_remote_region local_remote;
    struct mem_service_provider_remote_region corrupted_remote;
    struct mem_service_provider_mapping_binding local_mapping;
    struct mem_service_provider_mapping_binding remote_mapping;
    struct mem_service_visibility_completion visibility;
    enum mem_service_provider_state state;
    uint8_t expected[CONFORMANCE_VISIBLE_BYTES];
    uint8_t canary_seed;
    uint8_t payload_seed;
    uint8_t peer_canary_seed;
    uint8_t peer_payload_seed;
    uint8_t barrier_seed;
    uint8_t peer_barrier_seed;
    uint64_t canary_checksum = 0;
    uint64_t barrier_checksum;
    uint64_t payload_checksum;
    uint64_t peer_barrier_checksum;
    uint64_t peer_canary_checksum;
    uint64_t peer_payload_checksum;
    uint32_t peer_node;
    bool endpoint_open = false;
    bool channel_bound = false;
    const char *failure_stage = "argument-parse";
    int rc = 1;

    if (parse_args(argc, argv, &config) != 0) {
        fprintf(stderr,
                "usage: %s --node-id <0|1> [--node-count 2] "
                "[--generation <n>]\n",
                argv[0]);
        return 2;
    }
    peer_node = config.node_id ^ 1U;
    canary_seed = (uint8_t)(17U + config.node_id * 41U);
    peer_canary_seed = (uint8_t)(17U + peer_node * 41U);
    payload_seed = (uint8_t)(101U + config.node_id * 37U);
    peer_payload_seed = (uint8_t)(101U + peer_node * 37U);
    barrier_seed = (uint8_t)(201U + config.node_id * 19U);
    peer_barrier_seed = (uint8_t)(201U + peer_node * 19U);
    peer_barrier_checksum = pattern_checksum(peer_barrier_seed);
    peer_canary_checksum = pattern_checksum(peer_canary_seed);
    peer_payload_checksum = pattern_checksum(peer_payload_seed);
    if (peer_barrier_checksum == 0 || peer_canary_checksum == 0 ||
        peer_payload_checksum == 0) {
        return fail(&config, "pattern-checksum");
    }
    memset(&provider_config, 0, sizeof(provider_config));
    provider_config.instance = config.node_id == 0 ? "node0" : "node1";
    provider_config.import_region_bytes = CONFORMANCE_REGION_BYTES;
    provider_config.max_remote_mappings = 2U;
    provider_config.required_peer_mappings = 1U;
    provider_config.force_osync = true;
    memset(&registration, 0, sizeof(registration));
    memset(&registry, 0, sizeof(registry));
    memset(&channel, 0, sizeof(channel));
    memset(&canary_region, 0, sizeof(canary_region));
    memset(&canary_remote, 0, sizeof(canary_remote));
    memset(&canary_regions, 0, sizeof(canary_regions));
    memset(&local_binding, 0, sizeof(local_binding));
    memset(&local_mapping, 0, sizeof(local_mapping));
    memset(&remote_mapping, 0, sizeof(remote_mapping));

    failure_stage = "endpoint-open";
    if (mem_service_provider_obmm_endpoint_open(
            &endpoint, &provider_config) != 0) {
        goto done;
    }
    endpoint_open = true;
    failure_stage = "initial-degraded-readiness";
    if (mem_service_provider_obmm_endpoint_registration(
            &endpoint, &registration) != 0 ||
        mem_service_provider_registry_init(&registry) != 0 ||
        mem_service_provider_registry_register(
            &registry, &registration) != 0 ||
        registry.providers[0].state != MEM_SERVICE_PROVIDER_STATE_DEGRADED ||
        mem_service_provider_registry_data_plane_ready(&registry) ||
        mem_service_provider_channel_bind(
            &registry,
            registration.name,
            registration.instance,
            MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
                MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
            &channel) == 0) {
        goto done;
    }
    printf("mem_service obmm-provider-conformance: node=%u "
           "stage=pre-canary readiness=degraded data_plane_ready=0\n",
           config.node_id);

    failure_stage = "canary-prepare";
    if (mem_service_provider_obmm_endpoint_prepare_canary_region(
            &endpoint,
            CONFORMANCE_REGION_BYTES,
            CONFORMANCE_VISIBLE_BYTES,
            canary_seed,
            &canary_region,
            &canary_remote,
            &canary_checksum) != 0 ||
        canary_checksum != pattern_checksum(canary_seed)) {
        goto done;
    }
    failure_stage = "canary-descriptor-exchange";
    if (mem_service_provider_obmm_endpoint_exchange_remote_regions(
            &endpoint,
            config.node_id,
            config.node_count,
            config.generation,
            &canary_remote,
            canary_regions,
            CONFORMANCE_NODE_COUNT) != 0) {
        goto done;
    }
    failure_stage = "peer-canary";
    if (mem_service_provider_obmm_endpoint_verify_mapping(
            &endpoint,
            &canary_regions[peer_node],
            0,
            CONFORMANCE_VISIBLE_BYTES,
            peer_canary_checksum,
            CONFORMANCE_TIMEOUT_MS) != 0 ||
        mem_service_provider_registry_refresh(&registry) != 0 ||
        !mem_service_provider_registry_data_plane_ready(&registry) ||
        registry.providers[0].ops->probe(
            registry.providers[0].context, &state) != 0 ||
        state != MEM_SERVICE_PROVIDER_STATE_READY) {
        goto done;
    }
    failure_stage = "neutral-channel-bind";
    if (mem_service_provider_channel_bind(
            &registry,
            registration.name,
            registration.instance,
            MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
                MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
            &channel) != 0) {
        goto done;
    }
    channel_bound = true;
    printf("mem_service obmm-provider-conformance: node=%u "
           "stage=post-canary readiness=ready data_plane_ready=1\n",
           config.node_id);

    failure_stage = "neutral-barrier-local-publish";
    if (mem_service_provider_channel_map_remote_region(
            &channel,
            &canary_remote,
            CONFORMANCE_BARRIER_OFFSET,
            CONFORMANCE_VISIBLE_BYTES,
            NULL,
            MEM_SERVICE_MAPPING_FLAG_READ |
                MEM_SERVICE_MAPPING_FLAG_WRITE,
            &local_mapping) != 0) {
        goto done;
    }
    fill_pattern(local_mapping.mapping.base,
                 CONFORMANCE_VISIBLE_BYTES,
                 barrier_seed);
    barrier_checksum = mem_service_provider_checksum64(
        local_mapping.mapping.base, CONFORMANCE_VISIBLE_BYTES);
    if (barrier_checksum == 0 ||
        mem_service_provider_channel_publish_range(
            &channel,
            &local_mapping,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            barrier_checksum,
            &visibility) != 0 ||
        mem_service_provider_channel_unmap_remote_region(
            &channel, &local_mapping) != 0) {
        goto done;
    }
    failure_stage = "neutral-barrier-remote-visible";
    if (mem_service_provider_channel_map_remote_region(
            &channel,
            &canary_regions[peer_node],
            CONFORMANCE_BARRIER_OFFSET,
            CONFORMANCE_VISIBLE_BYTES,
            NULL,
            MEM_SERVICE_MAPPING_FLAG_READ,
            &remote_mapping) != 0 ||
        mem_service_provider_channel_wait_range_visible(
            &channel,
            &remote_mapping,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            peer_barrier_checksum,
            CONFORMANCE_TIMEOUT_MS,
            &visibility) != 0 ||
        mem_service_provider_channel_unmap_remote_region(
            &channel, &remote_mapping) != 0) {
        goto done;
    }

    failure_stage = "neutral-register-export-deregister";
    memset(&region_request, 0, sizeof(region_request));
    region_request.len = CONFORMANCE_REGION_BYTES;
    region_request.memory_kind = MEM_SERVICE_MEMORY_HOST;
    region_request.flags = MEM_SERVICE_REGION_FLAG_PROVIDER_ALLOCATED;
    if (mem_service_provider_channel_register_region(
            &channel, &region_request, &local_binding) != 0 ||
        mem_service_provider_channel_export_region(
            &channel, &local_binding, &local_remote) != 0 ||
        mem_service_provider_channel_deregister_region(
            &channel, &local_binding) != 0) {
        goto done;
    }
    failure_stage = "neutral-local-map-publish";
    if (mem_service_provider_channel_map_remote_region(
            &channel,
            &canary_remote,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            NULL,
            MEM_SERVICE_MAPPING_FLAG_READ |
                MEM_SERVICE_MAPPING_FLAG_WRITE,
            &local_mapping) != 0) {
        goto done;
    }
    fill_pattern(local_mapping.mapping.base,
                 CONFORMANCE_VISIBLE_BYTES,
                 payload_seed);
    payload_checksum = mem_service_provider_checksum64(
        local_mapping.mapping.base, CONFORMANCE_VISIBLE_BYTES);
    if (payload_checksum == 0 ||
        mem_service_provider_channel_publish_range(
            &channel,
            &local_mapping,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            payload_checksum,
            &visibility) != 0 ||
        mem_service_provider_channel_unmap_remote_region(
            &channel, &local_mapping) != 0) {
        goto done;
    }
    failure_stage = "neutral-remote-map-visible";
    if (mem_service_provider_channel_map_remote_region(
            &channel,
            &canary_regions[peer_node],
            0,
            CONFORMANCE_VISIBLE_BYTES,
            NULL,
            MEM_SERVICE_MAPPING_FLAG_READ,
            &remote_mapping) != 0 ||
        mem_service_provider_channel_wait_range_visible(
            &channel,
            &remote_mapping,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            peer_payload_checksum,
            CONFORMANCE_TIMEOUT_MS,
            &visibility) != 0 ||
        mem_service_provider_channel_invalidate_range(
            &channel,
            &remote_mapping,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            peer_payload_checksum,
            &visibility) != 0) {
        goto done;
    }
    fill_pattern(expected, sizeof(expected), peer_payload_seed);
    if (memcmp(remote_mapping.mapping.base, expected, sizeof(expected)) != 0) {
        failure_stage = "neutral-mapped-payload-compare";
        goto done;
    }

    failure_stage = "fail-closed-bounds";
    if (mem_service_provider_channel_map_remote_region(
            &channel,
            &canary_regions[peer_node],
            canary_regions[peer_node].len - 1U,
            CONFORMANCE_VISIBLE_BYTES,
            NULL,
            MEM_SERVICE_MAPPING_FLAG_READ,
            &local_mapping) == 0) {
        goto done;
    }
    failure_stage = "fail-closed-corrupt-descriptor";
    corrupted_remote = canary_regions[peer_node];
    corrupted_remote.descriptor.bytes[4] ^= 0xffU;
    if (mem_service_provider_channel_map_remote_region(
            &channel,
            &corrupted_remote,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            NULL,
            MEM_SERVICE_MAPPING_FLAG_READ,
            &local_mapping) == 0) {
        goto done;
    }
    failure_stage = "fail-closed-checksum";
    if (mem_service_provider_channel_wait_range_visible(
            &channel,
            &remote_mapping,
            0,
            CONFORMANCE_VISIBLE_BYTES,
            peer_payload_checksum ^ 1U,
            10U,
            &visibility) == 0) {
        goto done;
    }
    failure_stage = "neutral-remote-unmap";
    if (mem_service_provider_channel_unmap_remote_region(
            &channel, &remote_mapping) != 0) {
        goto done;
    }
    rc = 0;

done:
    if (channel_bound && remote_mapping.mapped) {
        (void)mem_service_provider_channel_unmap_remote_region(
            &channel, &remote_mapping);
    }
    if (channel_bound && local_mapping.mapped) {
        (void)mem_service_provider_channel_unmap_remote_region(
            &channel, &local_mapping);
    }
    if (channel_bound && local_binding.registered) {
        (void)mem_service_provider_channel_deregister_region(
            &channel, &local_binding);
    }
    if (endpoint_open) {
        mem_service_provider_obmm_endpoint_close(&endpoint);
    }
    if (rc != 0) {
        return fail(&config, failure_stage);
    }
    printf("mem_service obmm-provider-conformance: status=ok node=%u "
           "readiness=degraded-to-ready registration=verified "
           "mapping=sim-dec publish=verified invalidate=verified "
           "visibility=checksum-verified bounds=fail-closed "
           "descriptor=fail-closed checksum=fail-closed cleanup=verified\n",
           config.node_id);
    return 0;
}
