#ifndef MEM_SERVICE_REFERENCE_PROTOCOL_H
#define MEM_SERVICE_REFERENCE_PROTOCOL_H

#include "lingqu_object_service.h"
#include "mem_service_wire.h"
#include "mem_service_wire_payload.h"

struct mem_service_reference_request {
    uint64_t action;
    char key[96];
    char session_id[64];
    char idempotency_key[96];
    uint64_t generation;
    uint64_t version;
    uint64_t access;
    struct lingqu_object_ref_wire_v2 reference;
};

static inline size_t mem_service_reference_text_length(const char *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length]) ++length;
    return length;
}

static inline int mem_service_reference_decode_hex(
    const char *text, struct lingqu_object_ref_wire_v2 *ref)
{
    uint8_t bytes[256];
    if (!text || mem_service_reference_text_length(text, 513) != 512) return -EINVAL;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
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
    return lingqu_object_ref_v2_decode(bytes, sizeof(bytes), ref);
}

static inline int mem_service_reference_encode_hex(
    const struct lingqu_object_ref_wire_v2 *ref, char *text, size_t capacity)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t bytes[256];
    int result;
    if (!text || capacity < 513) return -EINVAL;
    result = lingqu_object_ref_v2_encode(ref, bytes, sizeof(bytes));
    if (result) return result;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        text[i * 2] = digits[bytes[i] >> 4];
        text[i * 2 + 1] = digits[bytes[i] & 15];
    }
    text[512] = 0;
    return 0;
}

/* Known fields are unique, untruncated and action-specific. Unknown optional
 * fields retain the v1 forward-compatibility policy; malformed lines fail. */
static inline int mem_service_reference_parse_request(
    const char *payload, struct mem_service_reference_request *output)
{
    static const char *names[] = {"action", "key", "session_id", "generation",
        "version", "reference_hex", "access", "idempotency_key"};
    struct mem_service_reference_request request;
    unsigned seen = 0, required;
    const char *cursor = payload;
    if (!payload || !output || mem_service_reference_text_length(payload, MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) >=
        MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) return -EINVAL;
    memset(&request, 0, sizeof(request));
    while (*cursor) {
        const char *end = strchr(cursor, '\n');
        const char *equal;
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor), field;
        char value[513];
        uint64_t number;
        char *tail;
        equal = (const char *)memchr(cursor, '=', length);
        if (!equal || equal == cursor) return -EINVAL;
        for (field = 0; field < sizeof(names) / sizeof(names[0]); ++field)
            if (strlen(names[field]) == (size_t)(equal - cursor) &&
                !memcmp(cursor, names[field], (size_t)(equal - cursor))) break;
        if (field < sizeof(names) / sizeof(names[0])) {
            size_t size = length - (size_t)(equal - cursor) - 1;
            if ((seen & (1U << field)) || !size || size >= sizeof(value)) return -EINVAL;
            seen |= 1U << field;
            memcpy(value, equal + 1, size); value[size] = 0;
            if (field == 1 || field == 2 || field == 7) {
                char *destination = field == 1 ? request.key :
                    (field == 2 ? request.session_id : request.idempotency_key);
                size_t capacity = field == 2 ? sizeof(request.session_id) : sizeof(request.key);
                if (size >= capacity) return -EINVAL;
                memcpy(destination, value, size + 1);
                if (!lingqu_object_ref_v2_token_length(destination, capacity)) return -EINVAL;
            } else if (field == 5) {
                if (mem_service_reference_decode_hex(value, &request.reference)) return -EINVAL;
            } else {
                if (value[0] < '0' || value[0] > '9') return -EINVAL;
                errno = 0; number = strtoull(value, &tail, 0);
                if (errno || *tail) return -EINVAL;
                if (field == 0) request.action = number;
                else if (field == 3) request.generation = number;
                else if (field == 4) request.version = number;
                else request.access = number;
            }
        }
        cursor = end ? end + 1 : cursor + length;
    }
    switch (request.action) {
    case MEM_SERVICE_REFERENCE_BEGIN:
    case MEM_SERVICE_REFERENCE_SEAL:
        required = 1U | 2U | 4U | 8U | 16U | 128U;
        if (!request.generation || !request.version) return -EINVAL;
        break;
    case MEM_SERVICE_REFERENCE_STAGE:
        required = 1U | 2U | 4U | 32U | 128U;
        break;
    case MEM_SERVICE_REFERENCE_RESOLVE:
        required = 1U | 2U;
        break;
    case MEM_SERVICE_REFERENCE_ACQUIRE:
    case MEM_SERVICE_REFERENCE_MAP_BEGIN:
        required = 1U | 2U | 4U | 32U | 64U | 128U;
        if (!request.access || request.access > 3 ||
            (request.access & ~request.reference.access) ||
            strcmp(request.key, request.reference.allocation_key)) return -EINVAL;
        if (request.action == MEM_SERVICE_REFERENCE_MAP_BEGIN &&
            request.access != LINGQU_OBJECT_REF_V2_READ) return -EINVAL;
        break;
    default: return -EINVAL;
    }
    if (seen != required) return -EINVAL;
    *output = request;
    return 0;
}

static inline int mem_service_reference_format_request(
    const struct mem_service_reference_request *request, char *output, size_t capacity)
{
    char payload[1536] = "", hex[513];
    struct mem_service_reference_request checked;
    if (!request || !output ||
        !lingqu_object_ref_v2_token_length(request->key, sizeof(request->key))) return -EINVAL;
    if (request->action != MEM_SERVICE_REFERENCE_RESOLVE &&
        (!lingqu_object_ref_v2_token_length(request->session_id, sizeof(request->session_id)) ||
         !lingqu_object_ref_v2_token_length(request->idempotency_key, sizeof(request->idempotency_key))))
        return -EINVAL;
    if (mem_service_wire_payload_append_u64(payload, sizeof(payload), "action", request->action) ||
        mem_service_wire_payload_append_field(payload, sizeof(payload), "key", request->key)) return -EINVAL;
    if (request->action != MEM_SERVICE_REFERENCE_RESOLVE &&
        (mem_service_wire_payload_append_field(payload, sizeof(payload), "session_id", request->session_id) ||
         mem_service_wire_payload_append_field(payload, sizeof(payload), "idempotency_key", request->idempotency_key)))
        return -EINVAL;
    if (request->action == MEM_SERVICE_REFERENCE_BEGIN || request->action == MEM_SERVICE_REFERENCE_SEAL) {
        if (mem_service_wire_payload_append_u64(payload, sizeof(payload), "generation", request->generation) ||
            mem_service_wire_payload_append_u64(payload, sizeof(payload), "version", request->version)) return -EINVAL;
    }
    if (request->action == MEM_SERVICE_REFERENCE_STAGE || request->action == MEM_SERVICE_REFERENCE_ACQUIRE ||
        request->action == MEM_SERVICE_REFERENCE_MAP_BEGIN) {
        if (mem_service_reference_encode_hex(&request->reference, hex, sizeof(hex)) ||
            mem_service_wire_payload_append_field(payload, sizeof(payload), "reference_hex", hex)) return -EINVAL;
    }
    if ((request->action == MEM_SERVICE_REFERENCE_ACQUIRE || request->action == MEM_SERVICE_REFERENCE_MAP_BEGIN) &&
        mem_service_wire_payload_append_u64(payload, sizeof(payload), "access", request->access)) return -EINVAL;
    if (mem_service_reference_parse_request(payload, &checked) || strlen(payload) >= capacity) return -EINVAL;
    memcpy(output, payload, strlen(payload) + 1);
    return 0;
}

#endif
