/* SPDX-License-Identifier: MIT */
#include "lingqu_object_service.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static const char golden[] =
    "4645524d4d424f51020005000200000001000000020000000300000000000000"
    "aaae337b0b4a94e8800f0000000000000001000000000000a8a7a6a5a4a3a2a1"
    "0001000003000000080706050403020118171615141312110000010000000000"
    "6f626a6563742f766965772d3100000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "67756573742d6e6f64652d310000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000";

static int from_hex(const char *text, uint8_t *bytes)
{
    if (strlen(text) != LINGQU_OBJECT_REF_V2_BYTES * 2) return -EINVAL;
    for (unsigned i = 0; i < LINGQU_OBJECT_REF_V2_BYTES; ++i) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; ++j) {
            unsigned char c = (unsigned char)text[i * 2 + j];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return -EINVAL;
            value = value * 16 + digit;
        }
        bytes[i] = (uint8_t)value;
    }
    return 0;
}

static struct lingqu_object_ref_wire_v2 example(void)
{
    struct lingqu_object_ref_wire_v2 ref;
    memset(&ref, 0, sizeof(ref));
    ref.object.magic = LINGQU_OBJECT_REF_MAGIC;
    ref.object.layout_version = LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    ref.object.object_kind = 5;
    ref.object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    ref.object.owner_entity = 1;
    ref.object.producer_entity = 2;
    ref.object.object_version = 3;
    ref.object.key_hash = UINT64_C(0xe8944a0b7b33aeaa);
    ref.object.payload_offset = 3968;
    ref.object.payload_bytes = 256;
    ref.object.payload_checksum = UINT64_C(0xa1a2a3a4a5a6a7a8);
    ref.wire_bytes = LINGQU_OBJECT_REF_V2_BYTES;
    ref.access = LINGQU_OBJECT_REF_V2_READ | LINGQU_OBJECT_REF_V2_WRITE;
    ref.allocation_generation = UINT64_C(0x0102030405060708);
    ref.provider_incarnation = UINT64_C(0x1112131415161718);
    ref.allocation_bytes = 65536;
    strcpy(ref.allocation_key, "object/view-1");
    strcpy(ref.home_node, "guest-node-1");
    return ref;
}

static void rejected(const struct lingqu_object_ref_wire_v2 *ref)
{
    unsigned char output[256], saved[256];
    memset(output, 0xa5, sizeof(output));
    memcpy(saved, output, sizeof(saved));
    assert(lingqu_object_ref_v2_encode(ref, output, sizeof(output)) != 0);
    assert(!memcmp(output, saved, sizeof(output)));
}

