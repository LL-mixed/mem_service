/* SPDX-License-Identifier: MIT */
#include "mem_service_client.h"
#include <assert.h>

static char reply[4096];
static unsigned calls;
static int transport_rc;
static bool writer_mode;
static unsigned publish_calls;
static unsigned mapping_state = 2;
static int publish_failure;
static char allocation_reply[2048];

static int writer_probe(void *context, enum mem_service_provider_state *state)
{
    (void)context;
    *state = MEM_SERVICE_PROVIDER_STATE_READY;
    return 0;
}

static int writer_publish(void *context, const struct mem_service_mapping_range_request *request,
                          struct mem_service_visibility_completion *completion)
{
    ++publish_calls;
    assert(request->mapping_handle == 9 && request->offset == 128 && request->len == 512);
    completion->status = 0;
    completion->visible_bytes = request->len;
    completion->checksum = mem_service_provider_checksum64((char *)context + request->offset, request->len);
    if (publish_failure == 2) ++completion->checksum;
    return publish_failure == 1 ? -1 : 0;
}

void mem_service_wire_client_options_init(struct mem_service_wire_client_options *options)
{
    memset(options, 0, sizeof(*options));
}

int mem_service_send_request_with_options(
    const char *connect_spec, const struct mem_service_wire_client_options *options,
    enum mem_service_wire_operation operation, const char *payload_in,
    char *payload_out, size_t payload_out_len, enum mem_service_wire_status *status_out)
{
    struct mem_service_reference_request decoded;
    (void)connect_spec; (void)options;
    if (writer_mode) {
        ++calls;
        if (operation == MEM_SERVICE_WIRE_OP_INSPECT_ALLOCATION) {
            snprintf(payload_out, payload_out_len, "%s", allocation_reply);
        } else {
            assert(operation == MEM_SERVICE_WIRE_OP_MAPPING_TRANSITION);
            assert(strstr(payload_in, "action=6\n"));
            snprintf(payload_out, payload_out_len,
                "key=allocation-1\nsession_id=owner\ngeneration=1\nmapping_id=11\nmapping_state=%u\n",
                mapping_state);
        }
        *status_out = transport_rc ? MEM_SERVICE_WIRE_STATUS_UNSUPPORTED : MEM_SERVICE_WIRE_STATUS_OK;
        return transport_rc;
    }
    assert(operation == MEM_SERVICE_WIRE_OP_REFERENCE_TRANSITION);
    assert(!mem_service_reference_parse_request(payload_in, &decoded));
    ++calls;
    assert(strlen(reply) < payload_out_len);
    strcpy(payload_out, reply);
    *status_out = transport_rc ? MEM_SERVICE_WIRE_STATUS_UNSUPPORTED : MEM_SERVICE_WIRE_STATUS_OK;
    return transport_rc;
}

