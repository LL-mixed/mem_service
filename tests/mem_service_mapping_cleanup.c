#include <stdio.h>
#include <string.h>
#include "components/mem_service/mem_service_client.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "cleanup check failed at line %d: %s\n", __LINE__, #condition); \
    return 1; } } while (0)

struct fault_state {
    unsigned mode, failures, maps, unmaps;
    bool ready;
};
static unsigned char bytes[32];

static int probe(void *opaque, enum mem_service_provider_state *state)
{
    struct fault_state *fault = opaque;
    *state = fault->ready ? MEM_SERVICE_PROVIDER_STATE_READY :
                           MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;
    return 0;
}

static int map(void *opaque, const struct mem_service_mapping_request *request,
               struct mem_service_mapping *mapping)
{
    struct fault_state *fault = opaque;
    ++fault->maps;
    mapping->handle = 991;
    mapping->base = fault->mode == 2 ? bytes + 1 : bytes;
    mapping->len = request->len + (fault->mode == 1);
    mapping->memory_kind = fault->mode == 3 ? MEM_SERVICE_MEMORY_ACCELERATOR :
                                           request->memory_kind;
    if (fault->mode == 5) mapping->base = NULL;
    return fault->mode == 4 ? -1 : 0;
}

static int unmap(void *opaque, uint64_t handle)
{
    struct fault_state *fault = opaque;
    ++fault->unmaps;
    if (handle != 991) return -1;
    if (fault->failures) {
        --fault->failures;
        return -1;
    }
    return 0;
}

int main(void)
{
    struct fault_state fault = {.ready = true};
    struct mem_service_provider_ops ops = {
        .probe = probe, .map_remote_region = map, .unmap_remote_region = unmap,
    };
    struct mem_service_provider provider = {
        .name = "cleanup-test", .instance = "one", .context = &fault,
        .capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING, .ops = &ops,
    };
    struct mem_service_provider_channel channel = {.provider = &provider};
    struct mem_service_client_allocation allocation = {
        .key = "cleanup-object", .state = "active", .generation = 7,
        .provider_backed = true, .capabilities = MEM_SERVICE_CLIENT_MANAGED_CAP_MAP,
        .address = (uint64_t)(uintptr_t)bytes, .size_bytes = 8,
        .address_len = sizeof(bytes), .descriptor_len = 1, .descriptor = {1},
    };
    struct mem_service_client_object_mapping mapping = {0};
    struct mem_service_visibility_completion completion;

    for (unsigned mode = 1; mode <= 5; ++mode) {
        fault = (struct fault_state){.mode = mode, .failures = 2, .ready = true};
        memset(&mapping, 0, sizeof(mapping));
        CHECK(mem_service_client_map_allocation(&channel, &allocation,
            MEM_SERVICE_CLIENT_MAP_READ, &mapping) == MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
        CHECK(fault.maps == 1 && fault.unmaps == 1);
        CHECK(mapping.binding.mapped && mapping.binding.owner == &provider);
        CHECK(mapping.binding.mapping.handle == 991);
        CHECK(!mapping.base && !mapping.len && !mapping.flags);
        CHECK(!mapping.binding.mapping.base && !mapping.binding.mapping.len);
        CHECK(mapping.generation == 7 && !strcmp(mapping.key, "cleanup-object"));
        CHECK(mem_service_provider_channel_publish_range(&channel, &mapping.binding,
            0, 1, 1, &completion) != 0);
        CHECK(mem_service_client_unmap_allocation(&channel, &mapping) != 0);
        CHECK(mapping.binding.mapped && fault.unmaps == 2);
        fault.ready = false;
        CHECK(mem_service_client_unmap_allocation(&channel, &mapping) == 0);
        CHECK(!mapping.binding.mapped && fault.unmaps == 3);
        CHECK(mem_service_client_unmap_allocation(&channel, &mapping) != 0);
        CHECK(fault.unmaps == 3);
    }

    fault = (struct fault_state){.mode = 1, .ready = true};
    memset(&mapping, 0, sizeof(mapping));
    CHECK(mem_service_client_map_allocation(&channel, &allocation,
        MEM_SERVICE_CLIENT_MAP_READ, &mapping) == -1);
    CHECK(fault.unmaps == 1 && !mapping.binding.mapped);

    fault = (struct fault_state){.ready = true, .failures = 1};
    CHECK(mem_service_client_map_allocation(&channel, &allocation,
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE, &mapping) == 0);
    CHECK(mapping.base == bytes && mapping.flags && mapping.len == 8);
    CHECK(mem_service_client_unmap_allocation(&channel, &mapping) != 0);
    CHECK(!mapping.base && !mapping.len && !mapping.flags);
    CHECK(mapping.binding.mapped && mapping.binding.mapping.handle == 991);
    CHECK(!mapping.binding.mapping.base && !mapping.binding.mapping.len);
    CHECK(mem_service_client_unmap_allocation(&channel, &mapping) == 0);

    fault = (struct fault_state){.ready = true};
    ops.unmap_remote_region = NULL;
    CHECK(mem_service_client_map_allocation(&channel, &allocation,
        MEM_SERVICE_CLIENT_MAP_READ, &mapping) != 0);
    CHECK(!fault.maps && !mapping.binding.mapped);
    puts("mapping_cleanup_regression=pass");
    return 0;
}
