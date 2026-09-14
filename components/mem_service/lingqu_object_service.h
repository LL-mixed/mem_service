#ifndef LINGQU_OBJECT_SERVICE_H
#define LINGQU_OBJECT_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>

#define LINGQU_OBJECT_REF_MAGIC 0x514f424d4d524546ULL
#define LINGQU_OBJECT_REF_LAYOUT_VERSION 1U
#define LINGQU_OBMM_OBJECT_REF_MAGIC LINGQU_OBJECT_REF_MAGIC
#define LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION LINGQU_OBJECT_REF_LAYOUT_VERSION
#define LINGQU_OBJECT_STATE_PENDING_WIRE 1U
#define LINGQU_OBJECT_STATE_COMMITTED_WIRE 2U
#define LINGQU_OBJECT_STATE_TOMBSTONED_WIRE 3U
#define LINGQU_OBJECT_STATE_QUARANTINED_WIRE 4U

struct lingqu_object_ref_wire {
    uint64_t magic;
    uint16_t layout_version;
    uint16_t object_kind;
    uint16_t state;
    uint16_t flags;
    uint32_t owner_entity;
    uint32_t producer_entity;
    uint64_t object_version;
    uint64_t key_hash;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    uint64_t payload_checksum;
};

/* V1 remains unchanged. V2 offsets are allocation-relative, never arena offsets.
 * These codecs validate metadata only; service binding and access are separate. */
#define LINGQU_OBJECT_REF_V2_LAYOUT_VERSION 2U
#define LINGQU_OBJECT_REF_V2_BYTES 256U
#define LINGQU_OBJECT_REF_V2_KEY_BYTES 96U
#define LINGQU_OBJECT_REF_V2_HOME_BYTES 64U
#define LINGQU_OBJECT_REF_V2_READ 1U
#define LINGQU_OBJECT_REF_V2_WRITE 2U

struct lingqu_object_ref_wire_v2 {
    struct lingqu_object_ref_wire object;
    uint32_t wire_bytes;
    uint32_t access;
    uint64_t allocation_generation;
    uint64_t provider_incarnation;
    uint64_t allocation_bytes;
    char allocation_key[LINGQU_OBJECT_REF_V2_KEY_BYTES];
    char home_node[LINGQU_OBJECT_REF_V2_HOME_BYTES];
};

typedef char lingqu_object_ref_v1_size_check[
    sizeof(struct lingqu_object_ref_wire) == 64 ? 1 : -1];
typedef char lingqu_object_ref_v2_size_check[
    sizeof(struct lingqu_object_ref_wire_v2) == LINGQU_OBJECT_REF_V2_BYTES ? 1 : -1];

static inline uint64_t lingqu_object_ref_key_hash(const char *key, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; ++i)
        hash = (hash ^ (unsigned char)key[i]) * UINT64_C(1099511628211);
    return hash;
}

/* Fixed-width strings have one nonempty token followed only by zero padding. */
static inline size_t lingqu_object_ref_v2_token_length(const char *text, size_t size)
{
    size_t length = 0;
    while (length < size && text[length]) {
        unsigned char byte = (unsigned char)text[length];
        if (byte <= 0x20 || byte == 0x7f || byte == '=') return 0;
        ++length;
    }
    if (!length || length == size) return 0;
    for (size_t i = length; i < size; ++i)
        if (text[i]) return 0;
    return length;
}

static inline int lingqu_object_ref_v2_validate(const struct lingqu_object_ref_wire_v2 *ref)
{
    size_t length;
    if (!ref) return -EINVAL;
    if (ref->object.magic != LINGQU_OBJECT_REF_MAGIC) return -EPROTO;
    if (ref->object.layout_version != LINGQU_OBJECT_REF_V2_LAYOUT_VERSION)
        return -EPROTONOSUPPORT;
    if (ref->wire_bytes != LINGQU_OBJECT_REF_V2_BYTES) return -EMSGSIZE;
    if (ref->object.flags || !ref->object.object_kind || !ref->object.object_version ||
        ref->object.state != LINGQU_OBJECT_STATE_COMMITTED_WIRE ||
        !ref->allocation_generation || !ref->provider_incarnation ||
        !ref->access || (ref->access & ~(LINGQU_OBJECT_REF_V2_READ | LINGQU_OBJECT_REF_V2_WRITE)))
        return -EINVAL;
    length = lingqu_object_ref_v2_token_length(ref->allocation_key, sizeof(ref->allocation_key));
    if (!length || !lingqu_object_ref_v2_token_length(ref->home_node, sizeof(ref->home_node)) ||
        ref->object.key_hash != lingqu_object_ref_key_hash(ref->allocation_key, length))
        return -EINVAL;
    if (!ref->allocation_bytes || !ref->object.payload_bytes ||
        ref->object.payload_offset > ref->allocation_bytes ||
        ref->object.payload_bytes > ref->allocation_bytes - ref->object.payload_offset)
        return -ERANGE;
    return 0;
}

