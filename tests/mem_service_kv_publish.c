#include "mem_service_internal.h"
#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_model_runtime.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_ub_ssd_gsva_io.h"

#include <assert.h>

static struct mem_service_cluster_runtime runtime;
static struct mem_service service;
static uint8_t *storage;
static size_t storage_len = 8U * 1024U * 1024U;
static unsigned int payload_copies, publications, updates;
static bool fail_update;

/* All tested production translation units redirect explicit memcpy here. */
void *mem_service_test_memcpy(void *dst, const void *src, size_t len)
{
    if ((uintptr_t)dst >= (uintptr_t)storage &&
        (uintptr_t)dst < (uintptr_t)storage + storage_len) {
        payload_copies++;
    }
    for (size_t i = 0; i < len; i++) {
        ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
    }
    return dst;
}

struct mem_service_cluster_runtime *mem_service_cluster_runtime_current(void)
{
    return &runtime;
}

int mem_service_cluster_runtime_require(struct mem_service_cluster_runtime *rt)
{
    return rt == &runtime ? 0 : -1;
}

long mem_service_wallclock_ms(void) { return 1234; }

int mem_service_update_region_range_at(const struct mem_service_cluster_slot *slot,
                                       uint64_t offset, uint64_t len, bool write)
{
    assert(slot == &runtime.slots[0] && write);
    assert(offset <= storage_len && len <= storage_len - offset);
    updates++;
    return fail_update ? -1 : 0;
}

int mem_service_write_cluster_payload(struct mem_service_cluster_runtime *rt,
                                      struct mem_service *svc,
                                      struct mem_service_cluster_slot *slot)
{
    assert(rt == &runtime && svc == &service && slot == &runtime.slots[0]);
    publications++;
    rt->publish_seq++;
    return 0;
}

int mem_service_cluster_runtime_make_gsva_buffer_desc(
    const struct mem_service_cluster_runtime *rt,
    const struct mem_service_record *record, struct mem_service_gsva_buffer_desc *out)
{
    (void)rt; (void)record; (void)out;
    return -1; /* Optional SSD backend stays disabled in this host fixture. */
}

enum mem_service_ub_ssd_gsva_io_status mem_service_ub_ssd_gsva_submit(
    const struct mem_service_ub_ssd_gsva_io_request *request,
    struct mem_service_ub_ssd_gsva_io_completion *completion)
{
    (void)request; (void)completion;
    assert(!"SSD submission is outside this fixture");
    return MEM_SERVICE_UB_SSD_GSVA_IO_UNSUPPORTED;
}

int mem_service_try_push_obmm_object_desc_to(
    struct mem_service_cluster_runtime *rt, uint32_t node, uint32_t kind,
    uint64_t offset, uint64_t len, uint64_t checksum, uint16_t epoch)
{
    (void)rt; (void)node; (void)kind; (void)offset;
    (void)len; (void)checksum; (void)epoch;
    assert(!"Terminal range does not send a downstream notification");
    return -1;
}

int mem_service_wait_obmm_object_ack_from(
    struct mem_service_cluster_runtime *rt, uint32_t node, uint32_t kind,
    uint64_t offset, uint64_t len, uint64_t checksum, uint16_t epoch)
{
    return mem_service_try_push_obmm_object_desc_to(
        rt, node, kind, offset, len, checksum, epoch);
}

int main(int argc, char **argv)
{
    struct mem_service_obmm_range_flow_request request = {
        .model_key = "fixture", .total_layers = 1, .range_nodes = 1,
        .hidden_range_bytes = 64,
        .local_placement = {.owner_node = 0, .layer_end = 1,
                            .layer_count = 1, .terminal = true},
        .publish_payload_in_place = true, .publish_kv_in_place = true,
    };
    uint8_t *hidden, *kv;
    uint64_t checksum, kv_checksum, arena_next, kv_offset;
    bool success = false, copy_mode = false;
    int rc;

    assert(argc == 2);
    storage = calloc(1, storage_len);
    assert(storage);
    runtime.local_idx = 0;
    runtime.node_count = 1;
    runtime.slots[0].region.addr = storage;
    runtime.slots[0].region.len = storage_len;
    runtime.payload_arena_base = MEM_SERVICE_OBMM_KV_STATE_BLOCK_TIER0_BYTES;
    runtime.payload_arena_next = runtime.payload_arena_base;
    assert(mem_service_payload_arena_alloc(
        &runtime, 64, 64, &request.publish_payload_offset) == 0);
    assert(mem_service_model_kv_state_alloc(
        &runtime, 80, &request.publish_kv_offset, NULL, NULL, NULL) == 0);
    hidden = storage + request.publish_payload_offset;
    kv = storage + request.publish_kv_offset;
    memset(hidden, 0x17, 64);
    memset(kv, 0x29, 80);
    checksum = mem_service_model_payload_checksum(hidden, 64);
    kv_checksum = mem_service_model_payload_checksum(kv, 80);
    kv_offset = request.publish_kv_offset;

    if (strcmp(argv[1], "in_place") == 0) {
        success = true;
    } else if (strcmp(argv[1], "copy") == 0) {
        success = copy_mode = true;
        request.publish_kv_in_place = false;
    } else if (strcmp(argv[1], "wrong_pointer") == 0) {
        kv++;
    } else if (strcmp(argv[1], "invalid_pointer") == 0) {
        kv = (uint8_t *)(uintptr_t)1; /* Must reject before checksum read. */
    } else if (strcmp(argv[1], "checksum") == 0) {
        kv_checksum++;
    } else if (strcmp(argv[1], "unreserved_padding") == 0) {
        runtime.payload_arena_next = kv_offset + 80;
    } else if (strcmp(argv[1], "visibility_failure") == 0) {
        fail_update = true;
    } else {
        assert(!"unknown test case");
    }
    arena_next = runtime.payload_arena_next;
    rc = mem_service_range_flow_publish_runtime_output(
        &service, &request, 0, 1, 0, hidden, 64, checksum, kv, 80, kv_checksum);
    if (success) {
        const struct mem_service_record *record = &service.records[1];
        assert(rc == 0 && service.record_count == 2 && publications == 1 && updates == 2);
        assert(record->object_payload_kind == MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE);
        assert(record->object_backing_len == 80 && record->object_payload_checksum == kv_checksum);
        assert(memcmp(storage + record->object_backing_offset, kv, 80) == 0);
        assert(payload_copies == (copy_mode ? 1U : 0U));
        if (copy_mode) {
            assert(record->object_backing_offset != kv_offset);
            assert(runtime.payload_arena_next > arena_next);
        } else {
            assert(record->object_backing_offset == kv_offset);
            assert(runtime.payload_arena_next == arena_next);
        }
    } else {
        assert(rc != 0 && service.record_count == 0 && publications == 0);
        assert(payload_copies == 0 && runtime.payload_arena_next == arena_next);
    }
    free(storage);
    printf("case=%s status=ok payload_copies=%u\n", argv[1], payload_copies);
    return 0;
}
