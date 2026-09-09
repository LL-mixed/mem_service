#include "mem_service_model_runtime.h"
#include "mem_service_object_contract.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct mem_service_cluster_runtime rt = {0};
    const uint64_t tier = MEM_SERVICE_OBMM_KV_STATE_BLOCK_TIER0_BYTES;
    uint64_t len = 40, offset = 0, block = 0, count = 0, reserved = 0;
    uint64_t check_block = 91, check_count = 92, check_reserved = 93;
    uint64_t arena_next;
    uint8_t *storage = calloc(1, 32U * 1024U * 1024U);
    const uint8_t *payload;
    int expected = -1;

    assert(argc == 2 && storage);
    rt.node_count = 2;
    rt.local_idx = 0;
    rt.slots[0].region.addr = storage;
    rt.slots[0].region.len = 32U * 1024U * 1024U;
    rt.payload_arena_base = tier;
    rt.payload_arena_next = tier;
    if (strcmp(argv[1], "multi_block") == 0) {
        len = 2 * MEM_SERVICE_OBMM_KV_STATE_BLOCK_TIER3_BYTES + 1;
        expected = 0;
    } else if (strcmp(argv[1], "valid") == 0) {
        expected = 0;
    }
    assert(mem_service_model_kv_state_alloc(&rt, len, &offset, &block,
                                            &count, &reserved) == 0);
    assert(offset % block == 0 && reserved == block * count && reserved >= len);
    if (strcmp(argv[1], "multi_block") == 0) {
        assert(count == 3);
    }
    payload = storage + offset;
    if (strcmp(argv[1], "wrong_pointer") == 0) {
        payload++;
    } else if (strcmp(argv[1], "unaligned") == 0) {
        offset++;
        payload++;
    } else if (strcmp(argv[1], "before_arena") == 0) {
        offset = 0;
        payload = storage;
    } else if (strcmp(argv[1], "unreserved_padding") == 0) {
        rt.payload_arena_next = offset + len;
    } else if (strcmp(argv[1], "slot_padding") == 0) {
        rt.slots[0].region.len = offset + len;
    } else if (strcmp(argv[1], "past_arena") == 0) {
        rt.payload_arena_next = offset - 1;
    } else if (strcmp(argv[1], "past_slot") == 0) {
        rt.slots[0].region.len = offset - 1;
    } else if (strcmp(argv[1], "length_overflow") == 0) {
        len = UINT64_MAX;
    } else if (strcmp(argv[1], "address_overflow") == 0) {
        rt.slots[0].region.addr = (void *)(UINTPTR_MAX - offset + 1);
    } else if (strcmp(argv[1], "empty") == 0) {
        len = 0;
    } else if (strcmp(argv[1], "bad_node") == 0) {
        rt.local_idx = rt.node_count;
    } else if (strcmp(argv[1], "too_many_nodes") == 0) {
        rt.node_count = MEM_SERVICE_CLUSTER_MAX_NODES + 1;
    } else if (strcmp(argv[1], "null_mapping") == 0) {
        rt.slots[0].region.addr = NULL;
    } else if (strcmp(argv[1], "null_payload") == 0) {
        payload = NULL;
    } else {
        assert(expected == 0);
    }
    arena_next = rt.payload_arena_next;
    assert(mem_service_model_kv_state_validate_reserved(
               &rt, payload, len, offset, &check_block, &check_count,
               &check_reserved) == expected);
    assert(rt.payload_arena_next == arena_next);
    if (expected == 0) {
        assert(check_block == block && check_count == count && check_reserved == reserved);
    } else {
        assert(check_block == 91 && check_count == 92 && check_reserved == 93);
    }
    free(storage);
    printf("case=%s status=ok\n", argv[1]);
    return 0;
}
