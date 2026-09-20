#include "mem_service_internal.h"
#include "mem_service_cluster_read.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_model_runtime.h"

#include <assert.h>

static struct mem_service_cluster_runtime runtime;
static struct mem_service service;
static uint8_t *exported;
static uint8_t *imported;
static size_t region_bytes;
static unsigned int sync_calls;
static unsigned int fail_sync_call;
static bool change_publication;

void mem_service_cpu_relax_wait(unsigned int *attempt)
{
    (*attempt)++;
}

struct mem_service_cluster_runtime *mem_service_cluster_runtime_current(void)
{
    return &runtime;
}

int mem_service_cluster_runtime_require(struct mem_service_cluster_runtime *rt)
{
    return rt == &runtime ? 0 : -1;
}

int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt,
                                     int owner_idx)
{
    return rt == &runtime && owner_idx == 1 ? 0 : -1;
}

int mem_service_sync_remote_range(const struct mem_service_cluster_slot *slot,
                                  uint64_t offset, uint64_t length)
{
    assert(slot == &runtime.slots[1]);
    assert(offset >= runtime.payload_offset);
    offset -= runtime.payload_offset;
    assert(offset <= region_bytes && length <= region_bytes - offset);
    sync_calls++;
    if (sync_calls == fail_sync_call) {
        return -1;
    }
    if (change_publication && sync_calls == 3) {
        struct mem_service_cluster_payload *payload = (void *)exported;
        payload->publish_seq++;
        payload->publish_done_seq++;
    }
    /* Imported bytes stay stale until the production reader requests a sync. */
    memcpy(imported + offset, exported + offset, length);
    return 0;
}

uint64_t mem_service_model_payload_checksum(const uint8_t *data, uint64_t bytes)
{
    uint64_t sum = 1469598103934665603ULL;
    for (uint64_t i = 0; i < bytes; i++) {
        sum = (sum ^ data[i]) * 1099511628211ULL;
    }
    return sum;
}

static void publish(uint64_t step, uint64_t token)
{
    struct mem_service_cluster_payload *payload = (void *)exported;
    struct mem_service_record *record = &payload->records[0];
    uint64_t words[8] = {step, token};

    memset(exported, 0, region_bytes);
    payload->magic = MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC;
    payload->version = MEM_SERVICE_CLUSTER_PAYLOAD_VERSION;
    payload->publish_seq = payload->publish_done_seq = (uint32_t)step + 1;
    payload->record_count = 1;
    ((struct mem_service_cluster_payload_compact_summary *)payload->record_pad)
        ->record_count = 1;
    record->in_use = true;
    record->kind = MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT;
    snprintf(record->key, sizeof(record->key),
             "tokens/fixture/decode-step%" PRIu64, step);
    record->object_payload_kind = MEM_SERVICE_OBMM_KIND_MODEL_TOKEN_RESULT;
    record->object_backing_offset = sizeof(*payload);
    record->object_backing_len = sizeof(words);
    record->object_payload_checksum = mem_service_model_payload_checksum(
        (const uint8_t *)words, sizeof(words));
    memcpy(exported + sizeof(*payload), words, sizeof(words));
}

static void publish_reference(uint64_t step, uint32_t owner_node)
{
    struct mem_service_cluster_payload *payload = (void *)exported;
    struct mem_service_record *record = &payload->records[0];
    struct lingqu_object_ref_wire reference = {0};
    char key[sizeof(record->key)];
    uint64_t offset;
    size_t key_len;

    snprintf(key,
             sizeof(key),
             "tokens/fixture/decode-step%" PRIu64,
             step);
    key_len = strlen(key);
    reference.magic = LINGQU_OBJECT_REF_MAGIC;
    reference.layout_version = LINGQU_OBJECT_REF_LAYOUT_VERSION;
    reference.object_kind = MEM_SERVICE_OBMM_KIND_MODEL_TOKEN_RESULT;
    reference.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    reference.owner_entity = owner_node;
    reference.producer_entity = owner_node;
    reference.object_version = record->version ? record->version : 1;
    reference.key_hash = lingqu_object_ref_key_hash(key, key_len);
    reference.payload_offset = record->object_backing_offset;
    reference.payload_bytes = record->object_backing_len;
    reference.payload_checksum = record->object_payload_checksum;
    offset = MEM_SERVICE_OBMM_TOKEN_REFERENCE_OFFSET +
             (reference.key_hash % MEM_SERVICE_OBMM_TOKEN_REFERENCE_SLOTS) *
                 MEM_SERVICE_OBMM_TOKEN_REFERENCE_SLOT_BYTES;
    assert(offset <= region_bytes);
    assert(MEM_SERVICE_OBMM_TOKEN_REFERENCE_SLOT_BYTES <=
           region_bytes - offset);
    memcpy(exported + offset, &reference, sizeof(reference));
    memcpy(exported + offset + sizeof(reference),
           &reference,
           sizeof(reference));
}

