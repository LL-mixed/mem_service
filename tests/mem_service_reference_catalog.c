/* SPDX-License-Identifier: MIT */
#include "mem_service_object_refs.h"
#include "mem_service_record_table.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OK(call) assert((call) == MEM_SERVICE_MANAGED_RESULT_OK)
#define BAD(call) assert((call) != MEM_SERVICE_MANAGED_RESULT_OK)

static struct mem_service *fresh(struct mem_service_managed_view *view)
{
    struct mem_service *svc = calloc(1, sizeof(*svc));
    const struct mem_service_managed_request request = {
        .key = "allocation-1", .idempotency_key = "allocate-1", .session_id = "writer",
        .size_bytes = 65536, .alignment_bytes = 4096,
        .capabilities = MEM_SERVICE_MANAGED_CAP_MAP,
        .home_node_id = "home-1", .home_incarnation = 7,
    };
    const uint8_t descriptor[] = {1, 2, 3, 4};
    assert(svc);
    mem_service_managed_table_init(&svc->managed);
    OK(mem_service_managed_allocate(&svc->managed, &request, view));
    OK(mem_service_managed_publish(&svc->managed, request.key, "home-1", 7,
        view->generation, descriptor, sizeof(descriptor), 0x100000, 65536, view));
    return svc;
}

static struct lingqu_object_ref_wire_v2 reference(const struct mem_service_managed_view *view)
{
    struct lingqu_object_ref_wire_v2 ref = {0};
    ref.object.magic = LINGQU_OBJECT_REF_MAGIC;
    ref.object.layout_version = LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    ref.object.object_kind = 5;
    ref.object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    ref.object.owner_entity = 1;
    ref.object.producer_entity = 2;
    ref.object.object_version = view->version;
    ref.object.key_hash = lingqu_object_ref_key_hash(view->key, strlen(view->key));
    ref.object.payload_offset = 4090;
    ref.object.payload_bytes = 512;
    ref.object.payload_checksum = 42;
    ref.wire_bytes = LINGQU_OBJECT_REF_V2_BYTES;
    ref.access = LINGQU_OBJECT_REF_V2_READ;
    ref.allocation_generation = view->generation;
    ref.provider_incarnation = view->provider_incarnation;
    ref.allocation_bytes = view->size_bytes;
    strcpy(ref.allocation_key, view->key);
    strcpy(ref.home_node, view->home_node_id);
    return ref;
}

