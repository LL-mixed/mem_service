/* Private worker disk codec; no process pointers are part of its disk format. */
#ifndef MEM_SERVICE_PROVIDER_OBMM_WORKER_LEDGER_H
#define MEM_SERVICE_PROVIDER_OBMM_WORKER_LEDGER_H

#include "../mem_service_client.h"
#include "../mem_service_provider.h"
#include <ub/obmm.h>
#include <ub/gsva.h>
#include <string.h>

struct worker_config {
    char connect[256];
    char node[MEM_SERVICE_CLIENT_PROVIDER_NODE_ID_LEN];
    char state[1024];
    uint64_t incarnation;
    uint64_t readiness_generation;
    uint64_t allocation_granularity_bytes;
    bool fast_allocation;
    unsigned char kernel_instance[16];
    uint64_t binding_version;
    uint64_t connect_length;
    uint64_t connect_hash[2];
    uint64_t state_length;
    uint64_t state_hash[2];
};

#define WORKER_LEDGER_CONFIG_BINDING_VERSION 1U

struct worker_reservation {
    bool occupied;
    bool export_no_backing;
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    uint64_t generation;
    struct obmm_gsva_segment_desc_v1 segment;
    struct obmm_cmd_export exported;
    struct mem_service_provider_descriptor descriptor;
};

#define WORKER_LEDGER_FRAME_BYTES 1024U

struct worker_ledger_entry {
    uint64_t sequence;
    char phase[32];
    struct worker_config config;
    struct mem_service_client_allocation work;
    struct worker_reservation reservation;
};

static uint64_t ledger_u64(unsigned char *bytes, uint64_t value, bool encode)
{
    uint64_t decoded = 0;
    unsigned i;
    for (i = 0; i < 8; i++) {
        if (encode) bytes[i] = (unsigned char)(value >> (8U * i));
        decoded |= (uint64_t)bytes[i] << (8U * i);
    }
    return decoded;
}

static uint64_t ledger_checksum(const unsigned char *bytes, size_t length)
{
    uint64_t value = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < length; i++) {
        value ^= bytes[i];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static uint64_t ledger_binding_hash(const char *value, uint64_t domain)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned i;

    for (i = 0; i < 8; ++i) {
        hash ^= (unsigned char)(domain >> (8U * i));
        hash *= UINT64_C(1099511628211);
    }
    do {
        hash ^= (unsigned char)*value;
        hash *= UINT64_C(1099511628211);
    } while (*value++ != '\0');
    return hash;
}

static void worker_config_set_binding(struct worker_config *config)
{
    config->binding_version = WORKER_LEDGER_CONFIG_BINDING_VERSION;
    config->connect_length = strlen(config->connect);
    config->connect_hash[0] = ledger_binding_hash(
        config->connect, UINT64_C(0x636f6e6e65637431));
    config->connect_hash[1] = ledger_binding_hash(
        config->connect, UINT64_C(0x636f6e6e65637432));
    config->state_length = strlen(config->state);
    config->state_hash[0] = ledger_binding_hash(
        config->state, UINT64_C(0x7374617465706174));
    config->state_hash[1] = ledger_binding_hash(
        config->state, UINT64_C(0x7374617465706132));
}

static bool worker_config_binding_matches(
    const struct worker_config *stored,
    const struct worker_config *current)
{
    return stored && current &&
           stored->binding_version == WORKER_LEDGER_CONFIG_BINDING_VERSION &&
           current->binding_version == WORKER_LEDGER_CONFIG_BINDING_VERSION &&
           stored->connect_length == current->connect_length &&
           stored->connect_hash[0] == current->connect_hash[0] &&
           stored->connect_hash[1] == current->connect_hash[1] &&
           stored->state_length == current->state_length &&
           stored->state_hash[0] == current->state_hash[0] &&
           stored->state_hash[1] == current->state_hash[1] &&
           stored->fast_allocation == current->fast_allocation;
}

static int ledger_string(unsigned char *bytes, char *value, size_t length, bool encode)
{
    size_t used;
    if (!encode) memcpy(value, bytes, length);
    used = strnlen(value, length);
    if (used == length) return -1;
    if (encode) {
        memset(bytes, 0, length);
        memcpy(bytes, value, used);
    } else {
        size_t i;
        for (i = used; i < length; i++) if (bytes[i]) return -1;
    }
    return 0;
}

