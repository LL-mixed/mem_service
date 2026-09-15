#define _POSIX_C_SOURCE 200809L
#include "mem_service_mapping_owner.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mem_service_access_domain {
    pthread_mutex_t lock;
    struct mem_service_client client;
    struct mem_service_provider_channel channel;
    char *connect_spec;
    struct mem_service_mapping_owner *owners;
};

struct mem_service_mapping_owner {
    struct mem_service_access_domain *domain;
    struct mem_service_mapping_owner *next;
    struct mem_service_client_object_mapping mapping;
    struct mem_service_client_mapping_lifecycle lifecycle;
    char release_operation[80];
    struct mem_service_mapping_owner_stats stats;
};

struct mem_service_mapping_lease {
    struct mem_service_mapping_owner *owner;
    struct mem_service_mapping_access_view view;
    enum mem_service_mapping_access_kind kind;
    uint64_t operation_id;
};

static int domain_lock(struct mem_service_access_domain *domain)
{
    int rc = pthread_mutex_lock(&domain->lock);
    return rc ? -rc : 0;
}

static void domain_unlock(struct mem_service_access_domain *domain)
{
    (void)pthread_mutex_unlock(&domain->lock);
}

static bool string_valid(const char *value, size_t capacity)
{
    if (!value || !value[0] || strnlen(value, capacity) >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < ' ' || *p == 127) return false;
    return true;
}

int mem_service_access_domain_create(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain **domain_out)
{
    if (!domain_out) return -EINVAL;
    *domain_out = NULL;
    if (!client || !client->connect_spec || !client->connect_spec[0] ||
        !channel || !channel->provider || !channel->provider->ops ||
        channel->provider->state != MEM_SERVICE_PROVIDER_STATE_READY ||
        !(channel->required_capabilities & MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING) ||
        !(channel->provider->capabilities & MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING))
        return -EINVAL;
    struct mem_service_access_domain *domain = calloc(1, sizeof(*domain));
    if (!domain) return -ENOMEM;
    domain->connect_spec = strdup(client->connect_spec);
    if (!domain->connect_spec) { free(domain); return -ENOMEM; }
    int rc = pthread_mutex_init(&domain->lock, NULL);
    if (rc) { free(domain->connect_spec); free(domain); return -rc; }
    domain->client = *client;
    domain->client.connect_spec = domain->connect_spec;
    domain->channel = *channel;
    *domain_out = domain;
    return 0;
}

int mem_service_access_domain_destroy(struct mem_service_access_domain *domain)
{
    if (!domain) return -EINVAL;
    int rc = domain_lock(domain);
    if (rc) return rc;
    bool busy = domain->owners != NULL;
    domain_unlock(domain);
    if (busy) return -EBUSY;
    rc = pthread_mutex_destroy(&domain->lock);
    if (rc) return -rc;
    free(domain->connect_spec);
    free(domain);
    return 0;
}

static bool mapping_valid(
    const struct mem_service_access_domain *domain,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle)
{
    const uint64_t rights = MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE;
    return mapping && lifecycle &&
        string_valid(mapping->key, sizeof(mapping->key)) && mapping->generation &&
        mapping->binding.mapped && mapping->binding.owner == domain->channel.provider &&
        mapping->binding.mapping.handle && mapping->base && mapping->len &&
        mapping->base == mapping->binding.mapping.base &&
        mapping->len == mapping->binding.mapping.len && mapping->flags &&
        !(mapping->flags & ~rights) &&
        mapping->len <= UINTPTR_MAX &&
        (uintptr_t)mapping->base <= UINTPTR_MAX - mapping->len &&
        lifecycle->pending && !lifecycle->terminal_action &&
        lifecycle->transaction.state == 2 && lifecycle->transaction.mapping_id &&
        lifecycle->transaction.generation == mapping->generation &&
        string_valid(lifecycle->operation_id, sizeof(lifecycle->operation_id)) &&
        string_valid(lifecycle->session_id, sizeof(lifecycle->session_id));
}

