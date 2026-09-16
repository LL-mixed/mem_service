#include "mem_service_allocation.h"

#include <stdio.h>
#include <string.h>

static bool mem_service_managed_string_valid(const char *value, size_t max_len)
{
    size_t len;

    if (value == NULL) {
        return false;
    }
    len = strlen(value);
    return len > 0 && len < max_len;
}

static bool mem_service_managed_alignment_valid(uint64_t alignment)
{
    if (alignment == 0) {
        return true;
    }
    return (alignment & (alignment - 1U)) == 0;
}

static bool mem_service_managed_capabilities_valid(uint64_t capabilities)
{
    return capabilities != 0 &&
           (capabilities & ~MEM_SERVICE_MANAGED_CAP_VALID_MASK) == 0;
}

static struct mem_service_managed_allocation *mem_service_managed_find(
    struct mem_service_managed_table *table,
    const char *key)
{
    size_t i;

    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        struct mem_service_managed_allocation *entry = &table->entries[i];

        if (entry->in_use && strcmp(entry->key, key) == 0) {
            return entry;
        }
    }
    return NULL;
}

static const struct mem_service_managed_allocation *mem_service_managed_find_const(
    const struct mem_service_managed_table *table,
    const char *key)
{
    size_t i;

    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        const struct mem_service_managed_allocation *entry = &table->entries[i];

        if (entry->in_use && strcmp(entry->key, key) == 0) {
            return entry;
        }
    }
    return NULL;
}

/*
 * Slot selection for a new allocation of `key`: an exact-key RETIRED slot
 * keeps key identity continuity (the fresh allocation still receives a new
 * generation); otherwise any unused slot; otherwise the first RETIRED slot
 * is reclaimed (its resources were released, the tombstone is dropped).
 * QUARANTINED slots are never reclaimed.
 */
static struct mem_service_managed_allocation *mem_service_managed_select_slot(
    struct mem_service_managed_table *table,
    const char *key)
{
    struct mem_service_managed_allocation *first_retired = NULL;
    size_t i;

    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        struct mem_service_managed_allocation *entry = &table->entries[i];

        if (!entry->in_use) {
            return entry;
        }
        if (entry->state == MEM_SERVICE_MANAGED_STATE_RETIRED) {
            if (strcmp(entry->key, key) == 0) {
                return entry;
            }
            if (first_retired == NULL) {
                first_retired = entry;
            }
        }
    }
    return first_retired;
}

size_t mem_service_managed_provider_lost(
    struct mem_service_managed_table *table,
    const char *node_id,
    uint64_t incarnation)
{
    size_t changed = 0;

    if (table == NULL || incarnation == 0 ||
        !mem_service_managed_string_valid(node_id, MEM_SERVICE_MANAGED_NODE_ID_LEN)) {
        return 0;
    }
    for (size_t i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        struct mem_service_managed_allocation *entry = &table->entries[i];
        bool affected = false;

        if (!entry->in_use || entry->home_node_id[0] == '\0' ||
            entry->state == MEM_SERVICE_MANAGED_STATE_RETIRED ||
            entry->state == MEM_SERVICE_MANAGED_STATE_QUARANTINED) {
            continue;
        }
        affected = strcmp(entry->home_node_id, node_id) == 0 &&
                   entry->provider_incarnation == incarnation;
        for (uint32_t holder = 0; !affected && holder < entry->holder_count;
             ++holder) {
            const struct mem_service_managed_holder *binding =
                &entry->holders[holder];
            /* Legacy holders remain conservative because their node scope
             * cannot be proven. */
            affected = binding->node_id[0] == '\0' ||
                (strcmp(binding->node_id, node_id) == 0 &&
                 binding->provider_incarnation == incarnation);
        }
        if (!affected) continue;
        entry->state = MEM_SERVICE_MANAGED_STATE_QUARANTINED;
        table->quarantine_events += 1U;
        changed += 1U;
    }
    return changed;
}

