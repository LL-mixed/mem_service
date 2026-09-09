#include "mem_service_provider_directory.h"

#include <stdio.h>
#include <string.h>

static const char *const mem_service_provider_directory_result_names[] = {
    "ok",
    "invalid_request",
    "not_found",
    "incarnation_conflict",
    "capacity",
};

const char *mem_service_provider_directory_result_name(
    enum mem_service_provider_directory_result result)
{
    if ((unsigned)result >=
        sizeof(mem_service_provider_directory_result_names) /
            sizeof(mem_service_provider_directory_result_names[0])) {
        return "unknown";
    }
    return mem_service_provider_directory_result_names[result];
}

static uint64_t mem_service_provider_directory_lease_ms(
    const struct mem_service_provider_directory *directory)
{
    if (directory->config.lease_ms == 0) {
        return MEM_SERVICE_PROVIDER_DIRECTORY_DEFAULT_LEASE_MS;
    }
    return directory->config.lease_ms;
}

static bool mem_service_provider_node_id_valid(const char *node_id)
{
    return node_id != NULL && node_id[0] != '\0' &&
           strlen(node_id) < MEM_SERVICE_PROVIDER_NODE_ID_LEN;
}

static bool mem_service_provider_entry_fresh(
    const struct mem_service_provider_directory *directory,
    const struct mem_service_provider_directory_entry *entry,
    uint64_t now_ms)
{
    uint64_t lease_ms = mem_service_provider_directory_lease_ms(directory);

    if (!entry->in_use) {
        return false;
    }
    if (now_ms <= entry->last_refresh_ms) {
        return true;
    }
    return now_ms - entry->last_refresh_ms < lease_ms;
}

void mem_service_provider_directory_init(
    struct mem_service_provider_directory *directory)
{
    if (directory == NULL) {
        return;
    }
    memset(directory, 0, sizeof(*directory));
}

int mem_service_provider_directory_configure(
    struct mem_service_provider_directory *directory,
    const struct mem_service_provider_directory_config *config)
{
    size_t i;
    size_t j;

    if (directory == NULL || config == NULL) {
        return -1;
    }
    if (config->required_count > MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS) {
        return -1;
    }
    if (config->lease_ms != 0 &&
        (config->lease_ms < MEM_SERVICE_PROVIDER_DIRECTORY_MIN_LEASE_MS ||
         config->lease_ms > MEM_SERVICE_PROVIDER_DIRECTORY_MAX_LEASE_MS)) {
        return -1;
    }
    for (i = 0; i < config->required_count; ++i) {
        if (!mem_service_provider_node_id_valid(config->required_nodes[i])) {
            return -1;
        }
        for (j = i + 1; j < config->required_count; ++j) {
            if (strcmp(config->required_nodes[i], config->required_nodes[j]) ==
                0) {
                return -1;
            }
        }
    }
    directory->config = *config;
    directory->config_set = true;
    return 0;
}