static void self_test(void)
{
    const char *valid = "status=ok\nstate=active\nkey=allocation-1\nreference_key=allocation-1\n"
        "action=1\ngeneration=1\nversion=2\nsize_bytes=4096\nalignment_bytes=4096\n"
        "capabilities=1\nlive_refs=1\nhome_node=home-1\nprovider_incarnation=7\n"
        "provider_backed=1\naddress=1048576\naddress_len=4096\ndescriptor_len=4\n"
        "descriptor_hex=01020304\n";
    struct mem_service_client client = {0};
    struct mem_service_reference_request request = {0};
    struct mem_service_client_reference_result result, saved;
    enum mem_service_wire_status status;
    request.action = MEM_SERVICE_REFERENCE_BEGIN;
    request.generation = 1; request.version = 1;
    strcpy(request.key, "allocation-1"); strcpy(request.session_id, "owner");
    strcpy(request.idempotency_key, "nonce");
    strcpy(reply, valid);
    assert(!mem_service_client_reference_transition(&client, &request, &result, &status));
    assert(result.allocation.version == 2 && result.allocation.descriptor_len == 4);
    memset(&result, 0xa5, sizeof(result)); saved = result;
    const char *bad[] = {"key=allocation-1\n", "generation=1\n", "version=2\n",
        "reference_key=allocation-1\n", "action=1\n", "provider_backed=1\n",
        "home_node=home-1\n", "descriptor_hex=01020304\n", "=bad\n", "broken\n"};
    unsigned rejected = 0;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        snprintf(reply, sizeof(reply), "%s%s", valid, bad[i]);
        assert(mem_service_client_reference_transition(&client, &request, &result, &status));
        assert(status == MEM_SERVICE_WIRE_STATUS_INTERNAL && !memcmp(&saved, &result, sizeof(result)));
        ++rejected;
    }
    const char *names[] = {"generation=1", "version=2", "action=1", "provider_backed=1",
        "descriptor_len=4", "key=allocation-1", "state=active"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        strcpy(reply, valid);
        char *field = strstr(reply, names[i]); assert(field);
        char *equal = strchr(field, '='); assert(equal); equal[1] = '-';
        assert(mem_service_client_reference_transition(&client, &request, &result, &status));
        assert(!memcmp(&saved, &result, sizeof(result))); ++rejected;
    }
    strcpy(reply, valid); transport_rc = 1;
    assert(mem_service_client_reference_transition(&client, &request, &result, &status) == 1);
    assert(status == MEM_SERVICE_WIRE_STATUS_UNSUPPORTED && !memcmp(&saved, &result, sizeof(result)));
    unsigned before = calls;
    request.version = 0;
    assert(mem_service_client_reference_transition(&client, &request, &result, &status) == 2);
    assert(calls == before && !memcmp(&saved, &result, sizeof(result)));
    printf("reference_client=pass malformed_replies=%u output_preserved=1 scope=mock-wire\n", rejected);

    transport_rc = 0;
    request.action = MEM_SERVICE_REFERENCE_MAP_BEGIN;
    request.access = 1;
    request.reference = (struct lingqu_object_ref_wire_v2){
        .object = {.magic = LINGQU_OBJECT_REF_MAGIC, .layout_version = 2, .object_kind = 5,
            .state = LINGQU_OBJECT_STATE_COMMITTED_WIRE, .object_version = 2,
            .payload_offset = 128, .payload_bytes = 512},
        .wire_bytes = 256, .access = 1, .allocation_generation = 1,
        .provider_incarnation = 7, .allocation_bytes = 4096,
        .allocation_key = "allocation-1", .home_node = "home-1",
    };
    request.reference.object.key_hash = lingqu_object_ref_key_hash("allocation-1", 12);
    char hex[513], valid_mapping[4096];
    assert(!mem_service_reference_encode_hex(&request.reference, hex, sizeof(hex)));
    snprintf(valid_mapping, sizeof(valid_mapping),
        "%sreference_hex=%s\nsession_id=owner\nmapping_id=11\nmapping_state=1\n", valid, hex);
    strstr(valid_mapping, "action=1")[7] = '6';
    struct mem_service_client_mapping_transaction transaction, saved_transaction;
    strcpy(reply, valid_mapping);
    assert(!mem_service_client_reference_map_begin(&client, &request, &result, &transaction, &status));
    assert(transaction.mapping_id == 11 && transaction.state == 1 && transaction.generation == 1);
    memset(&result, 0xa5, sizeof(result)); saved = result;
    memset(&transaction, 0xa5, sizeof(transaction)); saved_transaction = transaction;
    const char *mapping_fields[] = {"mapping_id=11", "mapping_state=1", "session_id=owner"};
    for (unsigned i = 0; i < 6; ++i) {
        strcpy(reply, valid_mapping);
        if (i < 3) {
            char *field = strstr(reply, mapping_fields[i]);
            strchr(field, '=')[1] = i == 0 ? '-' : '0';
        } else {
            size_t length = strlen(reply);
            snprintf(reply + length, sizeof(reply) - length, "%s\n", mapping_fields[i - 3]);
        }
        assert(mem_service_client_reference_map_begin(&client, &request, &result, &transaction, &status));
        assert(status == MEM_SERVICE_WIRE_STATUS_INTERNAL);
        assert(!memcmp(&saved, &result, sizeof(result)));
        assert(!memcmp(&saved_transaction, &transaction, sizeof(transaction)));
    }
    before = calls;
    assert(mem_service_client_reference_transition(&client, &request, &result, &status) == 2);
    assert(calls == before);
    puts("reference_map_begin_client=pass malformed_replies=6 legacy_result_abi=preserved");
}

