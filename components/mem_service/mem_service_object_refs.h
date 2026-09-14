#ifndef MEM_SERVICE_OBJECT_REFS_H
#define MEM_SERVICE_OBJECT_REFS_H

#include "mem_service.h"

uint64_t mem_service_checksum_bytes(const uint8_t *bytes, uint64_t len);
int mem_service_record_to_lingqu_object_ref(const struct mem_service_record *record,
                                            struct lingqu_object_ref_wire *ref_out);
int mem_service_record_to_lingqu_obmm_ref(const struct mem_service_record *record,
                                          struct lingqu_object_ref_wire *ref_out);

/* Serialized service-core catalog. All operations share svc's existing
 * allocation authority and record table. No provider or payload I/O occurs.
 * Begin via mem_service_managed_content_begin, stage views, then seal after
 * provider publication and confirmed unmap. Resolve/acquire require sealing.
 * Wire idempotency, durable recovery and reader mapping are separate seams. */
enum mem_service_managed_result mem_service_reference_stage(
    struct mem_service *svc, const char *object_key, const char *session_id,
    const struct lingqu_object_ref_wire_v2 *ref);
enum mem_service_managed_result mem_service_reference_seal(
    struct mem_service *svc, const char *allocation_key, const char *session_id,
    uint64_t generation, uint64_t version);
enum mem_service_managed_result mem_service_reference_resolve(
    struct mem_service *svc, const char *object_key,
    struct lingqu_object_ref_wire_v2 *ref_out);
enum mem_service_managed_result mem_service_reference_acquire(
    struct mem_service *svc, const struct lingqu_object_ref_wire_v2 *ref,
    const char *session_id, uint32_t requested_access,
    struct mem_service_managed_view *allocation_out);

#endif