static void mem_service_managed_fill_view(
    const struct mem_service_managed_allocation *entry,
    struct mem_service_managed_view *view_out)
{
    if (view_out == NULL) {
        return;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->state = entry->state;
    snprintf(view_out->key, sizeof(view_out->key), "%s", entry->key);
    snprintf(view_out->owner_session,
             sizeof(view_out->owner_session),
             "%s",
             entry->owner_session);
    view_out->generation = entry->generation;
    view_out->version = entry->version;
    view_out->size_bytes = entry->size_bytes;
    view_out->alignment_bytes = entry->alignment_bytes;
    view_out->capabilities = entry->capabilities;
    view_out->holder_count = entry->holder_count;
    memcpy(view_out->holders, entry->holders, sizeof(view_out->holders));
    view_out->provider_incarnation = entry->provider_incarnation;
    snprintf(view_out->home_node_id,
             sizeof(view_out->home_node_id),
             "%s",
             entry->home_node_id);
    view_out->provider_backed = entry->provider_backed;
    view_out->address = entry->address;
    view_out->address_len = entry->address_len;
    view_out->descriptor_len = entry->descriptor_len;
    memcpy(view_out->descriptor, entry->descriptor, sizeof(view_out->descriptor));
}

static int mem_service_managed_find_holder(
    const struct mem_service_managed_allocation *entry,
    const char *session_id)
{
    uint32_t i;

    for (i = 0; i < entry->holder_count; ++i) {
        if (strcmp(entry->holders[i].session_id, session_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Drive the backing release for a draining object whose last reference is
 * gone. A confirmed release retires the identity; an unconfirmed release
 * quarantines the object so its resources are isolated and counted instead
 * of being silently reused. Provider-backed objects have no in-process
 * release path: they wait in RETIRING for the bound home provider's
 * reclaim confirmation (mem_service_managed_reclaim).
 */
static enum mem_service_managed_state mem_service_managed_complete_retire(
    struct mem_service_managed_table *table,
    struct mem_service_managed_allocation *entry)
{
    if (entry->provider_backed || entry->home_node_id[0] != '\0') {
        return entry->state;
    }
    if (!table->backing_registered || table->backing_ops == NULL ||
        table->backing_ops->release == NULL ||
        table->backing_ops->release(table->backing_context,
                                    entry->descriptor,
                                    entry->descriptor_len) != 0) {
        table->quarantine_events += 1U;
        entry->state = MEM_SERVICE_MANAGED_STATE_QUARANTINED;
        return entry->state;
    }
    entry->descriptor_len = 0;
    memset(entry->descriptor, 0, sizeof(entry->descriptor));
    entry->state = MEM_SERVICE_MANAGED_STATE_RETIRED;
    return entry->state;
}

const char *mem_service_managed_state_name(enum mem_service_managed_state state)
{
    switch (state) {
    case MEM_SERVICE_MANAGED_STATE_ALLOCATING:
        return "allocating";
    case MEM_SERVICE_MANAGED_STATE_ACTIVE:
        return "active";
    case MEM_SERVICE_MANAGED_STATE_RETIRING:
        return "retiring";
    case MEM_SERVICE_MANAGED_STATE_RETIRED:
        return "retired";
    case MEM_SERVICE_MANAGED_STATE_QUARANTINED:
        return "quarantined";
    default:
        return "unknown";
    }
}

const char *mem_service_managed_result_name(enum mem_service_managed_result result)
{
    switch (result) {
    case MEM_SERVICE_MANAGED_RESULT_OK:
        return "ok";
    case MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST:
        return "invalid_request";
    case MEM_SERVICE_MANAGED_RESULT_NOT_FOUND:
        return "not_found";
    case MEM_SERVICE_MANAGED_RESULT_KEY_CONFLICT:
        return "key_conflict";
    case MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION:
        return "stale_generation";
    case MEM_SERVICE_MANAGED_RESULT_VERSION_CONFLICT:
        return "version_conflict";
    case MEM_SERVICE_MANAGED_RESULT_CAPACITY:
        return "capacity_exceeded";
    case MEM_SERVICE_MANAGED_RESULT_BACKING_UNAVAILABLE:
        return "backing_unavailable";
    case MEM_SERVICE_MANAGED_RESULT_BACKING_ERROR:
        return "backing_error";
    case MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER:
        return "not_holder";
    case MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT:
        return "state_conflict";
    case MEM_SERVICE_MANAGED_RESULT_PROVIDER_MISMATCH:
        return "provider_mismatch";
    case MEM_SERVICE_MANAGED_RESULT_PROVIDER_UNAVAILABLE:
        return "provider_unavailable";
    default:
        return "unknown";
    }
}

void mem_service_managed_table_init(struct mem_service_managed_table *table)
{
    if (table == NULL) {
        return;
    }
    memset(table, 0, sizeof(*table));
    table->next_generation = 1U;
    table->next_mapping_id = 1U;
}

int mem_service_managed_table_register_backing(
    struct mem_service_managed_table *table,
    const struct mem_service_managed_backing_ops *ops,
    void *context)
{
    if (table == NULL || ops == NULL || ops->reserve == NULL ||
        ops->release == NULL || table->backing_registered) {
        return -1;
    }
    table->backing_ops = ops;
    table->backing_context = context;
    table->backing_registered = true;
    return 0;
}

void mem_service_managed_table_unregister_backing(
    struct mem_service_managed_table *table)
{
    if (table == NULL) {
        return;
    }
    table->backing_ops = NULL;
    table->backing_context = NULL;
    table->backing_registered = false;
}

enum mem_service_managed_result mem_service_managed_allocate(
    struct mem_service_managed_table *table,
    const struct mem_service_managed_request *request,
    struct mem_service_managed_view *view_out)
{
    struct mem_service_managed_allocation *entry;
    uint8_t descriptor[MEM_SERVICE_MANAGED_DESCRIPTOR_MAX_LEN];
    uint32_t descriptor_len = 0;

    if (table == NULL || request == NULL ||
        !mem_service_managed_string_valid(request->key,
                                          MEM_SERVICE_MANAGED_KEY_LEN) ||
        !mem_service_managed_string_valid(
            request->idempotency_key,
            MEM_SERVICE_MANAGED_IDEMPOTENCY_KEY_LEN) ||
        (request->session_id != NULL &&
         !mem_service_managed_string_valid(
             request->session_id,
             MEM_SERVICE_MANAGED_SESSION_ID_LEN)) ||
        (request->home_node_id != NULL &&
         (!mem_service_managed_string_valid(
              request->home_node_id,
              MEM_SERVICE_MANAGED_NODE_ID_LEN) ||
          request->home_incarnation == 0)) ||
        request->size_bytes == 0 ||
        !mem_service_managed_alignment_valid(request->alignment_bytes) ||
        !mem_service_managed_capabilities_valid(request->capabilities)) {
        if (table != NULL) {
            table->allocate_rejected_count += 1U;
        }
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }

    entry = mem_service_managed_find(table, request->key);
    if (entry != NULL && entry->state != MEM_SERVICE_MANAGED_STATE_RETIRED) {
        /*
         * Object-level idempotent replay: the same allocate identity
         * returns the existing object instead of creating a second one.
         */
        if (strcmp(entry->allocate_idempotency_key,
                   request->idempotency_key) == 0) {
            table->allocate_ok_count += 1U;
            mem_service_managed_fill_view(entry, view_out);
            return MEM_SERVICE_MANAGED_RESULT_OK;
        }
        table->allocate_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_KEY_CONFLICT;
    }

    if (request->home_node_id == NULL && !table->backing_registered) {
        table->allocate_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_BACKING_UNAVAILABLE;
    }

    if (entry == NULL) {
        entry = mem_service_managed_select_slot(table, request->key);
        if (entry == NULL) {
            table->allocate_rejected_count += 1U;
            return MEM_SERVICE_MANAGED_RESULT_CAPACITY;
        }
    }

    /* Record the allocation intent before reserving resources. */
    memset(entry, 0, sizeof(*entry));
    entry->in_use = true;
    entry->state = MEM_SERVICE_MANAGED_STATE_ALLOCATING;
    snprintf(entry->key, sizeof(entry->key), "%s", request->key);
    snprintf(entry->allocate_idempotency_key,
             sizeof(entry->allocate_idempotency_key),
             "%s",
             request->idempotency_key);
    if (request->session_id != NULL) {
        snprintf(entry->owner_session,
                 sizeof(entry->owner_session),
                 "%s",
                 request->session_id);
    }
    entry->size_bytes = request->size_bytes;
    entry->alignment_bytes = request->alignment_bytes;
    entry->capabilities = request->capabilities;

    if (request->home_node_id != NULL) {
        /*
         * Provider-backed path: bind the intent to the home provider
         * boot and wait in ALLOCATING for its publish; the provider
         * reserves backing and address out of band.
         */
        snprintf(entry->home_node_id,
                 sizeof(entry->home_node_id),
                 "%s",
                 request->home_node_id);
        entry->provider_incarnation = request->home_incarnation;
        entry->generation = table->next_generation;
        table->next_generation += 1U;
        entry->version = 1U;
        table->allocate_ok_count += 1U;
        mem_service_managed_fill_view(entry, view_out);
        return MEM_SERVICE_MANAGED_RESULT_OK;
    }

    memset(descriptor, 0, sizeof(descriptor));
    if (table->backing_ops->reserve(table->backing_context,
                                    entry->size_bytes,
                                    entry->alignment_bytes,
                                    entry->capabilities,
                                    descriptor,
                                    sizeof(descriptor),
                                    &descriptor_len) != 0 ||
        descriptor_len > sizeof(descriptor)) {
        /* Roll back the intent slot; no resource may remain reserved. */
        memset(entry, 0, sizeof(*entry));
        table->allocate_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_BACKING_ERROR;
    }

    memcpy(entry->descriptor, descriptor, descriptor_len);
    entry->descriptor_len = descriptor_len;
    entry->generation = table->next_generation;
    table->next_generation += 1U;
    entry->version = 1U;
    entry->state = MEM_SERVICE_MANAGED_STATE_ACTIVE;
    table->allocate_ok_count += 1U;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

static enum mem_service_managed_result mem_service_managed_acquire_internal(
    struct mem_service_managed_table *table,
    const char *key,
    const char *session_id,
    const char *node_id,
    uint64_t provider_incarnation,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_managed_view *view_out,
    bool reference_checked)
{
    struct mem_service_managed_allocation *entry;
    struct mem_service_managed_holder *holder;
    int holder_index;
    bool node_bound = node_id != NULL && node_id[0] != '\0';

    if (table == NULL ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN) ||
        !mem_service_managed_string_valid(session_id,
                                          MEM_SERVICE_MANAGED_SESSION_ID_LEN) ||
        node_bound != (provider_incarnation != 0) ||
        (node_bound && !mem_service_managed_string_valid(
            node_id, MEM_SERVICE_MANAGED_NODE_ID_LEN))) {
        if (table != NULL) {
            table->acquire_rejected_count += 1U;
        }
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) {
        table->acquire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    if (has_expected_generation && expected_generation != entry->generation) {
        table->acquire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    }
    if (entry->state != MEM_SERVICE_MANAGED_STATE_ACTIVE) {
        table->acquire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
    if (entry->reference_mode && !reference_checked) {
        table->acquire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
    holder_index = mem_service_managed_find_holder(entry, session_id);
    if (holder_index >= 0) {
        holder = &entry->holders[holder_index];
        if (node_bound && holder->node_id[0] != '\0' &&
            (strcmp(holder->node_id, node_id) != 0 ||
             holder->provider_incarnation != provider_incarnation)) {
            table->acquire_rejected_count += 1U;
            return MEM_SERVICE_MANAGED_RESULT_PROVIDER_MISMATCH;
        }
        if (node_bound && holder->node_id[0] == '\0') {
            snprintf(holder->node_id, sizeof(holder->node_id), "%s", node_id);
            holder->provider_incarnation = provider_incarnation;
        }
        /* Re-acquire by an existing holder is naturally idempotent. */
        table->acquire_ok_count += 1U;
        mem_service_managed_fill_view(entry, view_out);
        return MEM_SERVICE_MANAGED_RESULT_OK;
    }
    if (entry->holder_count >= MEM_SERVICE_MANAGED_MAX_HOLDERS) {
        table->acquire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_CAPACITY;
    }
    holder = &entry->holders[entry->holder_count];
    memset(holder, 0, sizeof(*holder));
    snprintf(holder->session_id, sizeof(holder->session_id), "%s", session_id);
    holder->generation = entry->generation;
    if (node_bound) {
        snprintf(holder->node_id, sizeof(holder->node_id), "%s", node_id);
        holder->provider_incarnation = provider_incarnation;
    }
    entry->holder_count += 1U;
    table->acquire_ok_count += 1U;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_acquire(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, bool has_expected_generation,
    uint64_t expected_generation, struct mem_service_managed_view *view_out)
{
    return mem_service_managed_acquire_internal(table, key, session_id, NULL, 0,
        has_expected_generation, expected_generation, view_out, false);
}

enum mem_service_managed_result mem_service_managed_acquire_at_node(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, const char *node_id, uint64_t provider_incarnation,
    bool has_expected_generation, uint64_t expected_generation,
    struct mem_service_managed_view *view_out)
{
    return mem_service_managed_acquire_internal(table, key, session_id,
        node_id, provider_incarnation, has_expected_generation,
        expected_generation, view_out, false);
}

static bool mem_service_managed_has_mapping(
    const struct mem_service_managed_table *table,
    const struct mem_service_managed_allocation *entry)
{
    for (size_t i = 0; i < MEM_SERVICE_MANAGED_MAX_MAPPINGS; ++i)
        if (table->mappings[i].state != MEM_SERVICE_MANAGED_MAPPING_NONE &&
            table->mappings[i].generation == entry->generation &&
            !strcmp(table->mappings[i].key, entry->key)) return true;
    return false;
}

static enum mem_service_managed_result mem_service_managed_content_identity(
    const struct mem_service_managed_table *table, const char *key,
    uint64_t generation, uint64_t version,
    const struct mem_service_managed_allocation **entry_out)
{
    const struct mem_service_managed_allocation *entry;
    if (!table || !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN) ||
        !generation || !version) return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    entry = mem_service_managed_find_const(table, key);
    if (!entry) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    if (entry->generation != generation) return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    if (entry->version != version) return MEM_SERVICE_MANAGED_RESULT_VERSION_CONFLICT;
    if (entry->state != MEM_SERVICE_MANAGED_STATE_ACTIVE || !entry->provider_backed)
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    *entry_out = entry;
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

static bool mem_service_managed_content_owner(
    const struct mem_service_managed_allocation *entry, const char *session_id)
{
    return mem_service_managed_string_valid(session_id, MEM_SERVICE_MANAGED_SESSION_ID_LEN) &&
        !strcmp(entry->owner_session, session_id) && entry->holder_count == 1 &&
        mem_service_managed_find_holder(entry, session_id) >= 0;
}

enum mem_service_managed_result mem_service_managed_content_begin(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t expected_version,
    struct mem_service_managed_view *view_out)
{
    const struct mem_service_managed_allocation *current;
    struct mem_service_managed_allocation *entry;
    enum mem_service_managed_result result = mem_service_managed_content_identity(
        table, key, generation, expected_version, &current);
    if (result) return result;
    if (!mem_service_managed_content_owner(current, session_id))
        return MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER;
    if (current->content_writing || mem_service_managed_has_mapping(table, current))
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    if (current->version == UINT64_MAX) return MEM_SERVICE_MANAGED_RESULT_CAPACITY;
    entry = mem_service_managed_find(table, key);
    entry->reference_mode = true;
    entry->content_writing = true;
    ++entry->version;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_content_check(
    const struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t version,
    bool writing, struct mem_service_managed_view *view_out)
{
    const struct mem_service_managed_allocation *entry;
    enum mem_service_managed_result result = mem_service_managed_content_identity(
        table, key, generation, version, &entry);
    if (result) return result;
    if (!entry->reference_mode || entry->content_writing != writing)
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    if (writing && !mem_service_managed_content_owner(entry, session_id))
        return MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_content_seal(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t version)
{
    struct mem_service_managed_allocation *entry;
    enum mem_service_managed_result result = mem_service_managed_content_check(
        table, key, session_id, generation, version, true, NULL);
    if (result) return result;
    entry = mem_service_managed_find(table, key);
    if (mem_service_managed_has_mapping(table, entry))
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    entry->content_writing = false;
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_acquire_published(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, uint64_t generation, uint64_t version,
    struct mem_service_managed_view *view_out)
{
    enum mem_service_managed_result result = mem_service_managed_content_check(
        table, key, NULL, generation, version, false, NULL);
    if (result) {
        if (table) ++table->acquire_rejected_count;
        return result;
    }
    return mem_service_managed_acquire_internal(table, key, session_id, NULL, 0,
                                                true, generation, view_out, true);
}

enum mem_service_managed_result mem_service_managed_acquire_published_at_node(
    struct mem_service_managed_table *table, const char *key,
    const char *session_id, const char *node_id, uint64_t provider_incarnation,
    uint64_t generation, uint64_t version,
    struct mem_service_managed_view *view_out)
{
    enum mem_service_managed_result result = mem_service_managed_content_check(
        table, key, NULL, generation, version, false, NULL);
    if (result) {
        if (table) ++table->acquire_rejected_count;
        return result;
    }
    return mem_service_managed_acquire_internal(table, key, session_id,
        node_id, provider_incarnation, true, generation, view_out, true);
}

enum mem_service_managed_result mem_service_managed_release(
    struct mem_service_managed_table *table,
    const char *key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_managed_view *view_out)
{
    struct mem_service_managed_allocation *entry;
    int holder_index;
    uint32_t tail;

    if (table == NULL ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN) ||
        !mem_service_managed_string_valid(session_id,
                                          MEM_SERVICE_MANAGED_SESSION_ID_LEN)) {
        if (table != NULL) {
            table->release_rejected_count += 1U;
        }
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) {
        table->release_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    if (has_expected_generation && expected_generation != entry->generation) {
        table->release_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    }
    holder_index = mem_service_managed_find_holder(entry, session_id);
    if (holder_index < 0) {
        table->release_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER;
    }
    for (size_t i = 0; i < MEM_SERVICE_MANAGED_MAX_MAPPINGS; ++i) {
        const struct mem_service_managed_mapping *mapping = &table->mappings[i];
        if (mapping->state != MEM_SERVICE_MANAGED_MAPPING_NONE &&
            mapping->generation == entry->generation &&
            strcmp(mapping->key, key) == 0 &&
            strcmp(mapping->session_id, session_id) == 0) {
            table->release_rejected_count += 1U;
            return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
        }
    }
    tail = entry->holder_count - (uint32_t)holder_index - 1U;
    if (tail > 0) {
        memmove(&entry->holders[holder_index],
                &entry->holders[holder_index + 1],
                tail * sizeof(entry->holders[0]));
    }
    entry->holder_count -= 1U;
    memset(&entry->holders[entry->holder_count],
           0,
           sizeof(entry->holders[0]));
    if (entry->state == MEM_SERVICE_MANAGED_STATE_RETIRING &&
        entry->holder_count == 0) {
        (void)mem_service_managed_complete_retire(table, entry);
    }
    table->release_ok_count += 1U;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_retire(
    struct mem_service_managed_table *table,
    const char *key,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_managed_view *view_out)
{
    struct mem_service_managed_allocation *entry;

    if (table == NULL ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN)) {
        if (table != NULL) {
            table->retire_rejected_count += 1U;
        }
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) {
        table->retire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    if (has_expected_generation && expected_generation != entry->generation) {
        table->retire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    }
    switch (entry->state) {
    case MEM_SERVICE_MANAGED_STATE_ACTIVE:
        entry->state = MEM_SERVICE_MANAGED_STATE_RETIRING;
        /* fall through - a reference-free object retires immediately */
    case MEM_SERVICE_MANAGED_STATE_RETIRING:
        if (entry->holder_count == 0) {
            (void)mem_service_managed_complete_retire(table, entry);
        }
        table->retire_ok_count += 1U;
        mem_service_managed_fill_view(entry, view_out);
        return MEM_SERVICE_MANAGED_RESULT_OK;
    case MEM_SERVICE_MANAGED_STATE_ALLOCATING:
        /*
         * A remote reservation may already exist before publish arrives.
         * Keep the identity pending until its home confirms cancellation.
         */
        entry->state = MEM_SERVICE_MANAGED_STATE_RETIRING;
        table->retire_ok_count += 1U;
        mem_service_managed_fill_view(entry, view_out);
        return MEM_SERVICE_MANAGED_RESULT_OK;
    case MEM_SERVICE_MANAGED_STATE_RETIRED:
    case MEM_SERVICE_MANAGED_STATE_QUARANTINED:
    default:
        table->retire_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
}

enum mem_service_managed_result mem_service_managed_publish(
    struct mem_service_managed_table *table,
    const char *key,
    const char *node_id,
    uint64_t incarnation,
    uint64_t generation,
    const uint8_t *descriptor,
    uint32_t descriptor_len,
    uint64_t address,
    uint64_t address_len,
    struct mem_service_managed_view *view_out)
{
    struct mem_service_managed_allocation *entry;

    if (table == NULL ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN) ||
        !mem_service_managed_string_valid(node_id,
                                          MEM_SERVICE_MANAGED_NODE_ID_LEN) ||
        incarnation == 0 || descriptor == NULL || descriptor_len == 0 ||
        descriptor_len > MEM_SERVICE_MANAGED_DESCRIPTOR_MAX_LEN ||
        address_len == 0) {
        if (table != NULL) {
            table->publish_rejected_count += 1U;
        }
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) {
        table->publish_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    if (generation != entry->generation) {
        table->publish_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    }
    if (entry->home_node_id[0] == '\0' ||
        strcmp(entry->home_node_id, node_id) != 0 ||
        entry->provider_incarnation != incarnation) {
        table->publish_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_PROVIDER_MISMATCH;
    }
    if (entry->state == MEM_SERVICE_MANAGED_STATE_ACTIVE ||
        (entry->state == MEM_SERVICE_MANAGED_STATE_RETIRING && entry->provider_backed)) {
        /* Idempotent replay of an identical publish. */
        if (entry->provider_backed &&
            entry->descriptor_len == descriptor_len &&
            memcmp(entry->descriptor, descriptor, descriptor_len) == 0 &&
            entry->address == address &&
            entry->address_len == address_len) {
            table->publish_ok_count += 1U;
            mem_service_managed_fill_view(entry, view_out);
            return MEM_SERVICE_MANAGED_RESULT_OK;
        }
        table->publish_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
    if (entry->state != MEM_SERVICE_MANAGED_STATE_ALLOCATING &&
        entry->state != MEM_SERVICE_MANAGED_STATE_RETIRING) {
        table->publish_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
    memcpy(entry->descriptor, descriptor, descriptor_len);
    entry->descriptor_len = descriptor_len;
    entry->address = address;
    entry->address_len = address_len;
    entry->provider_backed = true;
    if (entry->state == MEM_SERVICE_MANAGED_STATE_ALLOCATING)
        entry->state = MEM_SERVICE_MANAGED_STATE_ACTIVE;
    table->publish_ok_count += 1U;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_reclaim(
    struct mem_service_managed_table *table,
    const char *key,
    const char *node_id,
    uint64_t incarnation,
    uint64_t generation,
    bool confirmed,
    struct mem_service_managed_view *view_out)
{
    struct mem_service_managed_allocation *entry;

    if (table == NULL ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN) ||
        !mem_service_managed_string_valid(node_id,
                                          MEM_SERVICE_MANAGED_NODE_ID_LEN) ||
        incarnation == 0) {
        if (table != NULL) {
            table->reclaim_rejected_count += 1U;
        }
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    if (generation != entry->generation) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    }
    if (entry->home_node_id[0] == '\0' ||
        strcmp(entry->home_node_id, node_id) != 0 ||
        entry->provider_incarnation != incarnation) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_PROVIDER_MISMATCH;
    }
    if (entry->state == MEM_SERVICE_MANAGED_STATE_RETIRED ||
        entry->state == MEM_SERVICE_MANAGED_STATE_QUARANTINED) {
        /* Idempotent replay against the terminal state. */
        table->reclaim_ok_count += 1U;
        mem_service_managed_fill_view(entry, view_out);
        return MEM_SERVICE_MANAGED_RESULT_OK;
    }
    if (entry->state != MEM_SERVICE_MANAGED_STATE_RETIRING ||
        entry->holder_count != 0) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
    if (!confirmed) {
        table->quarantine_events += 1U;
        entry->state = MEM_SERVICE_MANAGED_STATE_QUARANTINED;
        table->reclaim_ok_count += 1U;
        mem_service_managed_fill_view(entry, view_out);
        return MEM_SERVICE_MANAGED_RESULT_OK;
    }
    entry->descriptor_len = 0;
    memset(entry->descriptor, 0, sizeof(entry->descriptor));
    entry->address = 0;
    entry->address_len = 0;
    entry->provider_backed = false;
    entry->state = MEM_SERVICE_MANAGED_STATE_RETIRED;
    table->reclaim_ok_count += 1U;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_recover(
    struct mem_service_managed_table *table,
    const char *key,
    uint64_t generation,
    bool backing_gone,
    struct mem_service_managed_view *view_out)
{
    struct mem_service_managed_allocation *entry;
    size_t i;

    if (table == NULL || generation == 0 ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN)) {
        if (table != NULL) table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    if (entry->generation != generation) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    }
    if (entry->state == MEM_SERVICE_MANAGED_STATE_RETIRED ||
        (!backing_gone && entry->state == MEM_SERVICE_MANAGED_STATE_RETIRING &&
         entry->holder_count == 0)) {
        table->reclaim_ok_count += 1U;
        mem_service_managed_fill_view(entry, view_out);
        return MEM_SERVICE_MANAGED_RESULT_OK;
    }
    if (entry->state != MEM_SERVICE_MANAGED_STATE_QUARANTINED) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
    if (entry->holder_count != 0 || entry->content_writing) {
        table->reclaim_rejected_count += 1U;
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
    }
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_MAPPINGS; ++i) {
        const struct mem_service_managed_mapping *mapping = &table->mappings[i];

        if (mapping->state != MEM_SERVICE_MANAGED_MAPPING_NONE &&
            mapping->generation == entry->generation &&
            strcmp(mapping->key, entry->key) == 0) {
            table->reclaim_rejected_count += 1U;
            return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
        }
    }
    if (backing_gone) {
        entry->descriptor_len = 0;
        memset(entry->descriptor, 0, sizeof(entry->descriptor));
        entry->address = 0;
        entry->address_len = 0;
        entry->provider_backed = false;
        entry->state = MEM_SERVICE_MANAGED_STATE_RETIRED;
    } else {
        entry->state = MEM_SERVICE_MANAGED_STATE_RETIRING;
    }
    table->reclaim_ok_count += 1U;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_fence_holder(
    struct mem_service_managed_table *table,
    const char *key,
    uint64_t generation,
    const char *holder_node_id,
    uint64_t fenced_incarnation,
    uint32_t *fenced_holders_out,
    uint32_t *fenced_mappings_out,
    struct mem_service_managed_view *view_out)
{
    struct mem_service_managed_allocation *entry;
    bool owner_fenced = false;
    uint32_t fenced_holders = 0;
    uint32_t fenced_mappings = 0;
    uint32_t holder;
    uint32_t write = 0;
    size_t mapping_index;

    if (fenced_holders_out != NULL) *fenced_holders_out = 0;
    if (fenced_mappings_out != NULL) *fenced_mappings_out = 0;
    if (table == NULL || generation == 0 || fenced_incarnation == 0 ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN) ||
        !mem_service_managed_string_valid(holder_node_id,
                                          MEM_SERVICE_MANAGED_NODE_ID_LEN)) {
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    if (entry->generation != generation)
        return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    if (entry->state != MEM_SERVICE_MANAGED_STATE_QUARANTINED)
        return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;

    for (holder = 0; holder < entry->holder_count; ++holder) {
        const struct mem_service_managed_holder *binding =
            &entry->holders[holder];

        if (binding->provider_incarnation == fenced_incarnation &&
            strcmp(binding->node_id, holder_node_id) == 0) {
            if (binding->generation != generation)
                return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
            fenced_holders += 1U;
            if (strcmp(binding->session_id, entry->owner_session) == 0)
                owner_fenced = true;
        }
    }
    if (fenced_holders == 0) return MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER;

    for (mapping_index = 0;
         mapping_index < MEM_SERVICE_MANAGED_MAX_MAPPINGS;
         ++mapping_index) {
        struct mem_service_managed_mapping *mapping =
            &table->mappings[mapping_index];
        bool owned_by_fenced_holder = false;

        if (mapping->state == MEM_SERVICE_MANAGED_MAPPING_NONE ||
            mapping->generation != generation ||
            strcmp(mapping->key, key) != 0) {
            continue;
        }
        for (holder = 0; holder < entry->holder_count; ++holder) {
            const struct mem_service_managed_holder *binding =
                &entry->holders[holder];

            if (binding->provider_incarnation == fenced_incarnation &&
                strcmp(binding->node_id, holder_node_id) == 0 &&
                strcmp(binding->session_id, mapping->session_id) == 0) {
                owned_by_fenced_holder = true;
                break;
            }
        }
        if (owned_by_fenced_holder) {
            memset(mapping, 0, sizeof(*mapping));
            fenced_mappings += 1U;
        }
    }

    for (holder = 0; holder < entry->holder_count; ++holder) {
        const struct mem_service_managed_holder *binding =
            &entry->holders[holder];

        if (binding->provider_incarnation == fenced_incarnation &&
            strcmp(binding->node_id, holder_node_id) == 0) {
            continue;
        }
        if (write != holder) entry->holders[write] = *binding;
        write += 1U;
    }
    memset(&entry->holders[write], 0,
           (entry->holder_count - write) * sizeof(entry->holders[0]));
    entry->holder_count = write;
    if (owner_fenced) entry->content_writing = false;
    if (fenced_holders_out != NULL) *fenced_holders_out = fenced_holders;
    if (fenced_mappings_out != NULL) *fenced_mappings_out = fenced_mappings;
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

bool mem_service_managed_recovery_scope_known(
    const struct mem_service_managed_table *table)
{
    bool obligation = false;
    size_t i;

    if (table == NULL) return false;
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        const struct mem_service_managed_allocation *entry = &table->entries[i];
        uint32_t holder;

        if (!entry->in_use || entry->state == MEM_SERVICE_MANAGED_STATE_RETIRED)
            continue;
        if (entry->state == MEM_SERVICE_MANAGED_STATE_ACTIVE) continue;
        obligation = true;
        if (!entry->home_node_id[0] || !entry->provider_incarnation) return false;
        for (holder = 0; holder < entry->holder_count; ++holder)
            if (!entry->holders[holder].node_id[0] ||
                !entry->holders[holder].provider_incarnation)
                return false;
    }
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_MAPPINGS; ++i) {
        const struct mem_service_managed_mapping *mapping = &table->mappings[i];
        const struct mem_service_managed_allocation *entry;
        int holder;

        if (mapping->state == MEM_SERVICE_MANAGED_MAPPING_NONE) continue;
        entry = mem_service_managed_find_const(table, mapping->key);
        if (entry == NULL || entry->generation != mapping->generation) return false;
        holder = mem_service_managed_find_holder(entry, mapping->session_id);
        if (holder < 0 || !entry->holders[holder].node_id[0] ||
            !entry->holders[holder].provider_incarnation)
            return false;
    }
    return obligation;
}

bool mem_service_managed_recovery_pending(
    const struct mem_service_managed_table *table)
{
    size_t i;

    if (table == NULL) return true;
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        const struct mem_service_managed_allocation *entry = &table->entries[i];

        if (entry->in_use && entry->state != MEM_SERVICE_MANAGED_STATE_ACTIVE &&
            entry->state != MEM_SERVICE_MANAGED_STATE_RETIRED)
            return true;
    }
    return false;
}

enum mem_service_managed_result mem_service_managed_poll(
    const struct mem_service_managed_table *table,
    const char *node_id,
    uint64_t incarnation,
    uint64_t after_generation,
    struct mem_service_managed_view *view_out)
{
    const struct mem_service_managed_allocation *next = NULL;
    size_t i;

    if (view_out != NULL) {
        memset(view_out, 0, sizeof(*view_out));
    }
    if (table == NULL || view_out == NULL || incarnation == 0 ||
        !mem_service_managed_string_valid(node_id, MEM_SERVICE_MANAGED_NODE_ID_LEN)) {
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        const struct mem_service_managed_allocation *entry = &table->entries[i];

        if (!entry->in_use || entry->generation <= after_generation ||
            entry->provider_incarnation != incarnation ||
            strcmp(entry->home_node_id, node_id) != 0 ||
            (entry->state != MEM_SERVICE_MANAGED_STATE_ALLOCATING &&
             !(entry->state == MEM_SERVICE_MANAGED_STATE_RETIRING &&
               entry->holder_count == 0))) {
            continue;
        }
        if (next == NULL || entry->generation < next->generation) {
            next = entry;
        }
    }
    if (next == NULL) {
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    mem_service_managed_fill_view(next, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_poll_recovery(
    const struct mem_service_managed_table *table,
    const char *node_id,
    uint64_t fenced_incarnation,
    uint64_t after_generation,
    struct mem_service_managed_view *view_out)
{
    const struct mem_service_managed_allocation *next = NULL;
    size_t i;

    if (view_out != NULL) memset(view_out, 0, sizeof(*view_out));
    if (table == NULL || view_out == NULL || fenced_incarnation == 0 ||
        !mem_service_managed_string_valid(node_id,
                                          MEM_SERVICE_MANAGED_NODE_ID_LEN)) {
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        const struct mem_service_managed_allocation *entry = &table->entries[i];

        if (!entry->in_use || entry->generation <= after_generation ||
            entry->state != MEM_SERVICE_MANAGED_STATE_QUARANTINED ||
            entry->provider_incarnation != fenced_incarnation ||
            strcmp(entry->home_node_id, node_id) != 0) {
            continue;
        }
        if (next == NULL || entry->generation < next->generation) next = entry;
    }
    if (next == NULL) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    mem_service_managed_fill_view(next, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_poll_holder_recovery(
    const struct mem_service_managed_table *table,
    const char *holder_node_id,
    uint64_t fenced_incarnation,
    uint64_t after_generation,
    struct mem_service_managed_view *view_out)
{
    const struct mem_service_managed_allocation *next = NULL;
    size_t i;

    if (view_out != NULL) memset(view_out, 0, sizeof(*view_out));
    if (table == NULL || view_out == NULL || fenced_incarnation == 0 ||
        !mem_service_managed_string_valid(holder_node_id,
                                          MEM_SERVICE_MANAGED_NODE_ID_LEN)) {
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        const struct mem_service_managed_allocation *entry = &table->entries[i];
        uint32_t holder;
        bool found = false;

        if (!entry->in_use || entry->generation <= after_generation ||
            entry->state != MEM_SERVICE_MANAGED_STATE_QUARANTINED) {
            continue;
        }
        for (holder = 0; holder < entry->holder_count; ++holder) {
            const struct mem_service_managed_holder *binding =
                &entry->holders[holder];

            if (binding->provider_incarnation == fenced_incarnation &&
                strcmp(binding->node_id, holder_node_id) == 0) {
                found = true;
                break;
            }
        }
        if (found && (next == NULL || entry->generation < next->generation)) {
            next = entry;
        }
    }
    if (next == NULL) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    mem_service_managed_fill_view(next, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_inspect(
    const struct mem_service_managed_table *table,
    const char *key,
    struct mem_service_managed_view *view_out)
{
    const struct mem_service_managed_allocation *entry;

    if (table == NULL ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN)) {
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find_const(table, key);
    if (entry == NULL) {
        return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    }
    mem_service_managed_fill_view(entry, view_out);
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

void mem_service_managed_stats_snapshot(
    const struct mem_service_managed_table *table,
    struct mem_service_managed_stats *stats_out)
{
    size_t i;

    if (stats_out == NULL) {
        return;
    }
    memset(stats_out, 0, sizeof(*stats_out));
    if (table == NULL) {
        return;
    }
    stats_out->backing_registered = table->backing_registered ? 1U : 0U;
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_MAPPINGS; ++i) {
        enum mem_service_managed_mapping_state state = table->mappings[i].state;
        if (state == MEM_SERVICE_MANAGED_MAPPING_ACTIVE ||
            state == MEM_SERVICE_MANAGED_MAPPING_CLOSING) {
            stats_out->import_mappings += 1U;
        }
        if (state == MEM_SERVICE_MANAGED_MAPPING_PENDING ||
            state == MEM_SERVICE_MANAGED_MAPPING_CLOSING) {
            stats_out->in_flight += 1U;
        }
    }
    for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; ++i) {
        const struct mem_service_managed_allocation *entry = &table->entries[i];

        if (!entry->in_use) {
            continue;
        }
        switch (entry->state) {
        case MEM_SERVICE_MANAGED_STATE_ALLOCATING:
            stats_out->in_flight += 1U;
            stats_out->live_objects += 1U;
            break;
        case MEM_SERVICE_MANAGED_STATE_ACTIVE:
            stats_out->live_objects += 1U;
            stats_out->backing_allocated_bytes +=
                entry->provider_backed ? entry->address_len : entry->size_bytes;
            stats_out->address_reserved_bytes +=
                entry->provider_backed ? entry->address_len
                                       : entry->size_bytes;
            if (entry->provider_backed) {
                stats_out->export_mappings += 1U;
            }
            stats_out->live_refs += entry->holder_count;
            break;
        case MEM_SERVICE_MANAGED_STATE_RETIRING:
            stats_out->live_objects += 1U;
            stats_out->in_flight += 1U;
            stats_out->backing_allocated_bytes +=
                entry->provider_backed ? entry->address_len :
                entry->home_node_id[0] ? 0 : entry->size_bytes;
            stats_out->address_reserved_bytes +=
                entry->provider_backed ? entry->address_len
                : entry->home_node_id[0] ? 0 : entry->size_bytes;
            if (entry->provider_backed) {
                stats_out->export_mappings += 1U;
            }
            stats_out->live_refs += entry->holder_count;
            break;
        case MEM_SERVICE_MANAGED_STATE_QUARANTINED:
            stats_out->quarantined_objects += 1U;
            stats_out->quarantined_bytes +=
                entry->provider_backed ? entry->address_len :
                entry->home_node_id[0] ? 0 : entry->size_bytes;
            stats_out->live_refs += entry->holder_count;
            if (entry->provider_backed) {
                stats_out->export_mappings += 1U;
            } else if (entry->home_node_id[0]) {
                /* An unpublished reservation is still an unresolved intent. */
                stats_out->in_flight += 1U;
            }
            break;
        case MEM_SERVICE_MANAGED_STATE_RETIRED:
        default:
            break;
        }
    }
    stats_out->allocate_ok_count = table->allocate_ok_count;
    stats_out->acquire_ok_count = table->acquire_ok_count;
    stats_out->release_ok_count = table->release_ok_count;
    stats_out->retire_ok_count = table->retire_ok_count;
    stats_out->allocate_rejected_count = table->allocate_rejected_count;
    stats_out->acquire_rejected_count = table->acquire_rejected_count;
    stats_out->release_rejected_count = table->release_rejected_count;
    stats_out->retire_rejected_count = table->retire_rejected_count;
    stats_out->publish_ok_count = table->publish_ok_count;
    stats_out->publish_rejected_count = table->publish_rejected_count;
    stats_out->reclaim_ok_count = table->reclaim_ok_count;
    stats_out->reclaim_rejected_count = table->reclaim_rejected_count;
    stats_out->quarantine_events = table->quarantine_events;
}

static enum mem_service_managed_result mem_service_managed_mapping_transition_checked(
    struct mem_service_managed_table *table,
    const char *key,
    const char *session_id,
    uint64_t generation,
    uint64_t mapping_id,
    enum mem_service_managed_mapping_action action,
    bool published_checked,
    struct mem_service_managed_mapping *mapping_out)
{
    struct mem_service_managed_allocation *entry;
    struct mem_service_managed_mapping *mapping = NULL;
    size_t i;

    if (mapping_out != NULL) memset(mapping_out, 0, sizeof(*mapping_out));
    if (table == NULL || mapping_out == NULL || generation == 0 ||
        !mem_service_managed_string_valid(key, MEM_SERVICE_MANAGED_KEY_LEN) ||
        !mem_service_managed_string_valid(session_id, MEM_SERVICE_MANAGED_SESSION_ID_LEN) ||
        action < MEM_SERVICE_MANAGED_MAPPING_BEGIN ||
        action > MEM_SERVICE_MANAGED_MAPPING_INSPECT ||
        ((action == MEM_SERVICE_MANAGED_MAPPING_BEGIN) != (mapping_id == 0))) {
        return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_managed_find(table, key);
    if (entry == NULL) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
    if (entry->generation != generation) return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
    if (mem_service_managed_find_holder(entry, session_id) < 0)
        return MEM_SERVICE_MANAGED_RESULT_NOT_HOLDER;

    if (action == MEM_SERVICE_MANAGED_MAPPING_BEGIN) {
        if (!published_checked && entry->reference_mode && (!entry->content_writing ||
            entry->holder_count != 1 || strcmp(entry->owner_session, session_id)))
            return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
        if (entry->state != MEM_SERVICE_MANAGED_STATE_ACTIVE ||
            !entry->provider_backed || !(entry->capabilities & MEM_SERVICE_MANAGED_CAP_MAP))
            return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
        if (table->next_mapping_id == 0 || table->next_mapping_id == UINT64_MAX)
            return MEM_SERVICE_MANAGED_RESULT_CAPACITY;
        for (i = 0; i < MEM_SERVICE_MANAGED_MAX_MAPPINGS; ++i) {
            if (table->mappings[i].state == MEM_SERVICE_MANAGED_MAPPING_NONE) {
                mapping = &table->mappings[i];
                break;
            }
        }
        if (mapping == NULL) return MEM_SERVICE_MANAGED_RESULT_CAPACITY;
        memset(mapping, 0, sizeof(*mapping));
        mapping->id = table->next_mapping_id++;
        mapping->generation = generation;
        mapping->state = MEM_SERVICE_MANAGED_MAPPING_PENDING;
        snprintf(mapping->key, sizeof(mapping->key), "%s", key);
        snprintf(mapping->session_id, sizeof(mapping->session_id), "%s", session_id);
    } else {
        for (i = 0; i < MEM_SERVICE_MANAGED_MAX_MAPPINGS; ++i) {
            if (table->mappings[i].state != MEM_SERVICE_MANAGED_MAPPING_NONE &&
                table->mappings[i].id == mapping_id) {
                mapping = &table->mappings[i];
                break;
            }
        }
        if (mapping == NULL) return MEM_SERVICE_MANAGED_RESULT_NOT_FOUND;
        if (mapping->generation != generation || strcmp(mapping->key, key) ||
            strcmp(mapping->session_id, session_id))
            return MEM_SERVICE_MANAGED_RESULT_STALE_GENERATION;
        switch (action) {
        case MEM_SERVICE_MANAGED_MAPPING_CONFIRM:
            if (mapping->state != MEM_SERVICE_MANAGED_MAPPING_PENDING ||
                entry->state != MEM_SERVICE_MANAGED_STATE_ACTIVE)
                return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
            mapping->state = MEM_SERVICE_MANAGED_MAPPING_ACTIVE;
            break;
        case MEM_SERVICE_MANAGED_MAPPING_CLOSE:
            if (mapping->state != MEM_SERVICE_MANAGED_MAPPING_ACTIVE)
                return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
            mapping->state = MEM_SERVICE_MANAGED_MAPPING_CLOSING;
            break;
        case MEM_SERVICE_MANAGED_MAPPING_FINISH:
            if (mapping->state != MEM_SERVICE_MANAGED_MAPPING_CLOSING)
                return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
            break;
        case MEM_SERVICE_MANAGED_MAPPING_CANCEL:
            if (mapping->state != MEM_SERVICE_MANAGED_MAPPING_PENDING)
                return MEM_SERVICE_MANAGED_RESULT_STATE_CONFLICT;
            break;
        case MEM_SERVICE_MANAGED_MAPPING_INSPECT:
            break;
        default:
            return MEM_SERVICE_MANAGED_RESULT_INVALID_REQUEST;
        }
    }
    *mapping_out = *mapping;
    if (action == MEM_SERVICE_MANAGED_MAPPING_FINISH ||
        action == MEM_SERVICE_MANAGED_MAPPING_CANCEL) {
        mapping_out->state = MEM_SERVICE_MANAGED_MAPPING_NONE;
        memset(mapping, 0, sizeof(*mapping));
    }
    return MEM_SERVICE_MANAGED_RESULT_OK;
}

enum mem_service_managed_result mem_service_managed_mapping_transition(
    struct mem_service_managed_table *table, const char *key, const char *session_id,
    uint64_t generation, uint64_t mapping_id,
    enum mem_service_managed_mapping_action action,
    struct mem_service_managed_mapping *mapping_out)
{
    return mem_service_managed_mapping_transition_checked(table, key, session_id,
        generation, mapping_id, action, false, mapping_out);
}

enum mem_service_managed_result mem_service_managed_mapping_begin_published(
    struct mem_service_managed_table *table, const char *key, const char *session_id,
    uint64_t generation, uint64_t version,
    struct mem_service_managed_mapping *mapping_out)
{
    struct mem_service_managed_view view;
    enum mem_service_managed_result result = mem_service_managed_content_check(
        table, key, session_id, generation, version, false, &view);
    if (result) return result;
    return mem_service_managed_mapping_transition_checked(table, key, session_id,
        generation, 0, MEM_SERVICE_MANAGED_MAPPING_BEGIN, true, mapping_out);
}
