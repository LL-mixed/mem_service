#define _POSIX_C_SOURCE 200809L
#include "mem_service_provider_obmm_worker_ledger.h"
#include <assert.h>
#include <stdio.h>

static void reseal(unsigned char *frame)
{
    ledger_u64(frame + WORKER_LEDGER_FRAME_BYTES - 8,
               ledger_checksum(frame, WORKER_LEDGER_FRAME_BYTES - 8), true);
}

int main(int argc, char **argv)
{
    struct worker_ledger_entry in = {0}, out;
    unsigned char frame[WORKER_LEDGER_FRAME_BYTES], copy[WORKER_LEDGER_FRAME_BYTES];
    size_t i;
    assert(argc == 3 && !strcmp(argv[1], "--case"));
    in.sequence = 123;
    strcpy(in.phase, "published");
    strcpy(in.config.node, "node-identity");
    in.config.incarnation = UINT64_MAX - 1;
    in.config.readiness_generation = 37;
    in.config.allocation_granularity_bytes = 2097152;
    strcpy(in.work.key, "object-identity");
    in.work.generation = UINT64_C(0x1234567887654321);
    in.work.size_bytes = 65536;
    in.work.alignment_bytes = 4194304;
    in.work.capabilities = 1;
    in.reservation.occupied = true;
    in.reservation.segment = (struct obmm_gsva_segment_desc_v1){
        .version = 1, .flags = 7, .segment_id = UINT64_C(0xc4c2000000000123),
        .home_va = UINT64_C(0x700000000000), .size = 2097152, .epoch = 67,
        .home_cna = 0xc4c2, .owner_node_id = 3, .node_count = 8,
        .cache_policy = 2, .p_tag = 4, .access_flags = 3,
        .token_id = 98, .token_value = 101
    };
    in.reservation.exported.mem_id = 76;
    in.reservation.exported.tokenid = 987;
    in.reservation.exported.uba = in.reservation.segment.home_va;
    in.reservation.exported.length = 1;
    in.reservation.exported.flags = 5;
    in.reservation.exported.pxm_numa = -1;
    for (i = 0; i < OBMM_MAX_LOCAL_NUMA_NODES; i++)
        in.reservation.exported.size[i] = (i + 1) * 4096;
    in.reservation.exported.deid[15] = 17;
    in.reservation.exported.seid[0] = 29;
    in.reservation.descriptor.len = 96;
    for (i = 0; i < 96; i++) in.reservation.descriptor.bytes[i] = (unsigned char)(i * 17);
    assert(!ledger_encode(frame, &in));
    assert(!memcmp(frame, "obmm-worker-ledger-v1", sizeof("obmm-worker-ledger-v1") - 1));
    if (!strcmp(argv[2], "roundtrip")) {
        assert(!ledger_decode(frame, &out));
        assert(out.sequence == in.sequence);
        assert(!strcmp(out.phase, in.phase));
        assert(!strcmp(out.config.node, in.config.node));
        assert(out.config.incarnation == in.config.incarnation);
        assert(!strcmp(out.work.key, in.work.key));
        assert(out.work.generation == in.work.generation);
        assert(!memcmp(&out.reservation.segment, &in.reservation.segment,
                       sizeof(in.reservation.segment)));
        assert(!memcmp(&out.reservation.exported, &in.reservation.exported,
                       sizeof(in.reservation.exported)));
        assert(!memcmp(out.reservation.descriptor.bytes, in.reservation.descriptor.bytes, 96));
        assert(!ledger_encode(copy, &out) && !memcmp(copy, frame, sizeof(frame)));
    } else if (!strcmp(argv[2], "corruption")) {
        for (i = 0; i < sizeof(frame); i++) {
            memcpy(copy, frame, sizeof(frame));
            copy[i] ^= 1;
            assert(ledger_decode(copy, &out));
        }
    } else if (!strcmp(argv[2], "canonical")) {
        for (i = 21; i < 24; i++) {
            memcpy(copy, frame, sizeof(frame)); copy[i] = 1; reseal(copy);
            assert(ledger_decode(copy, &out));
        }
        memcpy(copy, frame, sizeof(frame)); copy[900] = 1; reseal(copy);
        assert(ledger_decode(copy, &out));
        memcpy(copy, frame, sizeof(frame)); copy[63] = 1; reseal(copy);
        assert(ledger_decode(copy, &out)); /* noncanonical phase padding */
        in.reservation.exported.priv = &in;
        assert(ledger_encode(copy, &in));
        in.reservation.exported.priv = NULL;
        in.reservation.descriptor.len = MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN + 1;
        assert(ledger_encode(copy, &in));
        in.reservation.descriptor.len = 0;
        memset(in.work.key, 'x', sizeof(in.work.key));
        assert(ledger_encode(copy, &in));
    } else return 2;
    printf("worker_ledger_%s=pass\n", argv[2]);
    return 0;
}