static inline void lingqu_object_ref_v2_put(uint8_t *bytes, uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static inline uint64_t lingqu_object_ref_v2_get(const uint8_t *bytes, unsigned width)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

/* Output remains untouched on failure. Unaligned buffers and overlap are valid. */
static inline int lingqu_object_ref_v2_encode(const struct lingqu_object_ref_wire_v2 *ref,
                                             void *output, size_t capacity)
{
    uint8_t bytes[LINGQU_OBJECT_REF_V2_BYTES] = {0};
    int rc;
    if (!output) return -EINVAL;
    if (capacity < sizeof(bytes)) return -EMSGSIZE;
    rc = lingqu_object_ref_v2_validate(ref);
    if (rc) return rc;
    lingqu_object_ref_v2_put(bytes, ref->object.magic, 8);
    lingqu_object_ref_v2_put(bytes + 8, ref->object.layout_version, 2);
    lingqu_object_ref_v2_put(bytes + 10, ref->object.object_kind, 2);
    lingqu_object_ref_v2_put(bytes + 12, ref->object.state, 2);
    lingqu_object_ref_v2_put(bytes + 14, ref->object.flags, 2);
    lingqu_object_ref_v2_put(bytes + 16, ref->object.owner_entity, 4);
    lingqu_object_ref_v2_put(bytes + 20, ref->object.producer_entity, 4);
    lingqu_object_ref_v2_put(bytes + 24, ref->object.object_version, 8);
    lingqu_object_ref_v2_put(bytes + 32, ref->object.key_hash, 8);
    lingqu_object_ref_v2_put(bytes + 40, ref->object.payload_offset, 8);
    lingqu_object_ref_v2_put(bytes + 48, ref->object.payload_bytes, 8);
    lingqu_object_ref_v2_put(bytes + 56, ref->object.payload_checksum, 8);
    lingqu_object_ref_v2_put(bytes + 64, ref->wire_bytes, 4);
    lingqu_object_ref_v2_put(bytes + 68, ref->access, 4);
    lingqu_object_ref_v2_put(bytes + 72, ref->allocation_generation, 8);
    lingqu_object_ref_v2_put(bytes + 80, ref->provider_incarnation, 8);
    lingqu_object_ref_v2_put(bytes + 88, ref->allocation_bytes, 8);
    memcpy(bytes + 96, ref->allocation_key, sizeof(ref->allocation_key));
    memcpy(bytes + 192, ref->home_node, sizeof(ref->home_node));
    memcpy(output, bytes, sizeof(bytes));
    return 0;
}

static inline int lingqu_object_ref_v2_decode(const void *input, size_t length,
                                             struct lingqu_object_ref_wire_v2 *output)
{
    const uint8_t *bytes = (const uint8_t *)input;
    struct lingqu_object_ref_wire_v2 ref;
    int rc;
    if (!input || !output) return -EINVAL;
    if (length != LINGQU_OBJECT_REF_V2_BYTES) return -EMSGSIZE;
    memset(&ref, 0, sizeof(ref));
    ref.object.magic = lingqu_object_ref_v2_get(bytes, 8);
    ref.object.layout_version = (uint16_t)lingqu_object_ref_v2_get(bytes + 8, 2);
    ref.object.object_kind = (uint16_t)lingqu_object_ref_v2_get(bytes + 10, 2);
    ref.object.state = (uint16_t)lingqu_object_ref_v2_get(bytes + 12, 2);
    ref.object.flags = (uint16_t)lingqu_object_ref_v2_get(bytes + 14, 2);
    ref.object.owner_entity = (uint32_t)lingqu_object_ref_v2_get(bytes + 16, 4);
    ref.object.producer_entity = (uint32_t)lingqu_object_ref_v2_get(bytes + 20, 4);
    ref.object.object_version = lingqu_object_ref_v2_get(bytes + 24, 8);
    ref.object.key_hash = lingqu_object_ref_v2_get(bytes + 32, 8);
    ref.object.payload_offset = lingqu_object_ref_v2_get(bytes + 40, 8);
    ref.object.payload_bytes = lingqu_object_ref_v2_get(bytes + 48, 8);
    ref.object.payload_checksum = lingqu_object_ref_v2_get(bytes + 56, 8);
    ref.wire_bytes = (uint32_t)lingqu_object_ref_v2_get(bytes + 64, 4);
    ref.access = (uint32_t)lingqu_object_ref_v2_get(bytes + 68, 4);
    ref.allocation_generation = lingqu_object_ref_v2_get(bytes + 72, 8);
    ref.provider_incarnation = lingqu_object_ref_v2_get(bytes + 80, 8);
    ref.allocation_bytes = lingqu_object_ref_v2_get(bytes + 88, 8);
    memcpy(ref.allocation_key, bytes + 96, sizeof(ref.allocation_key));
    memcpy(ref.home_node, bytes + 192, sizeof(ref.home_node));
    rc = lingqu_object_ref_v2_validate(&ref);
    if (rc) return rc;
    memcpy(output, &ref, sizeof(ref));
    return 0;
}

#define lingqu_obmm_object_ref_wire lingqu_object_ref_wire

#endif