static int read_token(uint64_t step, uint64_t *token)
{
    struct mem_service_obmm_range_flow_request request = {
        .model_key = "fixture",
    };
    return mem_service_range_flow_wait_terminal_token(
        &service, &request, step, 0, token);
}

int main(int argc, char **argv)
{
    uint64_t token = 0;
    int rc;
    bool success = false;
    assert(argc == 2);
    region_bytes = sizeof(struct mem_service_cluster_payload) + 64;
    exported = calloc(1, region_bytes);
    imported = calloc(1, region_bytes);
    assert(exported && imported);
    runtime.local_idx = 0;
    runtime.node_count = 2;
    runtime.payload_offset = 4096;
    runtime.slots[0].is_local = true;
    runtime.slots[1].region.addr = imported;
    runtime.slots[1].region.len = region_bytes;
    runtime.slots[1].region.fd = 1;
    runtime.slots[1].map_osync = true;

    publish(0, 11);
    memcpy(imported, exported, region_bytes);
    assert(read_token(0, &token) == 0 && token == 11);
    publish(1, 22);
    sync_calls = 0;

    if (strcmp(argv[1], "reference_remote") == 0 ||
        strcmp(argv[1], "reference_duplicate_mismatch") == 0 ||
        strcmp(argv[1], "reference_wrong_key") == 0) {
        struct lingqu_object_ref_wire reference;
        const char *key = strcmp(argv[1], "reference_wrong_key") == 0 ?
                              "tokens/fixture/decode-step0" :
                              "tokens/fixture/decode-step1";

        publish_reference(1, 1);
        if (strcmp(argv[1], "reference_duplicate_mismatch") == 0) {
            uint64_t key_hash = lingqu_object_ref_key_hash(
                "tokens/fixture/decode-step1",
                strlen("tokens/fixture/decode-step1"));
            uint64_t offset = MEM_SERVICE_OBMM_TOKEN_REFERENCE_OFFSET +
                              (key_hash %
                               MEM_SERVICE_OBMM_TOKEN_REFERENCE_SLOTS) *
                                  MEM_SERVICE_OBMM_TOKEN_REFERENCE_SLOT_BYTES;

            exported[offset + sizeof(reference) +
                     offsetof(struct lingqu_object_ref_wire, payload_checksum)] ^=
                1;
        }
        memset(&reference, 0, sizeof(reference));
        success = mem_service_model_refresh_terminal_token_reference(
            &runtime, &runtime.slots[1], 1, key, &reference);
        assert(success == (strcmp(argv[1], "reference_remote") == 0));
        if (success) {
            assert(reference.payload_checksum ==
                   ((struct mem_service_cluster_payload *)exported)
                       ->records[0]
                       .object_payload_checksum);
        }
        assert(sync_calls == 1U);
        printf("case=%s status=ok sync_calls=%u\n", argv[1], sync_calls);
        free(imported);
        free(exported);
        return 0;
    } else if (strcmp(argv[1], "record_by_key") == 0) {
        struct mem_service_record record;

        memset(&record, 0, sizeof(record));
        assert(mem_service_model_refresh_remote_record_by_key(
            &runtime,
            &runtime.slots[1],
            "tokens/fixture/decode-step1",
            &record));
        assert(record.kind == MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT);
        assert(record.object_backing_len == sizeof(uint64_t) * 8U);
        assert(sync_calls == 3U);
        printf("case=%s status=ok sync_calls=%u\n", argv[1], sync_calls);
        free(imported);
        free(exported);
        return 0;
    } else if (strcmp(argv[1], "record_by_backing") == 0 ||
               strcmp(argv[1], "record_by_backing_changed_publication") == 0) {
        struct mem_service_record record;
        const struct mem_service_record *published =
            &((struct mem_service_cluster_payload *)exported)->records[0];
        uint32_t cookie =
            (uint32_t)(published->object_payload_checksum ^
                       (published->object_payload_checksum >> 32));

        memset(&record, 0, sizeof(record));
        change_publication =
            strcmp(argv[1], "record_by_backing_changed_publication") == 0;
        success = mem_service_model_refresh_remote_record_by_obmm_object_backing(
            &runtime,
            &runtime.slots[1],
            MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT,
            MEM_SERVICE_OBMM_KIND_MODEL_TOKEN_RESULT,
            published->object_backing_offset,
            published->object_backing_len,
            cookie,
            &record);
        assert(success == !change_publication);
        if (success) {
            assert(record.kind == MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT);
            assert(record.object_backing_len == sizeof(uint64_t) * 8U);
        }
        assert(sync_calls == 3U);
        printf("case=%s status=ok sync_calls=%u\n", argv[1], sync_calls);
        free(imported);
        free(exported);
        return 0;
    } else if (strcmp(argv[1], "record_at_backing") == 0 ||
               strcmp(argv[1], "record_at_backing_changed_publication") == 0 ||
               strcmp(argv[1], "record_at_backing_wrong_locator") == 0) {
        struct mem_service_record record;
        const struct mem_service_record *published =
            &((struct mem_service_cluster_payload *)exported)->records[0];
        uint32_t cookie =
            (uint32_t)(published->object_payload_checksum ^
                       (published->object_payload_checksum >> 32));
        uint32_t locator =
            strcmp(argv[1], "record_at_backing_wrong_locator") == 0 ? 2U : 1U;

        memset(&record, 0, sizeof(record));
        change_publication =
            strcmp(argv[1], "record_at_backing_changed_publication") == 0;
        success = mem_service_model_refresh_remote_record_at_obmm_object_backing(
            &runtime,
            &runtime.slots[1],
            locator,
            MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT,
            MEM_SERVICE_OBMM_KIND_MODEL_TOKEN_RESULT,
            published->object_backing_offset,
            published->object_backing_len,
            cookie,
            &record);
        assert(success == (locator == 1U && !change_publication));
        if (success) {
            assert(record.kind == MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT);
            assert(record.object_backing_len == sizeof(uint64_t) * 8U);
        }
        assert(sync_calls == (locator == 1U ? 3U : 1U));
        printf("case=%s status=ok sync_calls=%u\n", argv[1], sync_calls);
        free(imported);
        free(exported);
        return 0;
    } else if (strcmp(argv[1], "stale_import") == 0) {
        success = true;
    } else if (strcmp(argv[1], "local") == 0) {
        runtime.node_count = 1;
        runtime.slots[0].region.addr = exported;
        runtime.slots[0].region.len = region_bytes;
        success = true;
    } else if (strcmp(argv[1], "checksum") == 0) {
        exported[region_bytes - 1] ^= 1;
    } else if (strcmp(argv[1], "metadata_sync_failure") == 0) {
        fail_sync_call = 1;
    } else if (strcmp(argv[1], "payload_sync_failure") == 0) {
        fail_sync_call = 4;
    } else if (strcmp(argv[1], "torn_publication") == 0) {
        ((struct mem_service_cluster_payload *)exported)->publish_done_seq = 0;
    } else if (strcmp(argv[1], "changed_publication") == 0) {
        change_publication = true;
    } else if (strcmp(argv[1], "record_bounds") == 0) {
        ((struct mem_service_cluster_payload *)exported)->records[0]
            .object_backing_offset = region_bytes - 1;
    } else if (strcmp(argv[1], "metadata_bounds") == 0) {
        runtime.slots[1].region.len = 1;
    } else if (strcmp(argv[1], "address_overflow") == 0) {
        runtime.payload_offset = UINT64_MAX - 1;
    } else {
        assert(!"unknown test case");
    }

    rc = read_token(1, &token);
    if (success) {
        assert(rc == 0 && token == 22);
        assert(sync_calls == (runtime.node_count == 1 ? 0U : 4U));
    } else {
        assert(rc != 0 && token == 11);
    }
    printf("case=%s status=ok sync_calls=%u\n", argv[1], sync_calls);
    free(imported);
    free(exported);
    return 0;
}