static int ledger_fields(unsigned char frame[WORKER_LEDGER_FRAME_BYTES],
                         struct worker_ledger_entry *entry, bool encode,
                         unsigned format_version)
{
    size_t offset = 24, i;
    struct worker_reservation *r = &entry->reservation;
    struct obmm_gsva_segment_desc_v1 *s = &r->segment;
    struct obmm_cmd_export *e = &r->exported;
#define FIELD(value) do { \
    uint64_t decoded = ledger_u64(frame + offset, (uint64_t)(value), encode); \
    if (!encode) (value) = decoded; \
    offset += 8; \
} while (0)
#define STRING(value) do { \
    if (ledger_string(frame + offset, value, sizeof(value), encode)) return -1; \
    offset += sizeof(value); \
} while (0)
    FIELD(entry->sequence);
    STRING(entry->phase);
    STRING(entry->config.node);
    STRING(entry->work.key);
    FIELD(entry->config.incarnation);
    FIELD(entry->config.readiness_generation);
    FIELD(entry->config.allocation_granularity_bytes);
    FIELD(entry->work.generation);
    FIELD(entry->work.size_bytes);
    FIELD(entry->work.alignment_bytes);
    FIELD(entry->work.capabilities);
    FIELD(r->occupied);
    FIELD(r->export_no_backing);
    FIELD(s->version); FIELD(s->flags); FIELD(s->segment_id);
    FIELD(s->home_va); FIELD(s->size); FIELD(s->epoch);
    FIELD(s->home_cna); FIELD(s->owner_node_id); FIELD(s->node_count);
    FIELD(s->cache_policy); FIELD(s->p_tag); FIELD(s->access_flags);
    FIELD(s->token_id); FIELD(s->token_value);
    for (i = 0; i < OBMM_MAX_LOCAL_NUMA_NODES; i++) FIELD(e->size[i]);
    FIELD(e->length); FIELD(e->flags); FIELD(e->uba); FIELD(e->mem_id);
    FIELD(e->tokenid); FIELD(e->pxm_numa);
    if (e->priv_len || e->vendor_len || e->vendor_info || e->priv) return -1;
    if (encode) {
        memcpy(frame + offset, e->deid, sizeof(e->deid));
        memcpy(frame + offset + sizeof(e->deid), e->seid, sizeof(e->seid));
    } else {
        memcpy(e->deid, frame + offset, sizeof(e->deid));
        memcpy(e->seid, frame + offset + sizeof(e->deid), sizeof(e->seid));
    }
    offset += sizeof(e->deid) + sizeof(e->seid);
    FIELD(r->descriptor.len);
    if (r->descriptor.len > sizeof(r->descriptor.bytes)) return -1;
    if (encode) memcpy(frame + offset, r->descriptor.bytes, r->descriptor.len);
    else memcpy(r->descriptor.bytes, frame + offset, sizeof(r->descriptor.bytes));
    offset += sizeof(r->descriptor.bytes);
    if (encode) memcpy(frame + offset, entry->config.kernel_instance, 16);
    else memcpy(entry->config.kernel_instance, frame + offset, 16);
    offset += 16;
    for (i = 0; i < 16 && !entry->config.kernel_instance[i]; i++) {}
    if (i == 16) return -1;
    if (format_version >= 3) {
        FIELD(entry->config.connect_length);
        FIELD(entry->config.connect_hash[0]);
        FIELD(entry->config.connect_hash[1]);
        FIELD(entry->config.state_length);
        FIELD(entry->config.state_hash[0]);
        FIELD(entry->config.state_hash[1]);
        FIELD(entry->config.fast_allocation);
        if (!entry->config.connect_length ||
            entry->config.connect_length >= sizeof(entry->config.connect) ||
            !entry->config.state_length ||
            entry->config.state_length >= sizeof(entry->config.state)) {
            return -1;
        }
    }
#undef FIELD
#undef STRING
    return offset <= WORKER_LEDGER_FRAME_BYTES - 8 ? 0 : -1;
}

static int ledger_encode_version(unsigned char frame[WORKER_LEDGER_FRAME_BYTES],
                                 struct worker_ledger_entry *entry,
                                 unsigned format_version)
{
    const char *magic;

    if (format_version != 2 && format_version != 3) return -1;
    magic = format_version == 3 ? "obmm-worker-ledger-v3" :
                                  "obmm-worker-ledger-v2";
    memset(frame, 0, WORKER_LEDGER_FRAME_BYTES);
    memcpy(frame, magic, strlen(magic));
    if (ledger_fields(frame, entry, true, format_version)) return -1;
    ledger_u64(frame + WORKER_LEDGER_FRAME_BYTES - 8,
               ledger_checksum(frame, WORKER_LEDGER_FRAME_BYTES - 8), true);
    return 0;
}

static int ledger_encode(unsigned char frame[WORKER_LEDGER_FRAME_BYTES],
                         struct worker_ledger_entry *entry)
{
    return ledger_encode_version(
        frame, entry,
        entry->config.binding_version == WORKER_LEDGER_CONFIG_BINDING_VERSION ?
            3U : 2U);
}

static int ledger_decode(unsigned char frame[WORKER_LEDGER_FRAME_BYTES],
                         struct worker_ledger_entry *entry)
{
    unsigned char canonical[WORKER_LEDGER_FRAME_BYTES];
    unsigned format_version;

    memset(entry, 0, sizeof(*entry));
    if (!memcmp(frame, "obmm-worker-ledger-v3",
                sizeof("obmm-worker-ledger-v3") - 1)) {
        format_version = 3;
        entry->config.binding_version =
            WORKER_LEDGER_CONFIG_BINDING_VERSION;
    } else if (!memcmp(frame, "obmm-worker-ledger-v2",
                       sizeof("obmm-worker-ledger-v2") - 1)) {
        format_version = 2;
    } else {
        return -1;
    }
    if (ledger_u64(frame + WORKER_LEDGER_FRAME_BYTES - 8, 0, false) !=
        ledger_checksum(frame, WORKER_LEDGER_FRAME_BYTES - 8) ||
        ledger_fields(frame, entry, false, format_version) ||
        ledger_encode_version(canonical, entry, format_version) ||
        memcmp(frame, canonical, sizeof(canonical))) return -1;
    return 0;
}

#endif