static void self_test(void)
{
    struct mem_service_managed_view view, output, saved_output;
    struct mem_service *svc = fresh(&view);
    struct mem_service_managed_mapping mapping;
    struct lingqu_object_ref_wire_v2 a, b, changed, resolved, saved_ref;
    struct lingqu_object_ref_wire legacy;
    uint64_t generation = view.generation;
    BAD(mem_service_managed_content_begin(&svc->managed, view.key, "writer", generation, 1, &view));
    OK(mem_service_managed_acquire(&svc->managed, "allocation-1", "writer", true, generation, &view));
    OK(mem_service_managed_acquire(&svc->managed, view.key, "reader", true, generation, &view));
    BAD(mem_service_managed_content_begin(&svc->managed, view.key, "writer", generation, 1, &view));
    OK(mem_service_managed_release(&svc->managed, view.key, "reader", true, generation, &view));
    BAD(mem_service_managed_content_begin(&svc->managed, view.key, "other", generation, 1, &view));
    OK(mem_service_managed_mapping_transition(&svc->managed, view.key, "writer", generation,
        0, MEM_SERVICE_MANAGED_MAPPING_BEGIN, &mapping));
    BAD(mem_service_managed_content_begin(&svc->managed, view.key, "writer", generation, 1, &view));
    OK(mem_service_managed_mapping_transition(&svc->managed, view.key, "writer", generation,
        mapping.id, MEM_SERVICE_MANAGED_MAPPING_CANCEL, &mapping));
    OK(mem_service_managed_content_begin(&svc->managed, view.key, "writer", generation, 1, &view));
    assert(view.version == 2);
    BAD(mem_service_managed_content_begin(&svc->managed, view.key, "writer", generation, 2, &view));
    BAD(mem_service_managed_acquire(&svc->managed, view.key, "reader", true, generation, &output));
    BAD(mem_service_reference_seal(svc, view.key, "writer", generation, 2));
    a = reference(&view);
    b = a; b.object.payload_offset = 8192; b.object.payload_checksum = 43;
    BAD(mem_service_reference_stage(svc, "logical/a", "other", &a));
    changed = a; ++changed.provider_incarnation;
    BAD(mem_service_reference_stage(svc, "logical/a", "writer", &changed));
    assert(svc->record_count == 0);
    OK(mem_service_reference_stage(svc, "logical/a", "writer", &a));
    OK(mem_service_reference_stage(svc, "logical/b", "writer", &b));
    OK(mem_service_reference_stage(svc, "logical/a", "writer", &a));
    assert(svc->record_count == 2);
    changed = a; ++changed.object.payload_checksum;
    BAD(mem_service_reference_stage(svc, "logical/a", "writer", &changed));
    memset(&resolved, 0xa5, sizeof(resolved)); saved_ref = resolved;
    BAD(mem_service_reference_resolve(svc, "logical/a", &resolved));
    assert(!memcmp(&resolved, &saved_ref, sizeof(resolved)));
    OK(mem_service_managed_mapping_transition(&svc->managed, view.key, "writer", generation,
        0, MEM_SERVICE_MANAGED_MAPPING_BEGIN, &mapping));
    BAD(mem_service_reference_seal(svc, view.key, "writer", generation, 2));
    OK(mem_service_managed_mapping_transition(&svc->managed, view.key, "writer", generation,
        mapping.id, MEM_SERVICE_MANAGED_MAPPING_CONFIRM, &mapping));
    BAD(mem_service_reference_seal(svc, view.key, "writer", generation, 2));
    OK(mem_service_managed_mapping_transition(&svc->managed, view.key, "writer", generation,
        mapping.id, MEM_SERVICE_MANAGED_MAPPING_CLOSE, &mapping));
    BAD(mem_service_reference_seal(svc, view.key, "writer", generation, 2));
    OK(mem_service_managed_mapping_transition(&svc->managed, view.key, "writer", generation,
        mapping.id, MEM_SERVICE_MANAGED_MAPPING_FINISH, &mapping));
    OK(mem_service_reference_seal(svc, view.key, "writer", generation, 2));
    OK(mem_service_reference_resolve(svc, "logical/a", &resolved));
    assert(!memcmp(&resolved, &a, sizeof(a)));
    OK(mem_service_reference_resolve(svc, "logical/b", &resolved));
    assert(!memcmp(&resolved, &b, sizeof(b)));
    BAD(mem_service_managed_acquire(&svc->managed, view.key, "reader", true, generation, &output));
    BAD(mem_service_managed_mapping_transition(&svc->managed, view.key, "writer", generation,
        0, MEM_SERVICE_MANAGED_MAPPING_BEGIN, &mapping));
    BAD(mem_service_record_to_lingqu_object_ref(mem_service_find_record(svc, "logical/a"), &legacy));

    /* Each mutation still passes the codec. None has an authoritative binding. */
    for (unsigned i = 0; i < 12; ++i) {
        changed = a;
        switch (i) {
        case 0: ++changed.allocation_generation; break;
        case 1: ++changed.object.object_version; break;
        case 2: ++changed.provider_incarnation; break;
        case 3: changed.home_node[0] = 'H'; break;
        case 4: ++changed.allocation_bytes; break;
        case 5: ++changed.object.payload_offset; break;
        case 6: --changed.object.payload_bytes; break;
        case 7: ++changed.object.payload_checksum; break;
        case 8: changed.access = 3; break;
        case 9: ++changed.object.owner_entity; break;
        case 10: ++changed.object.producer_entity; break;
        case 11: ++changed.object.object_kind; break;
        }
        assert(!lingqu_object_ref_v2_validate(&changed));
        memset(&output, 0xa5, sizeof(output)); saved_output = output;
        BAD(mem_service_reference_acquire(svc, &changed, "reader", 1, &output));
        assert(!memcmp(&output, &saved_output, sizeof(output)));
        OK(mem_service_managed_inspect(&svc->managed, view.key, &view));
        assert(view.holder_count == 1 && svc->record_count == 2);
    }
    BAD(mem_service_reference_acquire(svc, &a, "reader", 2, &output));
    OK(mem_service_reference_acquire(svc, &a, "reader", 1, &output));
    assert(output.holder_count == 2 && output.version == 2 && output.generation == generation);
    BAD(mem_service_managed_content_begin(&svc->managed, view.key, "writer", generation, 2, &view));
    OK(mem_service_managed_release(&svc->managed, view.key, "reader", true, generation, &view));

    /* Repeated publications retain one allocation and two logical record slots. */
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        uint64_t previous = view.version;
        OK(mem_service_managed_content_begin(&svc->managed, view.key, "writer",
                                             generation, previous, &view));
        BAD(mem_service_reference_acquire(svc, &a, "reader", 1, &output));
        BAD(mem_service_reference_resolve(svc, "logical/a", &resolved));
        a = reference(&view); a.object.payload_checksum += cycle;
        b = a; b.object.payload_offset = 8192; ++b.object.payload_checksum;
        OK(mem_service_reference_stage(svc,
            mem_service_find_record(svc, "logical/a")->key, "writer", &a));
        OK(mem_service_reference_stage(svc, "logical/b", "writer", &b));
        OK(mem_service_reference_seal(svc, view.key, "writer", generation, view.version));
        OK(mem_service_reference_acquire(svc, &a, "reader", 1, &output));
        OK(mem_service_managed_release(&svc->managed, view.key, "reader", true, generation, &view));
        assert(view.version == previous + 1 && svc->record_count == 2);
    }
    svc->managed.entries[0].version = UINT64_MAX;
    BAD(mem_service_managed_content_begin(&svc->managed, view.key, "writer", generation, UINT64_MAX, &view));
    assert(!svc->managed.entries[0].content_writing);
    svc->managed.entries[0].version = a.object.object_version;
    OK(mem_service_managed_retire(&svc->managed, view.key, true, generation, &view));
    BAD(mem_service_reference_acquire(svc, &a, "reader", 1, &output));
    OK(mem_service_managed_release(&svc->managed, view.key, "writer", true, generation, &view));
    OK(mem_service_managed_reclaim(&svc->managed, view.key, "home-1", 7, generation, true, &view));
    assert(view.state == MEM_SERVICE_MANAGED_STATE_RETIRED && view.holder_count == 0);
    {
        const struct mem_service_managed_request replacement = {
            .key = "allocation-1", .idempotency_key = "allocate-2", .session_id = "writer",
            .size_bytes = 65536, .alignment_bytes = 4096,
            .capabilities = MEM_SERVICE_MANAGED_CAP_MAP,
            .home_node_id = "home-2", .home_incarnation = 8,
        };
        const uint8_t descriptor[] = {5, 6, 7, 8};
        OK(mem_service_managed_allocate(&svc->managed, &replacement, &view));
        assert(view.generation != generation && view.version == 1);
        OK(mem_service_managed_publish(&svc->managed, view.key, "home-2", 8,
            view.generation, descriptor, sizeof(descriptor), 0x100000, 65536, &view));
        BAD(mem_service_reference_acquire(svc, &a, "reader", 1, &output));
        OK(mem_service_managed_acquire(&svc->managed, view.key, "writer", true, view.generation, &view));
        OK(mem_service_managed_content_begin(&svc->managed, view.key, "writer", view.generation, 1, &view));
        BAD(mem_service_reference_stage(svc, "logical/a", "writer", &a));
        a = reference(&view);
        OK(mem_service_reference_stage(svc, "logical/a", "writer", &a));
        OK(mem_service_reference_seal(svc, view.key, "writer", view.generation, 2));
        OK(mem_service_reference_resolve(svc, "logical/a", &resolved));
        assert(!memcmp(&a, &resolved, sizeof(a)) && svc->record_count == 2);
        BAD(mem_service_reference_resolve(svc, "logical/b", &resolved));
        BAD(mem_service_reference_acquire(svc, &b, "reader", 1, &output));
    }
    free(svc);
}