static struct mem_service_provider_directory_entry *
mem_service_provider_directory_find_mut(
    struct mem_service_provider_directory *directory,
    const char *node_id)
{
    size_t i;

    for (i = 0; i < MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS; ++i) {
        struct mem_service_provider_directory_entry *entry =
            &directory->entries[i];

        if (entry->in_use && strcmp(entry->node_id, node_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

const struct mem_service_provider_directory_entry *
mem_service_provider_directory_find(
    const struct mem_service_provider_directory *directory,
    const char *node_id)
{
    size_t i;

    if (directory == NULL || !mem_service_provider_node_id_valid(node_id)) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS; ++i) {
        const struct mem_service_provider_directory_entry *entry =
            &directory->entries[i];

        if (entry->in_use && strcmp(entry->node_id, node_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

bool mem_service_provider_directory_lookup_active(
    const struct mem_service_provider_directory *directory,
    const char *node_id,
    uint64_t now_ms,
    uint64_t *incarnation_out)
{
    const struct mem_service_provider_directory_entry *entry =
        mem_service_provider_directory_find(directory, node_id);

    if (incarnation_out != NULL) {
        *incarnation_out = 0;
    }
    if (entry == NULL ||
        !mem_service_provider_entry_fresh(directory, entry, now_ms)) {
        return false;
    }
    if (incarnation_out != NULL) {
        *incarnation_out = entry->incarnation;
    }
    return true;
}

enum mem_service_provider_directory_result
mem_service_provider_directory_register(
    struct mem_service_provider_directory *directory,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    uint64_t capabilities,
    uint64_t now_ms,
    bool *replaced_out)
{
    struct mem_service_provider_directory_entry *entry;
    size_t i;

    if (replaced_out != NULL) {
        *replaced_out = false;
    }
    if (directory == NULL || !mem_service_provider_node_id_valid(node_id) ||
        incarnation == 0 || capabilities == 0 ||
        (capabilities & ~MEM_SERVICE_PROVIDER_CAP_VALID_MASK) != 0) {
        if (directory != NULL) {
            directory->stats.register_rejected_count += 1U;
        }
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_provider_directory_find_mut(directory, node_id);
    if (entry != NULL) {
        if (entry->incarnation == incarnation) {
            /* Idempotent replay or refresh-in-place of the same boot. */
            entry->readiness_generation = readiness_generation;
            entry->capabilities = capabilities;
            entry->last_refresh_ms = now_ms;
            directory->stats.register_ok_count += 1U;
            return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_OK;
        }
        /* A newer boot of the same node replaces the stale entry. */
        entry->incarnation = incarnation;
        entry->readiness_generation = readiness_generation;
        entry->capabilities = capabilities;
        entry->registered_ms = now_ms;
        entry->last_refresh_ms = now_ms;
        directory->directory_epoch += 1U;
        directory->stats.register_ok_count += 1U;
        directory->stats.register_replace_count += 1U;
        if (replaced_out != NULL) {
            *replaced_out = true;
        }
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_OK;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS; ++i) {
        if (!directory->entries[i].in_use) {
            entry = &directory->entries[i];
            memset(entry, 0, sizeof(*entry));
            entry->in_use = true;
            snprintf(entry->node_id,
                     sizeof(entry->node_id),
                     "%s",
                     node_id);
            entry->incarnation = incarnation;
            entry->readiness_generation = readiness_generation;
            entry->capabilities = capabilities;
            entry->registered_ms = now_ms;
            entry->last_refresh_ms = now_ms;
            directory->directory_epoch += 1U;
            directory->stats.register_ok_count += 1U;
            return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_OK;
        }
    }
    directory->stats.register_rejected_count += 1U;
    return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_CAPACITY;
}

enum mem_service_provider_directory_result
mem_service_provider_directory_refresh(
    struct mem_service_provider_directory *directory,
    const char *node_id,
    uint64_t incarnation,
    uint64_t readiness_generation,
    uint64_t now_ms)
{
    struct mem_service_provider_directory_entry *entry;

    if (directory == NULL || !mem_service_provider_node_id_valid(node_id) ||
        incarnation == 0) {
        if (directory != NULL) {
            directory->stats.refresh_rejected_count += 1U;
        }
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_provider_directory_find_mut(directory, node_id);
    if (entry == NULL) {
        directory->stats.refresh_rejected_count += 1U;
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_NOT_FOUND;
    }
    if (entry->incarnation != incarnation) {
        directory->stats.refresh_rejected_count += 1U;
        directory->stats.incarnation_conflict_count += 1U;
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_INCARNATION_CONFLICT;
    }
    entry->readiness_generation = readiness_generation;
    entry->last_refresh_ms = now_ms;
    directory->stats.refresh_ok_count += 1U;
    return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_OK;
}

enum mem_service_provider_directory_result
mem_service_provider_directory_deregister(
    struct mem_service_provider_directory *directory,
    const char *node_id,
    uint64_t incarnation,
    uint64_t now_ms)
{
    struct mem_service_provider_directory_entry *entry;

    (void)now_ms;
    if (directory == NULL || !mem_service_provider_node_id_valid(node_id) ||
        incarnation == 0) {
        if (directory != NULL) {
            directory->stats.deregister_rejected_count += 1U;
        }
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_INVALID_REQUEST;
    }
    entry = mem_service_provider_directory_find_mut(directory, node_id);
    if (entry == NULL) {
        directory->stats.deregister_rejected_count += 1U;
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_NOT_FOUND;
    }
    if (entry->incarnation != incarnation) {
        directory->stats.deregister_rejected_count += 1U;
        directory->stats.incarnation_conflict_count += 1U;
        return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_INCARNATION_CONFLICT;
    }
    memset(entry, 0, sizeof(*entry));
    directory->directory_epoch += 1U;
    directory->stats.deregister_ok_count += 1U;
    return MEM_SERVICE_PROVIDER_DIRECTORY_RESULT_OK;
}

struct mem_service_provider_directory_poll
mem_service_provider_directory_poll(
    struct mem_service_provider_directory *directory,
    uint64_t now_ms)
{
    struct mem_service_provider_directory_poll poll;
    size_t i;

    memset(&poll, 0, sizeof(poll));
    if (directory == NULL) {
        return poll;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS; ++i) {
        struct mem_service_provider_directory_entry *entry =
            &directory->entries[i];

        if (entry->in_use &&
            !mem_service_provider_entry_fresh(directory, entry, now_ms)) {
            memset(entry, 0, sizeof(*entry));
            directory->directory_epoch += 1U;
            directory->stats.expired_count += 1U;
            poll.expired_this_poll += 1U;
        }
    }
    poll.required_count = directory->config.required_count;
    for (i = 0; i < MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS; ++i) {
        if (directory->entries[i].in_use) {
            poll.active_count += 1U;
        }
    }
    poll.ready = true;
    for (i = 0; i < directory->config.required_count; ++i) {
        const char *required = directory->config.required_nodes[i];

        if (mem_service_provider_directory_find(directory, required) == NULL) {
            poll.ready = false;
            if (poll.first_missing[0] == '\0') {
                snprintf(poll.first_missing,
                         sizeof(poll.first_missing),
                         "%s",
                         required);
            }
        }
    }
    return poll;
}

bool mem_service_provider_directory_data_ops_allowed(
    struct mem_service_provider_directory *directory,
    uint64_t now_ms)
{
    struct mem_service_provider_directory_poll poll;

    if (directory == NULL) {
        return true;
    }
    if (directory->config.required_count == 0) {
        return true;
    }
    poll = mem_service_provider_directory_poll(directory, now_ms);
    return poll.ready;
}

uint64_t mem_service_provider_directory_effective_lease_ms(
    const struct mem_service_provider_directory *directory)
{
    if (directory == NULL) {
        return MEM_SERVICE_PROVIDER_DIRECTORY_DEFAULT_LEASE_MS;
    }
    return mem_service_provider_directory_lease_ms(directory);
}

size_t mem_service_provider_directory_active_count(
    const struct mem_service_provider_directory *directory,
    uint64_t now_ms)
{
    size_t count = 0;
    size_t i;

    if (directory == NULL) {
        return 0;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_DIRECTORY_MAX_PROVIDERS; ++i) {
        if (mem_service_provider_entry_fresh(directory,
                                             &directory->entries[i],
                                             now_ms)) {
            count += 1U;
        }
    }
    return count;
}