static void self_test(void)
{
    struct lingqu_object_ref_wire_v2 ref = example(), decoded, saved;
    uint8_t expected[256], bytes[258];
    assert(sizeof(struct lingqu_object_ref_wire) == 64);
    assert(LINGQU_OBJECT_REF_LAYOUT_VERSION == 1);
    assert(offsetof(struct lingqu_object_ref_wire_v2, allocation_generation) == 72);
    assert(offsetof(struct lingqu_object_ref_wire_v2, allocation_key) == 96);
    assert(offsetof(struct lingqu_object_ref_wire_v2, home_node) == 192);
    assert(from_hex(golden, expected) == 0);
    memset(bytes, 0xa5, sizeof(bytes));
    assert(lingqu_object_ref_v2_encode(&ref, bytes + 1, 257) == 0);
    assert(bytes[0] == 0xa5 && bytes[257] == 0xa5);
    assert(!memcmp(bytes + 1, expected, sizeof(expected)));
    assert(lingqu_object_ref_v2_decode(bytes + 1, 256, &decoded) == 0);
    assert(!memcmp(&ref, &decoded, sizeof(ref)));
    /* The frozen V1 reader's exact-version admission rejects V2. */
    assert(decoded.object.layout_version != LINGQU_OBJECT_REF_LAYOUT_VERSION);
    for (size_t length = 0; length < 256; ++length) {
        memset(&decoded, 0xa5, sizeof(decoded)); saved = decoded;
        assert(lingqu_object_ref_v2_decode(expected, length, &decoded) == -EMSGSIZE);
        assert(!memcmp(&decoded, &saved, sizeof(saved)));
        memset(bytes, 0xa5, sizeof(bytes));
        assert(lingqu_object_ref_v2_encode(&ref, bytes, length) == -EMSGSIZE);
        for (size_t i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
    }
    assert(lingqu_object_ref_v2_decode(expected, 257, &decoded) == -EMSGSIZE);
    assert(lingqu_object_ref_v2_decode(NULL, 256, &decoded) == -EINVAL);
    assert(lingqu_object_ref_v2_decode(expected, 256, NULL) == -EINVAL);
    assert(lingqu_object_ref_v2_encode(NULL, bytes, 256) == -EINVAL);
    assert(lingqu_object_ref_v2_encode(&ref, NULL, 256) == -EINVAL);
    /* Corrupt each structural field; do not call a changed checksum proof. */
    const unsigned positions[] = {0,8,10,12,14,24,32,64,68,72,80,88,96,110,192,205};
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        memcpy(bytes, expected, 256);
        unsigned at = positions[i];
        if (at == 192) bytes[at] = 0;
        else if (at == 10 || at == 12 || at == 24 || at == 68) memset(bytes + at, 0, 2);
        else if (at == 72 || at == 80 || at == 88) memset(bytes + at, 0, 8);
        else bytes[at] ^= 0x40;
        memset(&decoded, 0xa5, sizeof(decoded)); saved = decoded;
        assert(lingqu_object_ref_v2_decode(bytes, 256, &decoded) != 0);
        assert(!memcmp(&decoded, &saved, sizeof(saved)));
    }
    for (unsigned access = 0; access < 16; ++access) {
        ref = example(); ref.access = access;
        assert((lingqu_object_ref_v2_validate(&ref) == 0) == (access >= 1 && access <= 3));
    }
    ref = example(); ref.object.payload_bytes = 0; rejected(&ref);
    ref = example(); ref.object.payload_offset = UINT64_MAX; rejected(&ref);
    ref = example(); ref.object.payload_bytes = UINT64_MAX; rejected(&ref);
    ref = example(); ref.object.payload_offset = 65536; rejected(&ref);
    ref = example(); ref.object.layout_version = 1; rejected(&ref);
    ref = example(); memset(ref.allocation_key, 'k', sizeof(ref.allocation_key)); rejected(&ref);
    ref = example(); ref.home_node[0] = '\0'; rejected(&ref);
    const unsigned char invalid[] = {' ', '=', '\n', '\t', 0x7f};
    for (unsigned i = 0; i < sizeof(invalid); ++i) {
        ref = example(); ref.home_node[2] = (char)invalid[i]; rejected(&ref);
    }
    ref = example();
    memset(ref.allocation_key, 'k', sizeof(ref.allocation_key) - 1);
    ref.allocation_key[95] = 0;
    ref.object.key_hash = lingqu_object_ref_key_hash(ref.allocation_key, 95);
    memset(ref.home_node, 'h', sizeof(ref.home_node) - 1); ref.home_node[63] = 0;
    ref.allocation_bytes = UINT64_MAX;
    ref.object.payload_offset = UINT64_MAX - 1;
    ref.object.payload_bytes = 1;
    assert(lingqu_object_ref_v2_encode(&ref, bytes, 256) == 0);
    assert(lingqu_object_ref_v2_decode(bytes, 256, &decoded) == 0);
    assert(!memcmp(&ref, &decoded, sizeof(ref)));
    union { struct lingqu_object_ref_wire_v2 ref; uint8_t bytes[257]; } overlap;
    overlap.ref = example();
    assert(lingqu_object_ref_v2_encode(&overlap.ref, overlap.bytes + 1, 256) == 0);
    assert(!memcmp(overlap.bytes + 1, expected, 256));
    assert(lingqu_object_ref_v2_decode(overlap.bytes + 1, 256, &overlap.ref) == 0);
    ref = example(); assert(!memcmp(&overlap.ref, &ref, sizeof(ref)));
    /* Nonzero identity/checksum substitutions remain structurally valid.
     * A service must authorize the identity; no content was read here. */
    ref.allocation_generation++; ref.object.payload_checksum++; ref.home_node[0] = 'G';
    assert(lingqu_object_ref_v2_validate(&ref) == 0);
    puts("object_ref_v2=pass bytes=256 v1_bytes=64 truncations=256 mutations=16 scope=codec-only");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--self-test")) { self_test(); return 0; }
    if (argc == 3 && !strcmp(argv[1], "--decode-hex")) {
        uint8_t bytes[256];
        struct lingqu_object_ref_wire_v2 ref;
        int rc = from_hex(argv[2], bytes);
        if (!rc) rc = lingqu_object_ref_v2_decode(bytes, sizeof(bytes), &ref);
        if (rc) { fprintf(stderr, "object_ref_v2=invalid rc=%d\n", rc); return 1; }
        printf("object_ref_v2=valid key=%s home=%s access=%u generation=%" PRIu64
               " incarnation=%" PRIu64 " version=%" PRIu64 " offset=%" PRIu64
               " bytes=%" PRIu64 " allocation_bytes=%" PRIu64 " scope=codec-only\n",
               ref.allocation_key, ref.home_node, ref.access, ref.allocation_generation,
               ref.provider_incarnation, ref.object.object_version, ref.object.payload_offset,
               ref.object.payload_bytes, ref.allocation_bytes);
        return 0;
    }
    fprintf(stderr, "usage: object-ref-v2 --self-test | --decode-hex <512 hex digits>\n");
    return 2;
}
