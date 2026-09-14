#include "mem_service_object_refs.h"
#include "mem_service_record_table.h"

#include <stdint.h>
#include <string.h>

uint64_t mem_service_checksum_bytes(const uint8_t *bytes, uint64_t len)
{
    uint64_t hash = 1469598103934665603ULL;
    uint64_t i;

    for (i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int mem_service_record_to_lingqu_object_ref(const struct mem_service_record *record,
                                            struct lingqu_object_ref_wire *ref_out)
{
    if (!record || !record->in_use || !ref_out ||
        record->object_backing_len == 0 ||
        record->kind == MEM_SERVICE_RECORD_MANAGED_VIEW) {
        return -1;
    }
    memset(ref_out, 0, sizeof(*ref_out));
    ref_out->magic = LINGQU_OBJECT_REF_MAGIC;
    ref_out->layout_version = LINGQU_OBJECT_REF_LAYOUT_VERSION;
    ref_out->object_kind = (uint16_t)record->object_payload_kind;
    ref_out->state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    ref_out->owner_entity = record->object_owner_node;
    ref_out->producer_entity = record->object_owner_node;
    ref_out->object_version = record->version;
    ref_out->key_hash =
        mem_service_checksum_bytes((const uint8_t *)record->key,
                             (uint64_t)strnlen(record->key,
                                               sizeof(record->key)));
    ref_out->payload_offset = record->object_backing_offset;
    ref_out->payload_bytes = record->object_backing_len;
    ref_out->payload_checksum = record->object_payload_checksum;
    return 0;
}

int mem_service_record_to_lingqu_obmm_ref(const struct mem_service_record *record,
                                          struct lingqu_object_ref_wire *ref_out)
{
    return mem_service_record_to_lingqu_object_ref(record, ref_out);
}

static bool mem_service_reference_key_valid(const char *key)
{
    char token[MEM_SERVICE_MANAGED_KEY_LEN] = {0};
    size_t length;
    if (!key) return false;
    length = strnlen(key, sizeof(token));
    if (!length || length == sizeof(token)) return false;
    memcpy(token, key, length);
    return lingqu_object_ref_v2_token_length(token, sizeof(token)) != 0;
}

static bool mem_service_reference_equal(const struct lingqu_object_ref_wire_v2 *a,
                                         const struct lingqu_object_ref_wire_v2 *b)
{
    uint8_t left[256], right[256];
    return !lingqu_object_ref_v2_encode(a, left, sizeof(left)) &&
        !lingqu_object_ref_v2_encode(b, right, sizeof(right)) &&
        !memcmp(left, right, sizeof(left));
}

static bool mem_service_reference_record_valid(const struct mem_service_record *record)
{
    const struct lingqu_object_ref_wire_v2 *ref = &record->managed_ref;
    return record->in_use && record->kind == MEM_SERVICE_RECORD_MANAGED_VIEW &&
        !lingqu_object_ref_v2_validate(ref) && record->version == ref->object.object_version &&
        record->object_owner_node == ref->object.owner_entity &&
        record->object_payload_kind == ref->object.object_kind &&
        record->object_backing_offset == ref->object.payload_offset &&
        record->object_backing_len == ref->object.payload_bytes &&
        record->object_payload_checksum == ref->object.payload_checksum;
}

static enum mem_service_managed_result mem_service_reference_binding(
    struct mem_service *svc, const struct lingqu_object_ref_wire_v2 *ref,
    const char *session_id, bool writing, struct mem_service_managed_view *view)
{
    enum mem_service_managed_result result;
    if (!svc || lingqu_object_ref_v2_validate(ref))
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    result = mem_service_managed_content_check(&svc->managed, ref->allocation_key,
        session_id, ref->allocation_generation, ref->object.object_version, writing, view);
    if (result) return result;
    if (strcmp(view->key, ref->allocation_key) ||
        strcmp(view->home_node_id, ref->home_node) ||
        view->provider_incarnation != ref->provider_incarnation ||
        view->size_bytes != ref->allocation_bytes)
        return MEM_SERVICE_MANAGED_RESULT_PROVIDER_MISMATCH;
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_reference_stage(
    struct mem_service *svc, const char *object_key, const char *session_id,
    const struct lingqu_object_ref_wire_v2 *ref)
{
    struct mem_service_managed_view view;
    struct mem_service_record *record;
    struct lingqu_object_ref_wire_v2 saved;
    char saved_key[MEM_SERVICE_MANAGED_KEY_LEN];
    enum mem_service_managed_result result;
    if (!svc || !mem_service_reference_key_valid(object_key))
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    result = mem_service_reference_binding(svc, ref, session_id, true, &view);
    if (result) return result;
    record = mem_service_find_record(svc, object_key);
    if (record) {
        if (!mem_service_reference_record_valid(record) ||
            strcmp(record->managed_ref.allocation_key, ref->allocation_key))
            return MEM_SERVICE_MANAGED_RESULT_KEY_CONFLICT;
        if (record->managed_ref.allocation_generation == ref->allocation_generation &&
            record->version == ref->object.object_version)
            return mem_service_reference_equal(&record->managed_ref, ref) ?
                MEM_SERVICE_MANAGED_RESULT_OK : MEM_SERVICE_MANAGED_RESULT_KEY_CONFLICT;
    }
    saved = *ref; /* ref may alias the record being replaced. */
    memcpy(saved_key, object_key, strlen(object_key) + 1);
    if (!record) record = mem_service_alloc_record(svc);
    if (!record) return MEM_SERVICE_MANAGED_RESULT_CAPACITY;
    memset(record, 0, sizeof(*record));
    record->in_use = true;
    record->kind = MEM_SERVICE_RECORD_MANAGED_VIEW;
    memcpy(record->key, saved_key, strlen(saved_key) + 1);
    record->version = saved.object.object_version;
    record->object_owner_node = saved.object.owner_entity;
    record->object_payload_kind = saved.object.object_kind;
    record->object_backing_offset = saved.object.payload_offset;
    record->object_backing_len = saved.object.payload_bytes;
    record->object_payload_checksum = saved.object.payload_checksum;
    record->managed_ref = saved;
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_reference_seal(
    struct mem_service *svc, const char *allocation_key, const char *session_id,
    uint64_t generation, uint64_t version)
{
    struct mem_service_managed_view view;
    enum mem_service_managed_result result;
    size_t count = 0;
    if (!svc || !mem_service_reference_key_valid(allocation_key))
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    result = mem_service_managed_content_check(&svc->managed, allocation_key,
        session_id, generation, version, true, &view);
    if (result) return result;
    for (size_t i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        const struct mem_service_record *record = &svc->records[i];
        const struct lingqu_object_ref_wire_v2 *ref = &record->managed_ref;
        if (!record->in_use || record->kind != MEM_SERVICE_RECORD_MANAGED_VIEW ||
            ref->allocation_generation != generation || ref->object.object_version != version ||
            strncmp(ref->allocation_key, allocation_key, sizeof(ref->allocation_key))) continue;
        if (!mem_service_reference_record_valid(record))
            return MEM_SERVICE_MANAGED_RESULT_KEY_CONFLICT;
        result = mem_service_reference_binding(svc, ref, session_id, true, &view);
        if (result) return result;
        ++count;
    }
    if (!count) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    return mem_service_managed_content_seal(&svc->managed, allocation_key,
                                             session_id, generation, version);
}

enum mem_service_managed_result mem_service_reference_resolve(
    struct mem_service *svc, const char *object_key,
    struct lingqu_object_ref_wire_v2 *ref_out)
{
    struct mem_service_managed_view view;
    struct mem_service_record *record;
    enum mem_service_managed_result result;
    if (!svc || !ref_out || !mem_service_reference_key_valid(object_key))
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    record = mem_service_find_record(svc, object_key);
    if (!record || !mem_service_reference_record_valid(record))
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    result = mem_service_reference_binding(svc, &record->managed_ref, NULL, false, &view);
    if (result) return result;
    *ref_out = record->managed_ref;
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_reference_validate(
    struct mem_service *svc, const struct lingqu_object_ref_wire_v2 *ref,
    const char *session_id, uint32_t requested_access,
    struct mem_service_managed_view *allocation_out)
{
    struct mem_service_managed_view view;
    enum mem_service_managed_result result;
    bool registered = false;
    if (!svc || !allocation_out || !requested_access || lingqu_object_ref_v2_validate(ref) ||
        (requested_access & ~ref->access)) return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    result = mem_service_reference_binding(svc, ref, NULL, false, &view);
    if (result) return result;
    if ((requested_access & LINGQU_OBJECT_REF_V2_WRITE) &&
        (!session_id || strcmp(session_id, view.owner_session)))
        return MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER;
    for (size_t i = 0; i < MEM_SERVICE_MAX_RECORDS; ++i)
        if (mem_service_reference_record_valid(&svc->records[i]) &&
            mem_service_reference_equal(&svc->records[i].managed_ref, ref)) {
            registered = true;
            break;
        }
    if (!registered) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    *allocation_out = view;
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_reference_acquire(
    struct mem_service *svc, const struct lingqu_object_ref_wire_v2 *ref,
    const char *session_id, uint32_t requested_access,
    struct mem_service_managed_view *allocation_out)
{
    struct mem_service_managed_view current;
    enum mem_service_managed_result result;
    if (!allocation_out) return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    result = mem_service_reference_validate(svc, ref, session_id, requested_access, &current);
    if (result) return result;
    return mem_service_managed_acquire_published(&svc->managed, ref->allocation_key,
        session_id, ref->allocation_generation, ref->object.object_version, allocation_out);
}

enum mem_service_managed_result mem_service_reference_map_begin(
    struct mem_service *svc, const struct lingqu_object_ref_wire_v2 *ref,
    const char *session_id, uint32_t requested_access,
    struct mem_service_managed_mapping *mapping_out)
{
    struct mem_service_managed_view current;
    enum mem_service_managed_result result;
    if (!mapping_out || requested_access != LINGQU_OBJECT_REF_V2_READ)
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    result = mem_service_reference_validate(svc, ref, session_id, requested_access, &current);
    if (result) return result;
    return mem_service_managed_mapping_begin_published(&svc->managed, ref->allocation_key,
        session_id, ref->allocation_generation, ref->object.object_version, mapping_out);
}