int mem_service_mapping_owner_adopt(
    struct mem_service_access_domain *domain,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_mapping_lifecycle *lifecycle,
    const char *release_operation_id, uint32_t pin_limit,
    struct mem_service_mapping_owner **owner_out,
    enum mem_service_wire_status *status_out)
{
    if (status_out) *status_out = MEM_SERVICE_WIRE_STATUS_INVALID_SESSION;
    if (!owner_out) return -EINVAL;
    *owner_out = NULL;
    if (!domain || !pin_limit || pin_limit > MEM_SERVICE_MAPPING_OWNER_MAX_PINS ||
        !string_valid(release_operation_id, 80) || !mapping_valid(domain, mapping, lifecycle))
        return -EINVAL;
    struct mem_service_mapping_owner *owner = calloc(1, sizeof(*owner));
    if (!owner) return -ENOMEM;
    int rc = domain_lock(domain);
    if (rc) { free(owner); return rc; }
    for (struct mem_service_mapping_owner *p = domain->owners; p; p = p->next) {
        if (!strcmp(p->release_operation, release_operation_id)) {
            rc = -EEXIST;
            goto failed;
        }
        if (!p->stats.holder_pending) continue;
        if (p->mapping.binding.mapping.handle == mapping->binding.mapping.handle ||
            p->lifecycle.transaction.mapping_id == lifecycle->transaction.mapping_id ||
            (p->mapping.generation == mapping->generation &&
             !strcmp(p->mapping.key, mapping->key) &&
             !strcmp(p->lifecycle.session_id, lifecycle->session_id))) {
            rc = -EEXIST;
            goto failed;
        }
    }
    struct mem_service_client_mapping_transaction current;
    char inspect_operation[96];
    snprintf(inspect_operation, sizeof(inspect_operation), "%s-%u",
             lifecycle->operation_id, (unsigned)MEM_SERVICE_CLIENT_MAPPING_INSPECT);
    rc = mem_service_client_mapping_transition(&domain->client, mapping->key,
        lifecycle->session_id, mapping->generation, lifecycle->transaction.mapping_id,
        MEM_SERVICE_CLIENT_MAPPING_INSPECT, inspect_operation, &current, status_out);
    if (rc) goto failed;
    if (current.state != 2 || current.mapping_id != lifecycle->transaction.mapping_id ||
        current.generation != mapping->generation) {
        if (status_out) *status_out = MEM_SERVICE_WIRE_STATUS_STALE_REF;
        rc = -ESTALE;
        goto failed;
    }
    owner->domain = domain;
    owner->mapping = *mapping;
    owner->lifecycle = *lifecycle;
    memcpy(owner->release_operation, release_operation_id, strlen(release_operation_id) + 1);
    owner->stats.pin_limit = pin_limit;
    owner->stats.mapping_pending = true;
    owner->stats.holder_pending = true;
    owner->next = domain->owners;
    domain->owners = owner;
    memset(mapping, 0, sizeof(*mapping));
    memset(lifecycle, 0, sizeof(*lifecycle));
    *owner_out = owner;
    domain_unlock(domain);
    return 0;
failed:
    domain_unlock(domain);
    free(owner);
    return rc;
}

int mem_service_mapping_owner_acquire(
    struct mem_service_mapping_owner *owner,
    enum mem_service_mapping_access_kind kind,
    uint64_t offset, uint64_t len, uint64_t flags,
    struct mem_service_mapping_lease **lease_out,
    struct mem_service_mapping_access_view *view_out)
{
    if (!lease_out || !view_out) return -EINVAL;
    *lease_out = NULL;
    memset(view_out, 0, sizeof(*view_out));
    const uint64_t rights = MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE;
    if (!owner || !len || !flags || (flags & ~rights) ||
        (kind != MEM_SERVICE_MAPPING_ACCESS_CPU && kind != MEM_SERVICE_MAPPING_ACCESS_COMPUTE))
        return -EINVAL;
    struct mem_service_access_domain *domain = owner->domain;
    int rc = domain_lock(domain);
    if (rc) return rc;
    if (owner->stats.closing || !owner->stats.mapping_pending) {
        rc = -ECANCELED;
        goto out;
    }
    if (flags & ~owner->mapping.flags) { rc = -EACCES; goto out; }
    if (offset > owner->mapping.len || len > owner->mapping.len - offset) {
        rc = -ERANGE;
        goto out;
    }
    if (owner->stats.cpu_pins + owner->stats.compute_pins >= owner->stats.pin_limit) {
        rc = -ENOSPC;
        goto out;
    }
    struct mem_service_mapping_lease *lease = calloc(1, sizeof(*lease));
    if (!lease) { rc = -ENOMEM; goto out; }
    lease->owner = owner;
    lease->kind = kind;
    lease->view = (struct mem_service_mapping_access_view){
        .base = (uint8_t *)owner->mapping.base + offset,
        .offset = offset, .len = len, .flags = flags,
        .generation = owner->mapping.generation,
    };
    if (kind == MEM_SERVICE_MAPPING_ACCESS_CPU) ++owner->stats.cpu_pins;
    else ++owner->stats.compute_pins;
    *view_out = lease->view;
    *lease_out = lease;
out:
    domain_unlock(domain);
    return rc;
}

int mem_service_mapping_lease_release(struct mem_service_mapping_lease *lease)
{
    if (!lease) return -EINVAL;
    struct mem_service_mapping_owner *owner = lease->owner;
    int rc = domain_lock(owner->domain);
    if (rc) return rc;
    if (lease->operation_id) { domain_unlock(owner->domain); return -EBUSY; }
    if (lease->kind == MEM_SERVICE_MAPPING_ACCESS_CPU) --owner->stats.cpu_pins;
    else --owner->stats.compute_pins;
    domain_unlock(owner->domain);
    free(lease);
    return 0;
}

int mem_service_mapping_lease_apply(
    struct mem_service_mapping_lease *lease,
    mem_service_mapping_access_fn callback, void *context)
{
    if (!lease || !callback) return -EINVAL;
    struct mem_service_mapping_owner *owner = lease->owner;
    int rc = domain_lock(owner->domain);
    if (rc) return rc;
    if (lease->operation_id) { domain_unlock(owner->domain); return -EBUSY; }
    /* Existing leases remain valid throughout closing/drain. Their presence
     * prevents unmap, including while a provider operation is in progress. */
    rc = callback(context, &owner->domain->channel, &owner->mapping,
                  &owner->lifecycle, &lease->view);
    domain_unlock(owner->domain);
    return rc;
}

