#include "components/mem_service/mem_service_client.h"
#include "components/mem_service/mem_service_wire_payload.h"
#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "managed mapping line %d: %s\n", __LINE__, #value); \
    return 1; } } while (0)

static struct {
    unsigned state, maps, unmaps, unmap_failures, map_failures, snapshots;
    unsigned fail_before, fail_after;
    bool ready, timeout, snapshot_failure, admission_rejected, seen[7];
    bool reference_mode;
    unsigned replies[7];
    char keys[7][96];
} peer;
static unsigned char bytes[32];
static struct mem_service_client_allocation authoritative;
static struct lingqu_object_ref_wire_v2 reference;

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
    bool reference_begin = operation == MEM_SERVICE_WIRE_OP_REFERENCE_TRANSITION;
    if (reference_begin) {
        struct mem_service_reference_request decoded;
        if (!peer.reference_mode || mem_service_reference_parse_request(payload_in, &decoded) ||
            decoded.action != MEM_SERVICE_REFERENCE_MAP_BEGIN ||
            memcmp(&decoded.reference, &reference, sizeof(reference))) return 1;
        action = MEM_SERVICE_CLIENT_MAPPING_BEGIN;
    }
    if (operation == MEM_SERVICE_WIRE_OP_INSPECT_ALLOCATION) {
        char hex[MEM_SERVICE_CLIENT_ALLOCATION_DESCRIPTOR_MAX_LEN * 2 + 1];
        ++peer.snapshots;
        if (peer.snapshot_failure) return -1;
        for (unsigned i = 0; i < authoritative.descriptor_len; ++i)
            snprintf(hex + i * 2, 3, "%02x", authoritative.descriptor[i]);
        snprintf(payload_out, payload_out_len,
            "key=object\nstate=active\ngeneration=7\nsize_bytes=8\n"
            "alignment_bytes=4096\ncapabilities=%llu\nhome_node=home\n"
            "provider_incarnation=9\nprovider_backed=1\naddress=%llu\n"
            "address_len=%zu\ndescriptor_hex=%s\n",
            (unsigned long long)authoritative.capabilities,
            (unsigned long long)(uintptr_t)bytes, sizeof(bytes), hex);
        *status_out = MEM_SERVICE_WIRE_STATUS_OK;
        return 0;
    }
    if ((!reference_begin && operation != MEM_SERVICE_WIRE_OP_MAPPING_TRANSITION) ||
        action < 1 || action > 6 ||
        !mem_service_wire_payload_get_string(&view, "idempotency_key", key, sizeof(key)))
        return 1;
    if (action == MEM_SERVICE_CLIENT_MAPPING_BEGIN && options->max_attempts != 1)
        return 1;
    if (action == MEM_SERVICE_CLIENT_MAPPING_BEGIN && peer.admission_rejected) {
        *status_out = MEM_SERVICE_WIRE_STATUS_STALE_REF;
        return 1;
    }
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
    if (reference_begin) {
        char hex[513], descriptor[193];
        if (mem_service_reference_encode_hex(&reference, hex, sizeof(hex))) return 1;
        for (unsigned i = 0; i < authoritative.descriptor_len; ++i)
            snprintf(descriptor + 2 * i, 3, "%02x", authoritative.descriptor[i]);
        snprintf(payload_out + strlen(payload_out), payload_out_len - strlen(payload_out),
            "status=ok\nstate=active\naction=6\nreference_key=object\nreference_hex=%s\n"
            "version=2\nsize_bytes=8\nalignment_bytes=4096\ncapabilities=%llu\n"
            "live_refs=1\nhome_node=home\nprovider_incarnation=9\nprovider_backed=1\n"
            "address=%llu\naddress_len=%zu\ndescriptor_len=96\ndescriptor_hex=%s\n",
            hex, (unsigned long long)authoritative.capabilities,
            (unsigned long long)(uintptr_t)bytes, sizeof(bytes), descriptor);
    }
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
    if (peer.reference_mode && (request->offset != 3 || request->len != 4 ||
        request->requested_address != bytes + 3 ||
        request->flags != (MEM_SERVICE_MAPPING_FLAG_READ | MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS)))
        return -1;
    ++peer.maps;
    if (peer.map_failures) { --peer.map_failures; return -1; }
    mapping->handle = 991;
    mapping->base = bytes + request->offset;
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
        .address_len = sizeof(bytes), .descriptor_len = 96, .descriptor = {1},
        .alignment_bytes = 4096, .home_node = "home", .provider_incarnation = 9,
    };
    authoritative = allocation;
    reference = (struct lingqu_object_ref_wire_v2){
        .object = {.magic = LINGQU_OBJECT_REF_MAGIC, .layout_version = 2, .object_kind = 5,
            .state = LINGQU_OBJECT_STATE_COMMITTED_WIRE, .object_version = 2,
            .payload_offset = 3, .payload_bytes = 4},
        .wire_bytes = 256, .access = 1, .allocation_generation = 7,
        .provider_incarnation = 9, .allocation_bytes = 8,
        .allocation_key = "object", .home_node = "home",
    };
    reference.object.key_hash = lingqu_object_ref_key_hash("object", 6);
    mem_service_client_init(&client, "unix:/unused");
    client.wire_options.max_attempts = 3;
    for (unsigned mode = 0; mode < 2; ++mode) {
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        struct mem_service_client_object_mapping mapping = {0};
        struct mem_service_client_mapping_lifecycle lifecycle = {0};
        struct mem_service_client_reference_lifecycle reference_lifecycle = {0};
        struct mem_service_client_mapping_lifecycle *active = mode ?
            &reference_lifecycle.mapping : &lifecycle;
        enum mem_service_wire_status status;
        int rc;
        memset(&peer, 0, sizeof(peer));
        peer.ready = true;
        peer.reference_mode = mode != 0;
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
        rc = mode ? mem_service_client_map_managed_reference(&client, &channel,
            &reference, "owner", "fresh-operation", &mapping, &reference_lifecycle, &status) :
            mem_service_client_map_managed_allocation(&client, &channel,
            &allocation, "owner", "fresh-operation", MEM_SERVICE_CLIENT_MAP_READ,
            &mapping, &lifecycle, &status);
        if (scenario <= 3 || scenario == 8) {
            CHECK(rc != 0 && !mapping.base && !mapping.flags);
            if (scenario <= 1 || scenario == 8)
                CHECK(peer.maps == 0 && active->pending);
            if (scenario == 2) CHECK(!active->pending && peer.state == 0);
        } else {
            CHECK(rc == 0 && mapping.base == bytes + (mode ? 3 : 0) && peer.state == 2);
            CHECK(mapping.len == (mode ? 4 : 8));
            if (scenario == 4) peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_CLOSE;
            if (scenario == 5) peer.fail_after = MEM_SERVICE_CLIENT_MAPPING_FINISH;
            if (scenario == 6) peer.fail_before = MEM_SERVICE_CLIENT_MAPPING_INSPECT;
            if (scenario == 7) peer.unmap_failures = 1;
            rc = mode ? mem_service_client_unmap_managed_reference(&client, &channel,
                &mapping, &reference_lifecycle, &status) :
                mem_service_client_unmap_managed_allocation(&client, &channel, &mapping, &lifecycle, &status);
            CHECK(rc == MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
            CHECK(active->pending && !mapping.base && !mapping.flags);
            CHECK(mapping.binding.mapped == (scenario != 5));
            peer.ready = false;
        }
        if (active->pending) {
            rc = mode ? mem_service_client_unmap_managed_reference(&client, &channel,
                &mapping, &reference_lifecycle, &status) :
                mem_service_client_unmap_managed_allocation(&client, &channel, &mapping, &lifecycle, &status);
            CHECK(rc == 0);
        }
        CHECK(!active->pending && !mapping.binding.mapped && peer.state == 0);
        CHECK(peer.maps <= 1 && peer.unmaps <= 2);
    }
    }
    puts("reference_mapping_faults=pass scenarios=9 exact_subrange=1 cleanup=confirmed");
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
    for (unsigned mutation = 0; mutation < allocation.descriptor_len + 12; ++mutation) {
        struct mem_service_client_allocation changed = allocation;
        struct mem_service_client_object_mapping mapping = {0};
        struct mem_service_client_mapping_lifecycle lifecycle = {0};
        enum mem_service_wire_status status;
        memset(&peer, 0, sizeof(peer));
        peer.ready = true;
        if (mutation < allocation.descriptor_len) changed.descriptor[mutation] ^= 1;
        else switch (mutation - allocation.descriptor_len) {
        case 0: --changed.descriptor_len; break;
        case 1: changed.descriptor_len = sizeof(changed.descriptor) + 1; break;
        case 2: changed.address += 4096; break;
        case 3: ++changed.address_len; break;
        case 4: ++changed.size_bytes; break;
        case 5: ++changed.alignment_bytes; break;
        case 6: changed.capabilities ^= 1024; break;
        case 7: changed.home_node[0] = 'x'; break;
        case 8: ++changed.provider_incarnation; break;
        case 9: changed.provider_backed = false; break;
        case 10: snprintf(changed.state, sizeof(changed.state), "retiring"); break;
        case 11: memset(changed.home_node, 'x', sizeof(changed.home_node)); break;
        }
        int rc = mem_service_client_map_managed_allocation(&client, &channel,
            &changed, "owner", "binding-check", MEM_SERVICE_CLIENT_MAP_READ,
            &mapping, &lifecycle, &status);
        if (rc != -1) fprintf(stderr, "binding mutation=%u rc=%d provider_maps=%u "
            "snapshots=%u state=%u\n", mutation, rc, peer.maps, peer.snapshots, peer.state);
        CHECK(rc == -1);
        CHECK(status == MEM_SERVICE_WIRE_STATUS_STALE_REF);
        CHECK(!peer.maps && peer.snapshots == 1 && peer.state == 0);
        CHECK(!peer.seen[MEM_SERVICE_CLIENT_MAPPING_BEGIN]);
        CHECK(!lifecycle.pending && !mapping.binding.mapped && !mapping.base);
    }
    {
        struct mem_service_client_allocation changed = allocation;
        struct mem_service_client_object_mapping mapping = {0};
        struct mem_service_client_mapping_lifecycle lifecycle = {0};
        enum mem_service_wire_status status;
        memset(&peer, 0, sizeof(peer));
        peer.ready = true;
        changed.version = 99;
        changed.live_refs = 8;
        changed.holder_count = 7;
        CHECK(!mem_service_client_map_managed_allocation(&client, &channel,
            &changed, "owner", "mutable-counts", MEM_SERVICE_CLIENT_MAP_READ,
            &mapping, &lifecycle, &status));
        CHECK(peer.maps == 1 && peer.snapshots == 1);
        CHECK(!mem_service_client_unmap_managed_allocation(&client, &channel,
            &mapping, &lifecycle, &status));
    }
    for (unsigned failure = 0; failure < 2; ++failure) {
        struct mem_service_client_object_mapping mapping = {0};
        struct mem_service_client_mapping_lifecycle lifecycle = {0};
        enum mem_service_wire_status status;
        memset(&peer, 0, sizeof(peer));
        peer.ready = true;
        peer.snapshot_failure = failure == 0;
        /* A concurrent retire/generation change after a valid snapshot is
         * still rejected by BEGIN, before any provider mapping is created. */
        peer.admission_rejected = failure == 1;
        CHECK(mem_service_client_map_managed_allocation(&client, &channel,
            &allocation, "owner", "snapshot-failure", MEM_SERVICE_CLIENT_MAP_READ,
            &mapping, &lifecycle, &status) != 0);
        CHECK(!peer.maps && peer.snapshots == 1 && !mapping.base);
        CHECK(status == (failure ? MEM_SERVICE_WIRE_STATUS_STALE_REF :
                                  MEM_SERVICE_WIRE_STATUS_INTERNAL));
        CHECK(!peer.seen[MEM_SERVICE_CLIENT_MAPPING_BEGIN]);
        CHECK(!lifecycle.pending && peer.state == 0);
    }
    puts("managed_binding_integrity=pass mutations=108 preflight_failures=2");
    puts("managed_mapping_faults=pass scenarios=11");
    return 0;
}
