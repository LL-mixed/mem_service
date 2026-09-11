#include "components/mem_service/mem_service_client.h"
#include "components/mem_service/mem_service_wire_payload.h"
#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "managed mapping line %d: %s\n", __LINE__, #value); \
    return 1; } } while (0)

static struct {
    unsigned state, maps, unmaps, unmap_failures, map_failures;
    unsigned fail_before, fail_after;
    bool ready, timeout, seen[7];
    unsigned replies[7];
    char keys[7][96];
} peer;
static unsigned char bytes[32];

int __wrap_mem_service_send_request_with_options(
    const char *connect_spec, const struct mem_service_wire_client_options *options,
    enum mem_service_wire_operation operation, const char *payload_in,
    char *payload_out, size_t payload_out_len,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_wire_payload_view view =
        mem_service_wire_payload_view_from_cstr(payload_in);
    unsigned action = mem_service_wire_payload_get_u64(&view, "action", 0);
    char key[96];
    unsigned reply;
    (void)connect_spec;
    *status_out = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    if (operation != MEM_SERVICE_WIRE_OP_MAPPING_TRANSITION ||
        action < 1 || action > 6 ||
        !mem_service_wire_payload_get_string(&view, "idempotency_key", key, sizeof(key)))
        return 1;
    if (action == MEM_SERVICE_CLIENT_MAPPING_BEGIN && options->max_attempts != 1)
        return 1;
    if (peer.fail_before == action) {
        peer.fail_before = 0;
        return -1;
    }
    if (peer.seen[action] && strcmp(peer.keys[action], key)) return 1;
    if (action == MEM_SERVICE_CLIENT_MAPPING_INSPECT) {
        reply = peer.state;
    } else if (peer.seen[action]) {
        reply = peer.replies[action];
    } else {
        if (action == 1 && peer.state == 0) peer.state = 1;
        else if (action == 2 && peer.state == 1) peer.state = 2;
        else if (action == 3 && peer.state == 2) peer.state = 3;
        else if (action == 4 && peer.state == 3) peer.state = 0;
        else if (action == 5 && peer.state == 1) peer.state = 0;
        else return 1;
        reply = peer.state;
        peer.seen[action] = true;
        peer.replies[action] = reply;
        snprintf(peer.keys[action], sizeof(peer.keys[action]), "%s", key);
    }
    if (peer.fail_after == action) {
        peer.fail_after = 0;
        if (peer.timeout) *status_out = MEM_SERVICE_WIRE_STATUS_TIMEOUT;
        return 1;
    }
    snprintf(payload_out, payload_out_len,
        "key=object\nsession_id=owner\ngeneration=7\nmapping_id=11\nmapping_state=%u\n", reply);
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

static int probe(void *context, enum mem_service_provider_state *state)
{
    (void)context;
    *state = peer.ready ? MEM_SERVICE_PROVIDER_STATE_READY :
                          MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;
    return 0;
}

static int map(void *context, const struct mem_service_mapping_request *request,
               struct mem_service_mapping *mapping)
{
    (void)context;
    if (peer.state != 1) return -1;
    ++peer.maps;
    if (peer.map_failures) { --peer.map_failures; return -1; }
    mapping->handle = 991;
    mapping->base = bytes;
    mapping->len = request->len;
    mapping->memory_kind = request->memory_kind;
    return 0;
}

static int unmap(void *context, uint64_t handle)
{
    (void)context;
    if (handle != 991 || (peer.state != 1 && peer.state != 3)) return -1;
    ++peer.unmaps;
    if (peer.unmap_failures) { --peer.unmap_failures; return -1; }
    return 0;
}

int main(void)
{
    struct mem_service_provider_ops ops = {
        .probe = probe, .map_remote_region = map, .unmap_remote_region = unmap,
    };
    struct mem_service_provider provider = {
        .name = "managed-test", .instance = "one", .ops = &ops,
        .capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
    };
    struct mem_service_provider_channel channel = {.provider = &provider};
    struct mem_service_client client;
    struct mem_service_client_allocation allocation = {
        .key = "object", .state = "active", .generation = 7,
        .provider_backed = true, .capabilities = MEM_SERVICE_CLIENT_MANAGED_CAP_MAP,
        .address = (uint64_t)(uintptr_t)bytes, .size_bytes = 8,
        .address_len = sizeof(bytes), .descriptor_len = 1, .descriptor = {1},
    };
    mem_service_client_init(&client, "unix:/unused");
    client.wire_options.max_attempts = 3;
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        struct mem_service_client_object_mapping mapping = {0};
        struct mem_service_client_mapping_lifecycle lifecycle = {0};
        enum mem_service_wire_status status;
        int rc;
        memset(&peer, 0, sizeof(peer));
        peer.ready = true;
        if (scenario == 0) peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_BEGIN;
        if (scenario == 8) {
            peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_BEGIN;
            peer.timeout = true;
        }
        if (scenario == 1) peer.fail_before = MEM_SERVICE_CLIENT_MAPPING_BEGIN;
        if (scenario == 2) peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_CONFIRM;
        if (scenario == 3) {
            peer.map_failures = 1;
            peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_CANCEL;
        }
        rc = mem_service_client_map_managed_allocation(&client, &channel,
            &allocation, "owner", "fresh-operation", MEM_SERVICE_CLIENT_MAP_READ,
            &mapping, &lifecycle, &status);
        if (scenario <= 3 || scenario == 8) {
            CHECK(rc != 0 && !mapping.base && !mapping.flags);
            if (scenario <= 1 || scenario == 8)
                CHECK(peer.maps == 0 && lifecycle.pending);
            if (scenario == 2) CHECK(!lifecycle.pending && peer.state == 0);
        } else {
            CHECK(rc == 0 && mapping.base == bytes && peer.state == 2);
            if (scenario == 4) peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_CLOSE;
            if (scenario == 5) peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_FINISH;
            if (scenario == 6) peer.fail_before = MEM_SERVICE_CLIENT_MAPPING_INSPECT;
            if (scenario == 7) peer.unmap_failures = 1;
            CHECK(mem_service_client_unmap_managed_allocation(&client, &channel,
                &mapping, &lifecycle, &status) == MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
            CHECK(lifecycle.pending && !mapping.base && !mapping.flags);
            CHECK(mapping.binding.mapped == (scenario != 5));
            peer.ready = false;
        }
        if (lifecycle.pending) {
            CHECK(mem_service_client_unmap_managed_allocation(&client, &channel,
                &mapping, &lifecycle, &status) == 0);
        }
        CHECK(!lifecycle.pending && !mapping.binding.mapped && peer.state == 0);
        CHECK(peer.maps <= 1 && peer.unmaps <= 2);
    }
    for (unsigned state = 2; state <= 3; ++state) {
        struct mem_service_client_object_mapping mapping = {0};
        struct mem_service_client_mapping_lifecycle lifecycle = {0};
        enum mem_service_wire_status status;
        memset(&peer, 0, sizeof(peer));
        peer.ready = true;
        peer.state = state;
        peer.seen[1] = true;
        peer.replies[1] = 1;
        snprintf(peer.keys[1], sizeof(peer.keys[1]), "reused-operation-1");
        CHECK(mem_service_client_map_managed_allocation(&client, &channel,
            &allocation, "owner", "reused-operation", MEM_SERVICE_CLIENT_MAP_READ,
            &mapping, &lifecycle, &status) == MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
        CHECK(lifecycle.pending && !mapping.binding.mapped && !peer.maps);
        CHECK(mem_service_client_unmap_managed_allocation(&client, &channel,
            &mapping, &lifecycle, &status) == MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
        CHECK(peer.state == state && !peer.unmaps && !peer.seen[3] && !peer.seen[4]);
    }
    puts("managed_mapping_faults=pass scenarios=11");
    return 0;
}