int mem_service_mapping_lease_begin(
    struct mem_service_mapping_lease *lease, uint64_t operation_id)
{
    if (!lease || !operation_id || lease->kind != MEM_SERVICE_MAPPING_ACCESS_COMPUTE)
        return -EINVAL;
    struct mem_service_mapping_owner *owner = lease->owner;
    int rc = domain_lock(owner->domain);
    if (rc) return rc;
    if (owner->stats.closing) rc = -ECANCELED;
    else if (lease->operation_id) rc = -EBUSY;
    else {
        lease->operation_id = operation_id;
        ++owner->stats.in_flight_pins;
    }
    domain_unlock(owner->domain);
    return rc;
}

int mem_service_mapping_lease_complete(
    struct mem_service_mapping_lease *lease, uint64_t operation_id)
{
    if (!lease || !operation_id || lease->kind != MEM_SERVICE_MAPPING_ACCESS_COMPUTE)
        return -EINVAL;
    struct mem_service_mapping_owner *owner = lease->owner;
    int rc = domain_lock(owner->domain);
    if (rc) return rc;
    if (operation_id != lease->operation_id) rc = -ESTALE;
    else {
        lease->operation_id = 0;
        --owner->stats.in_flight_pins;
    }
    domain_unlock(owner->domain);
    return rc;
}

int mem_service_mapping_owner_inspect(
    struct mem_service_mapping_owner *owner,
    struct mem_service_mapping_owner_stats *stats_out)
{
    if (!owner || !stats_out) return -EINVAL;
    int rc = domain_lock(owner->domain);
    if (rc) return rc;
    *stats_out = owner->stats;
    domain_unlock(owner->domain);
    return 0;
}

static int owner_drain(
    struct mem_service_mapping_owner *owner,
    enum mem_service_wire_status *status_out, bool release_holder)
{
    if (status_out) *status_out = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    if (!owner) return -EINVAL;
    struct mem_service_access_domain *domain = owner->domain;
    int rc = domain_lock(domain);
    if (rc) return rc;
    owner->stats.closing = true;
    if (owner->stats.cpu_pins || owner->stats.compute_pins || owner->stats.in_flight_pins) {
        rc = -EBUSY;
        goto out;
    }
    if (owner->stats.mapping_pending) {
        rc = mem_service_client_unmap_managed_allocation(&domain->client,
            &domain->channel, &owner->mapping, &owner->lifecycle, status_out);
        if (rc || owner->lifecycle.pending || owner->mapping.binding.mapped) {
            if (!rc && status_out) *status_out = MEM_SERVICE_WIRE_STATUS_INTERNAL;
            rc = MEM_SERVICE_MAPPING_CLEANUP_REQUIRED;
            goto out;
        }
        owner->stats.mapping_pending = false;
    }
    if (release_holder && owner->stats.holder_pending) {
        struct mem_service_client_allocation released;
        rc = mem_service_client_release_object(&domain->client, owner->mapping.key,
            owner->release_operation, owner->lifecycle.session_id, true,
            owner->mapping.generation, &released, status_out);
        if (rc || released.generation != owner->mapping.generation ||
            strcmp(released.key, owner->mapping.key)) {
            if (!rc && status_out) *status_out = MEM_SERVICE_WIRE_STATUS_INTERNAL;
            rc = MEM_SERVICE_MAPPING_CLEANUP_REQUIRED;
            goto out;
        }
        owner->stats.holder_pending = false;
    }
    if (status_out) *status_out = MEM_SERVICE_WIRE_STATUS_OK;
out:
    domain_unlock(domain);
    return rc;
}

int mem_service_mapping_owner_unmap(
    struct mem_service_mapping_owner *owner,
    enum mem_service_wire_status *status_out)
{
    return owner_drain(owner, status_out, false);
}

int mem_service_mapping_owner_close(
    struct mem_service_mapping_owner *owner,
    enum mem_service_wire_status *status_out)
{
    return owner_drain(owner, status_out, true);
}

int mem_service_mapping_owner_destroy(struct mem_service_mapping_owner *owner)
{
    if (!owner) return -EINVAL;
    struct mem_service_access_domain *domain = owner->domain;
    int rc = domain_lock(domain);
    if (rc) return rc;
    if (!owner->stats.closing || owner->stats.mapping_pending ||
        owner->stats.holder_pending || owner->stats.cpu_pins ||
        owner->stats.compute_pins || owner->stats.in_flight_pins) {
        domain_unlock(domain);
        return -EBUSY;
    }
    struct mem_service_mapping_owner **cursor = &domain->owners;
    while (*cursor && *cursor != owner) cursor = &(*cursor)->next;
    if (!*cursor) { domain_unlock(domain); return -ESTALE; }
    *cursor = owner->next;
    domain_unlock(domain);
    free(owner);
    return 0;
}
