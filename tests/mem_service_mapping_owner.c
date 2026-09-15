#define _POSIX_C_SOURCE 200809L
#include "mem_service_mapping_owner.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct fixture_object {
    char key[16], session[16], release_id[80];
    unsigned char bytes[4096];
    unsigned state, queries, unmaps, releases, unmap_failures, lost_releases;
    uint64_t id, generation;
    bool held;
};
static struct fixture_object objects[2];
static const struct mem_service_provider_ops provider_ops = {0};
static struct mem_service_provider provider = {
    .capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
    .state = MEM_SERVICE_PROVIDER_STATE_READY, .ops = &provider_ops,
};
static struct mem_service_provider_channel channel = {
    .provider = &provider, .required_capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
};

/* Only control/provider boundaries are replaced; the owner and its mutex,
 * admission, accounting and cleanup sequence are the production code. */
static struct fixture_object *lookup(const char *key, const char *session, uint64_t generation)
{
    for (unsigned i = 0; i < 2; ++i)
        if (!strcmp(key, objects[i].key) && !strcmp(session, objects[i].session) &&
            generation == objects[i].generation) return &objects[i];
    assert(!"unexpected object identity");
    return NULL;
}

int mem_service_client_mapping_transition(
    const struct mem_service_client *client, const char *key, const char *session,
    uint64_t generation, uint64_t id, enum mem_service_client_mapping_action action,
    const char *operation, struct mem_service_client_mapping_transaction *out,
    enum mem_service_wire_status *status)
{
    assert(!strcmp(client->connect_spec, "unix:/fixture"));
    assert(action == MEM_SERVICE_CLIENT_MAPPING_INSPECT && operation && operation[0]);
    struct fixture_object *o = lookup(key, session, generation);
    assert(o->id == id);
    ++o->queries;
    *out = (struct mem_service_client_mapping_transaction){
        .mapping_id = id, .generation = generation, .state = o->state,
    };
    if (status) *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_client_unmap_managed_allocation(
    const struct mem_service_client *client, const struct mem_service_provider_channel *ch,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_mapping_lifecycle *lifecycle,
    enum mem_service_wire_status *status)
{
    (void)client;
    assert(ch->provider == &provider && mapping->binding.mapped && lifecycle->pending);
    struct fixture_object *o = lookup(mapping->key, lifecycle->session_id, mapping->generation);
    ++o->unmaps;
    mapping->base = NULL;
    mapping->len = mapping->flags = 0;
    if (o->unmap_failures) {
        --o->unmap_failures;
        if (status) *status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
        return MEM_SERVICE_MAPPING_CLEANUP_REQUIRED;
    }
    mapping->binding.mapped = false;
    lifecycle->pending = false;
    o->state = 0;
    if (status) *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_client_release_object(
    const struct mem_service_client *client, const char *key, const char *operation,
    const char *session, bool has_generation, uint64_t generation,
    struct mem_service_client_allocation *out, enum mem_service_wire_status *status)
{
    (void)client;
    struct fixture_object *o = lookup(key, session, generation);
    assert(has_generation && o->state == 0);
    ++o->releases;
    if (o->release_id[0]) assert(!strcmp(o->release_id, operation));
    else snprintf(o->release_id, sizeof(o->release_id), "%s", operation);
    o->held = false;
    if (o->lost_releases) {
        --o->lost_releases;
        if (status) *status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
        return -EIO;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->key, sizeof(out->key), "%s", key);
    out->generation = generation;
    if (status) *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

static void mapping_inputs(unsigned i, struct mem_service_client_object_mapping *mapping,
                           struct mem_service_client_mapping_lifecycle *lifecycle)
{
    struct fixture_object *o = &objects[i];
    memset(o, 0, sizeof(*o));
    snprintf(o->key, sizeof(o->key), "object-%u", i);
    if (i) snprintf(o->key, sizeof(o->key), "对象 1=ok");
    snprintf(o->session, sizeof(o->session), "session-%u", i);
    o->id = 10 + i; o->generation = 20 + i; o->state = 2; o->held = true;
    memset(o->bytes, 23 + i, sizeof(o->bytes));
    memset(mapping, 0, sizeof(*mapping));
    memcpy(mapping->key, o->key, sizeof(o->key));
    mapping->generation = o->generation;
    mapping->base = o->bytes; mapping->len = sizeof(o->bytes);
    mapping->flags = i ? MEM_SERVICE_CLIENT_MAP_READ :
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE;
    mapping->binding = (struct mem_service_provider_mapping_binding){
        .owner = &provider, .mapped = true,
        .mapping = {.base = o->bytes, .len = sizeof(o->bytes), .handle = o->id},
    };
    memset(lifecycle, 0, sizeof(*lifecycle));
    snprintf(lifecycle->operation_id, sizeof(lifecycle->operation_id), "map-%u", i);
    memcpy(lifecycle->session_id, o->session, sizeof(o->session));
    lifecycle->pending = true;
    lifecycle->transaction = (struct mem_service_client_mapping_transaction){
        .mapping_id = o->id, .generation = o->generation, .state = 2,
    };
}

struct thread_group {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    unsigned ready, callbacks, active_callbacks;
    bool drain;
};
struct worker { struct thread_group *group; struct mem_service_mapping_owner *owner; unsigned index; };

static int counted_callback(void *opaque, const struct mem_service_provider_channel *ch,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    (void)ch; (void)mapping; (void)lifecycle; (void)view;
    ++*(unsigned *)opaque;
    return 0;
}

static int serialized_callback(void *opaque, const struct mem_service_provider_channel *ch,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct worker *w = opaque;
    assert(ch->provider == &provider && lifecycle->pending && mapping->binding.mapped);
    assert(view->base == (unsigned char *)mapping->base + view->offset);
    assert(w->group->active_callbacks++ == 0);
    const struct timespec delay = {.tv_nsec = 100000};
    nanosleep(&delay, NULL);
    assert(*(unsigned char *)view->base == 23 + w->index % 2);
    ++w->group->callbacks;
    assert(--w->group->active_callbacks == 0);
    return 0;
}

static void *worker_main(void *opaque)
{
    struct worker *w = opaque;
    struct mem_service_mapping_lease *lease;
    struct mem_service_mapping_access_view view;
    assert(!mem_service_mapping_owner_acquire(w->owner, MEM_SERVICE_MAPPING_ACCESS_CPU,
        w->index * 16, 16, MEM_SERVICE_CLIENT_MAP_READ, &lease, &view));
    pthread_mutex_lock(&w->group->lock);
    ++w->group->ready;
    pthread_cond_broadcast(&w->group->condition);
    while (!w->group->drain) pthread_cond_wait(&w->group->condition, &w->group->lock);
    pthread_mutex_unlock(&w->group->lock);
    for (unsigned i = 0; i < 100; ++i)
        assert(!mem_service_mapping_lease_apply(lease, serialized_callback, w));
    assert(!mem_service_mapping_lease_release(lease));
    return NULL;
}

static int self_test(void)
{
    char connect[] = "unix:/fixture";
    struct mem_service_client client = {.connect_spec = connect};
    struct mem_service_access_domain *domain;
    assert(!mem_service_access_domain_create(&client, &channel, &domain));
    connect[0] = 'X'; /* The domain owns its connect string snapshot. */
    struct mem_service_client_object_mapping mapping[2], original[2];
    struct mem_service_client_mapping_lifecycle lifecycle[2], old_lifecycle[2];
    struct mem_service_mapping_owner *owner[2], *rejected = NULL;
    enum mem_service_wire_status status;
    for (unsigned i = 0; i < 2; ++i) {
        mapping_inputs(i, &mapping[i], &lifecycle[i]);
        original[i] = mapping[i]; old_lifecycle[i] = lifecycle[i];
        assert(mem_service_mapping_owner_adopt(domain, &mapping[i], &lifecycle[i],
            "bad\nrelease", 8, &rejected, &status) == -EINVAL);
        assert(mem_service_mapping_owner_adopt(domain, &mapping[i], &lifecycle[i],
            "unused", 0, &rejected, &status) == -EINVAL);
        objects[i].state = 3;
        assert(mem_service_mapping_owner_adopt(domain, &mapping[i], &lifecycle[i],
            "unused", 8, &rejected, &status) == -ESTALE);
        assert(!rejected && status == MEM_SERVICE_WIRE_STATUS_STALE_REF);
        assert(!memcmp(&mapping[i], &original[i], sizeof(mapping[i])));
        assert(!memcmp(&lifecycle[i], &old_lifecycle[i], sizeof(lifecycle[i])));
        objects[i].state = 2;
        assert(!mem_service_mapping_owner_adopt(domain, &mapping[i], &lifecycle[i],
            i ? "release-1" : "release-0", 8, &owner[i], &status));
        assert(!mapping[i].binding.mapped && !mapping[i].base && !mapping[i].key[0]);
        assert(!lifecycle[i].pending && !lifecycle[i].operation_id[0]);
        assert(mem_service_mapping_owner_adopt(domain, &original[i], &old_lifecycle[i],
            "duplicate", 8, &rejected, &status) == -EEXIST);
        assert(mem_service_mapping_owner_destroy(owner[i]) == -EBUSY);
    }
    assert(mem_service_access_domain_destroy(domain) == -EBUSY);
    struct mem_service_mapping_lease *lease = NULL, *pins[8];
    struct mem_service_mapping_access_view view;
    assert(mem_service_mapping_owner_acquire(owner[1], MEM_SERVICE_MAPPING_ACCESS_CPU,
        0, 16, MEM_SERVICE_CLIENT_MAP_WRITE, &lease, &view) == -EACCES);
    assert(!lease && !view.base);
    assert(mem_service_mapping_owner_acquire(owner[0], MEM_SERVICE_MAPPING_ACCESS_CPU,
        UINT64_MAX, 16, MEM_SERVICE_CLIENT_MAP_READ, &lease, &view) == -ERANGE);
    assert(mem_service_mapping_owner_acquire(owner[0], MEM_SERVICE_MAPPING_ACCESS_CPU,
        4090, 16, MEM_SERVICE_CLIENT_MAP_READ, &lease, &view) == -ERANGE);
    for (unsigned i = 0; i < 8; ++i)
        assert(!mem_service_mapping_owner_acquire(owner[0], MEM_SERVICE_MAPPING_ACCESS_CPU,
            0, 16, MEM_SERVICE_CLIENT_MAP_READ, &pins[i], &view));
    assert(mem_service_mapping_owner_acquire(owner[0], MEM_SERVICE_MAPPING_ACCESS_CPU,
        0, 16, MEM_SERVICE_CLIENT_MAP_READ, &lease, &view) == -ENOSPC);
    assert(!lease && !view.base);
    assert(mem_service_mapping_lease_begin(pins[0], 77) == -EINVAL);
    for (unsigned i = 0; i < 8; ++i) assert(!mem_service_mapping_lease_release(pins[i]));
    assert(!mem_service_mapping_owner_acquire(owner[0], MEM_SERVICE_MAPPING_ACCESS_COMPUTE,
        4096 - 16, 16, MEM_SERVICE_CLIENT_MAP_READ, &lease, &view));
    unsigned callbacks = 0;
    assert(!mem_service_mapping_lease_apply(lease, counted_callback, &callbacks));
    assert(callbacks == 1);
    assert(!mem_service_mapping_lease_begin(lease, 77));
    assert(mem_service_mapping_lease_apply(lease, counted_callback, &callbacks) == -EBUSY);
    assert(callbacks == 1);
    assert(mem_service_mapping_lease_begin(lease, 78) == -EBUSY);
    assert(mem_service_mapping_lease_complete(lease, 78) == -ESTALE);
    assert(mem_service_mapping_lease_release(lease) == -EBUSY);

    struct thread_group group = {
        .lock = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
    };
    struct worker workers[8];
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) {
        workers[i] = (struct worker){.group = &group, .owner = owner[i % 2], .index = i};
        assert(!pthread_create(&threads[i], NULL, worker_main, &workers[i]));
    }
    pthread_mutex_lock(&group.lock);
    while (group.ready != 8) pthread_cond_wait(&group.condition, &group.lock);
    pthread_mutex_unlock(&group.lock);
    for (unsigned i = 0; i < 2; ++i) {
        struct mem_service_mapping_owner_stats stats;
        assert(!mem_service_mapping_owner_inspect(owner[i], &stats));
        assert(stats.cpu_pins == 4 && stats.compute_pins == (i ? 0 : 1));
        assert(stats.in_flight_pins == (i ? 0 : 1));
        assert(mem_service_mapping_owner_unmap(owner[i], &status) == -EBUSY);
        assert(mem_service_mapping_owner_close(owner[i], &status) == -EBUSY);
        assert(objects[i].unmaps == 0 && objects[i].held);
        struct mem_service_mapping_lease *blocked;
        assert(mem_service_mapping_owner_acquire(owner[i], MEM_SERVICE_MAPPING_ACCESS_CPU,
            0, 16, MEM_SERVICE_CLIENT_MAP_READ, &blocked, &view) == -ECANCELED);
        assert(!blocked);
    }
    assert(mem_service_mapping_lease_release(lease) == -EBUSY);
    assert(!mem_service_mapping_lease_complete(lease, 77));
    assert(!mem_service_mapping_lease_apply(lease, counted_callback, &callbacks));
    assert(callbacks == 2);
    assert(mem_service_mapping_lease_complete(lease, 77) == -ESTALE);
    assert(mem_service_mapping_lease_begin(lease, 78) == -ECANCELED);
    assert(!mem_service_mapping_lease_release(lease));
    pthread_mutex_lock(&group.lock);
    group.drain = true;
    pthread_cond_broadcast(&group.condition);
    pthread_mutex_unlock(&group.lock);
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(group.callbacks == 800 && !group.active_callbacks);
    assert(!pthread_cond_destroy(&group.condition));
    assert(!pthread_mutex_destroy(&group.lock));