static void capacity_and_corruption_test(void)
{
    struct mem_service_managed_view view;
    struct mem_service *svc = fresh(&view);
    struct lingqu_object_ref_wire_v2 ref, resolved;
    struct mem_service_record saved;
    OK(mem_service_managed_acquire(&svc->managed, view.key, "writer", true, view.generation, &view));
    OK(mem_service_managed_content_begin(&svc->managed, view.key, "writer", view.generation, 1, &view));
    ref = reference(&view);
    BAD(mem_service_reference_stage(svc, "invalid key", "writer", &ref));
    assert(svc->record_count == 0);
    OK(mem_service_reference_stage(svc, "logical/a", "writer", &ref));
    saved = svc->records[0];
    ++svc->records[0].object_payload_checksum;
    BAD(mem_service_reference_seal(svc, view.key, "writer", view.generation, 2));
    assert(svc->managed.entries[0].content_writing);
    svc->records[0] = saved;
    for (size_t i = 1; i < MEM_SERVICE_MAX_RECORDS; ++i) {
        svc->records[i].in_use = true;
        svc->records[i].kind = MEM_SERVICE_RECORD_EXECUTION_ARTIFACT;
        snprintf(svc->records[i].key, sizeof(svc->records[i].key), "legacy-%zu", i);
    }
    svc->record_count = MEM_SERVICE_MAX_RECORDS;
    BAD(mem_service_reference_stage(svc, "overflow", "writer", &ref));
    BAD(mem_service_reference_stage(svc, "legacy-1", "writer", &ref));
    assert(svc->record_count == MEM_SERVICE_MAX_RECORDS &&
           !memcmp(&saved, &svc->records[0], sizeof(saved)));
    OK(mem_service_reference_seal(svc, view.key, "writer", view.generation, 2));
    ++svc->records[0].managed_ref.object.payload_checksum;
    BAD(mem_service_reference_resolve(svc, "logical/a", &resolved));
    free(svc);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
        self_test();
        capacity_and_corruption_test();
        puts("reference_catalog=pass views=2 identity_mutations=12 publication_cycles=100 scope=core-metadata");
        return 0;
    }
    fprintf(stderr, "usage: reference-catalog --self-test\n");
    return 2;
}
