/* SPDX-License-Identifier: MIT */
#include "mem_service_client.h"
#include <assert.h>

static char reply[4096];
static unsigned calls;
static int transport_rc;

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

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--self-test")) return 2;
    self_test();
    return 0;
}