    objects[0].unmap_failures = 1;
    objects[0].lost_releases = 1;
    struct mem_service_mapping_owner_stats stats;
    assert(mem_service_mapping_owner_unmap(owner[0], &status) == MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
    assert(!mem_service_mapping_owner_inspect(owner[0], &stats));
    assert(stats.closing && stats.mapping_pending && stats.holder_pending);
    assert(!stats.cpu_pins && !stats.compute_pins && !stats.in_flight_pins);
    assert(objects[0].held && !objects[0].releases && objects[0].unmaps == 1);
    assert(mem_service_mapping_owner_destroy(owner[0]) == -EBUSY);
    assert(!mem_service_mapping_owner_unmap(owner[0], &status));
    assert(status == MEM_SERVICE_WIRE_STATUS_OK);
    assert(!mem_service_mapping_owner_unmap(owner[0], &status));
    assert(!mem_service_mapping_owner_inspect(owner[0], &stats));
    assert(stats.closing && !stats.mapping_pending && stats.holder_pending);
    assert(objects[0].held && !objects[0].releases && objects[0].unmaps == 2);
    assert(mem_service_mapping_owner_destroy(owner[0]) == -EBUSY);
    assert(mem_service_mapping_owner_acquire(owner[0], MEM_SERVICE_MAPPING_ACCESS_CPU,
        0, 16, MEM_SERVICE_CLIENT_MAP_READ, &lease, &view) == -ECANCELED);
    assert(!lease && !view.base);
    assert(mem_service_mapping_owner_close(owner[0], &status) == MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
    assert(!mem_service_mapping_owner_inspect(owner[0], &stats));
    assert(!stats.mapping_pending && stats.holder_pending);
    assert(!objects[0].held && objects[0].releases == 1 && objects[0].unmaps == 2);
    assert(!mem_service_mapping_owner_close(owner[0], &status));
    assert(!mem_service_mapping_owner_unmap(owner[0], &status));
    assert(!mem_service_mapping_owner_close(owner[0], &status));
    assert(objects[0].releases == 2 && objects[0].unmaps == 2);
    assert(!mem_service_mapping_owner_close(owner[1], &status));
    assert(objects[1].releases == 1 && objects[1].unmaps == 1);
    assert(mem_service_mapping_owner_adopt(domain, &original[1], &old_lifecycle[1],
        "late-snapshot", 8, &rejected, &status) == -ESTALE);
    assert(!rejected && original[1].binding.mapped && old_lifecycle[1].pending);
    assert(!mem_service_mapping_owner_destroy(owner[0]));
    assert(!mem_service_mapping_owner_destroy(owner[1]));
    assert(!mem_service_access_domain_destroy(domain));
    puts("mapping_owner_native=pass threads=8 owners=2 callbacks=800 scope=boundary-fixture");
    puts("mapping_owner_unmap=pass holder_retained=1 retry_idempotent=1 admission_closed=1");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--self-test")) {
        fprintf(stderr, "usage: mapping-owner --self-test\n");
        return 2;
    }
    return self_test();
}
