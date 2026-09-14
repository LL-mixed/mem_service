/* SPDX-License-Identifier: MIT */
#include "mem_service_reference_protocol.h"
#include <assert.h>

static void self_test(void)
{
    struct mem_service_reference_request request = {0}, decoded, saved;
    struct lingqu_object_ref_wire_v2 *ref = &request.reference;
    char payload[2048], changed[4096], hex[513], output[2048], before[2048];
    unsigned actions = 0, rejected = 0;
    ref->object.magic = LINGQU_OBJECT_REF_MAGIC;
    ref->object.layout_version = LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    ref->object.object_kind = 5;
    ref->object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    ref->object.object_version = 2;
    ref->object.payload_bytes = 512;
    ref->wire_bytes = 256; ref->access = 1;
    ref->allocation_generation = 1; ref->provider_incarnation = 7;
    ref->allocation_bytes = 4096;
    strcpy(ref->allocation_key, "allocation-1"); strcpy(ref->home_node, "home-1");
    ref->object.key_hash = lingqu_object_ref_key_hash(ref->allocation_key, strlen(ref->allocation_key));
    strcpy(request.key, "allocation-1"); strcpy(request.session_id, "writer");
    strcpy(request.idempotency_key, "nonce-1"); request.generation = 1;
    request.version = 1; request.access = 1;
    for (request.action = 1; request.action <= 5; ++request.action) {
        assert(!mem_service_reference_format_request(&request, payload, sizeof(payload)));
        assert(!mem_service_reference_parse_request(payload, &decoded));
        assert(decoded.action == request.action && !strcmp(decoded.key, request.key));
        if (request.action == 2 || request.action == 5)
            assert(!memcmp(&decoded.reference, ref, sizeof(*ref)));
        snprintf(changed, sizeof(changed), "%sfuture_optional=accepted\n", payload);
        assert(!mem_service_reference_parse_request(changed, &decoded));
        saved = decoded;
        snprintf(changed, sizeof(changed), "%skey=duplicate\n", payload);
        assert(mem_service_reference_parse_request(changed, &decoded));
        assert(!memcmp(&saved, &decoded, sizeof(saved))); ++rejected;
        memset(output, 0xa5, sizeof(output)); memcpy(before, output, sizeof(before));
        assert(mem_service_reference_format_request(&request, output, strlen(payload)));
        assert(!memcmp(output, before, sizeof(before)));
        ++actions;
    }
    request.action = MEM_SERVICE_REFERENCE_BEGIN;
    assert(!mem_service_reference_format_request(&request, payload, sizeof(payload)));
    const char *bad[] = {"-1", "+1", " 1", "18446744073709551616", "1garbage", "0", "09"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        snprintf(changed, sizeof(changed),
            "action=1\nkey=allocation-1\nsession_id=writer\nidempotency_key=nonce\ngeneration=%s\nversion=1\n", bad[i]);
        saved = decoded;
        assert(mem_service_reference_parse_request(changed, &decoded));
        assert(!memcmp(&saved, &decoded, sizeof(saved))); ++rejected;
    }
    const char *extra[] = {"access=1\n", "malformed\n", "=empty_name\n", "\n"};
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        snprintf(changed, sizeof(changed), "%s%s", payload, extra[i]);
        assert(mem_service_reference_parse_request(changed, &decoded)); ++rejected;
    }
    assert(!mem_service_reference_encode_hex(ref, hex, sizeof(hex)));
    for (size_t i = 0; i < 512; ++i) {
        char saved_char = hex[i]; hex[i] = 0;
        assert(mem_service_reference_decode_hex(hex, &decoded.reference));
        hex[i] = saved_char;
    }
    hex[511] = 'g'; assert(mem_service_reference_decode_hex(hex, &decoded.reference));
    request.action = MEM_SERVICE_REFERENCE_RESOLVE;
    memset(request.key, 'x', sizeof(request.key));
    assert(mem_service_reference_format_request(&request, output, sizeof(output)));
    printf("reference_protocol=pass actions=%u invalid_requests=%u hex_truncations=512 scope=codec-only\n",
           actions, rejected);
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--self-test")) return 2;
    self_test();
    return 0;
}