static void writer_test(void)
{
    unsigned char bytes[4096];
    struct mem_service_client client = {0};
    struct mem_service_client_allocation allocation = {0};
    struct mem_service_client_object_mapping mapping = {0};
    struct mem_service_client_mapping_lifecycle lifecycle = {0};
    struct mem_service_client_reference_view view = {.offset = 128, .len = 512,
        .object_kind = 5, .owner_entity = 1, .producer_entity = 2};
    struct lingqu_object_ref_wire_v2 reference, saved;
    enum mem_service_wire_status status;
    static const struct mem_service_provider_ops ops = {.probe = writer_probe, .publish_range = writer_publish};
    struct mem_service_provider provider = {.ops = &ops, .context = bytes,
        .capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING};
    struct mem_service_provider_channel channel = {.provider = &provider};
    writer_mode = true;
    transport_rc = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i + 17);
    snprintf(allocation_reply, sizeof(allocation_reply),
        "state=active\nkey=allocation-1\ngeneration=1\nversion=2\nsize_bytes=4096\n"
        "alignment_bytes=4096\ncapabilities=1\nlive_refs=1\nhome_node=home-1\n"
        "provider_incarnation=7\nprovider_backed=1\naddress=%llu\naddress_len=4096\n"
        "descriptor_len=4\ndescriptor_hex=01020304\n", (unsigned long long)(uintptr_t)bytes);
    assert(!mem_service_client_inspect_allocation(&client, "allocation-1", &allocation, &status));
    strcpy(mapping.key, allocation.key);
    mapping.generation = 1; mapping.base = bytes; mapping.len = sizeof(bytes); mapping.flags = 3;
    mapping.binding.mapped = true; mapping.binding.owner = &provider;
    mapping.binding.mapping.base = bytes; mapping.binding.mapping.len = sizeof(bytes);
    mapping.binding.mapping.handle = 9;
    lifecycle.pending = true; lifecycle.transaction.mapping_id = 11;
    lifecycle.transaction.generation = 1; lifecycle.transaction.state = 2;
    strcpy(lifecycle.session_id, "owner"); strcpy(lifecycle.operation_id, "writer-map");
    assert(!mem_service_client_prepare_managed_reference(&client, &channel, &allocation,
        &mapping, &lifecycle, &view, &reference, &status));
    assert(publish_calls == 1 && !lingqu_object_ref_v2_validate(&reference));
    assert(reference.object.payload_checksum == mem_service_provider_checksum64(bytes + 128, 512));
    assert(reference.object.payload_offset == 128 && reference.object.payload_bytes == 512);
    assert(reference.object.object_version == 2 && reference.access == LINGQU_OBJECT_REF_V2_READ);
    assert(reference.allocation_generation == 1 && reference.provider_incarnation == 7);
    assert(!strcmp(reference.allocation_key, "allocation-1") && !strcmp(reference.home_node, "home-1"));
    saved = reference;
    for (unsigned fault = 0; fault < 10; ++fault) {
        unsigned before = publish_calls;
        struct mem_service_client_allocation bad_allocation = allocation;
        struct mem_service_client_object_mapping bad_mapping = mapping;
        struct mem_service_client_reference_view bad_view = view;
        mapping_state = 2; transport_rc = 0; publish_failure = 0;
        switch (fault) {
        case 0: bad_view.offset = UINT64_MAX; break;
        case 1: bad_view.len = 0; break;
        case 2: bad_mapping.flags = 1; break;
        case 3: bad_mapping.binding.mapping.base = NULL; break;
        case 4: ++bad_allocation.version; break;
        case 5: ++bad_allocation.descriptor[0]; break;
        case 6: mapping_state = 3; break;
        case 7: transport_rc = 1; break;
        case 8: publish_failure = 1; break;
        case 9: publish_failure = 2; break;
        }
        assert(mem_service_client_prepare_managed_reference(&client, &channel, &bad_allocation,
            &bad_mapping, &lifecycle, &bad_view, &reference, &status));
        assert(!memcmp(&reference, &saved, sizeof(reference)));
        assert(publish_calls == before + (fault >= 8));
    }
    puts("reference_writer_prepare=pass failures=10 output_preserved=1 scope=mock-wire-provider");
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--self-test")) return 2;
    self_test();
    writer_test();
    return 0;
}
