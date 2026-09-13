#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mem_service_provider_obmm.h"

#ifdef __linux__
#include "common/obmm_common.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MEM_SERVICE_OBMM_DESCRIPTOR_MAGIC 0x4d534f42U
#define MEM_SERVICE_OBMM_DESCRIPTOR_VERSION 1U
#define MEM_SERVICE_OBMM_DESCRIPTOR_BYTES 48U
#define MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_VERSION 2U
#define MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES 96U
/* Simulator-private opt-in bit: old kernels reject unknown import flags. */
#define MEM_SERVICE_OBMM_IMPORT_FLAG_GSVA_V4 0x8UL
#define MEM_SERVICE_OBMM_DEFAULT_DEVICE "/dev/obmm"
#define MEM_SERVICE_OBMM_DEFAULT_CNA_PATH \
    "/sys/bus/ub/devices/00001/primary_cna"

struct mem_service_obmm_descriptor_v1 {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
    bool strict_gsva;
    uint64_t segment_id;
    uint64_t epoch;
    uint32_t segment_flags;
    uint32_t owner_node;
    uint32_t node_count;
    uint32_t cache_policy;
    uint32_t p_tag;
    uint32_t access_flags;
    uint32_t gsva_token_id;
    uint32_t gsva_token_value;
};

#ifdef __linux__
struct mem_service_obmm_region_slot {
    bool active;
    uint64_t handle;
    struct mem_service_obmm_descriptor_v1 descriptor;
};

struct mem_service_obmm_view {
    struct {
        void *address;
        uint64_t len;
        bool owned;
    } parts[3];
};

struct mem_service_obmm_mapping_slot {
    bool active;
    bool imported;
    bool map_osync;
    bool close_uncertain;
    uint64_t handle;
    uint64_t view_offset;
    uint64_t view_len;
    uint64_t view_access;
    uint64_t compute_pins;
    struct mem_service_obmm_descriptor_v1 descriptor;
    struct obmm_helpers_region region;
    struct mem_service_obmm_view view;
    struct mem_service_obmm_view probe_view;
};

struct mem_service_obmm_context {
    int obmm_fd;
    bool closing;
    bool close_uncertain;
    uint32_t local_cna;
    bool mapping_verified;
    bool force_osync;
    uint32_t max_remote_mappings;
    uint32_t required_peer_mappings;
    uint32_t verified_peer_count;
    uint64_t next_region_handle;
    uint64_t next_mapping_handle;
    uint64_t import_region_bytes;
    char instance[MEM_SERVICE_PROVIDER_INSTANCE_LEN];
    uint64_t import_pas[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    bool import_osync[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    struct mem_service_obmm_descriptor_v1
        verified_peer_descriptors[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    struct mem_service_obmm_region_slot
        regions[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    struct mem_service_obmm_mapping_slot
        mappings[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
};

struct mem_service_provider_obmm_mapping_pin {
    struct mem_service_obmm_context *context;
    uint64_t mapping_handle;
};
#endif

static void mem_service_obmm_put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint32_t mem_service_obmm_get_u32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static void mem_service_obmm_put_u64(uint8_t *bytes, uint64_t value)
{
    mem_service_obmm_put_u32(bytes, (uint32_t)(value >> 32));
    mem_service_obmm_put_u32(bytes + 4, (uint32_t)value);
}

static uint64_t mem_service_obmm_get_u64(const uint8_t *bytes)
{
    return ((uint64_t)mem_service_obmm_get_u32(bytes) << 32) |
           mem_service_obmm_get_u32(bytes + 4);
}

static bool mem_service_obmm_gsva_valid(
    const struct mem_service_obmm_descriptor_v1 *d)
{
    /* GSVA v1 requires strict identity, token value and ACTIVE, not RETIRED. */
    return d->segment_id && d->epoch && d->segment_flags == 7U &&
           d->node_count && d->owner_node < d->node_count &&
           d->gsva_token_id && d->gsva_token_value &&
           d->cache_policy <= 4U &&
           d->access_flags && !(d->access_flags & ~3U) &&
           d->remote_uba && !(d->remote_uba & 4095U) && !(d->size & 4095U) &&
           d->size <= UINT64_MAX - d->remote_uba;
}

static int mem_service_obmm_descriptor_encode(
    const struct mem_service_obmm_descriptor_v1 *descriptor,
    struct mem_service_provider_descriptor *opaque_out)
{
    uint8_t *bytes;

    if (descriptor == NULL || opaque_out == NULL ||
        descriptor->export_mem_id == 0 || descriptor->size == 0 ||
        descriptor->token_id == 0 || descriptor->export_cna == 0 ||
        (descriptor->strict_gsva && !mem_service_obmm_gsva_valid(descriptor))) {
        return -1;
    }
    memset(opaque_out, 0, sizeof(*opaque_out));
    opaque_out->len = descriptor->strict_gsva ? MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES :
                                               MEM_SERVICE_OBMM_DESCRIPTOR_BYTES;
    bytes = opaque_out->bytes;
    mem_service_obmm_put_u32(bytes, MEM_SERVICE_OBMM_DESCRIPTOR_MAGIC);
    mem_service_obmm_put_u32(bytes + 4, descriptor->strict_gsva ?
        MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_VERSION : MEM_SERVICE_OBMM_DESCRIPTOR_VERSION);
    mem_service_obmm_put_u32(bytes + 8, opaque_out->len);
    mem_service_obmm_put_u32(bytes + 12, 0);
    mem_service_obmm_put_u64(bytes + 16, descriptor->export_mem_id);
    mem_service_obmm_put_u64(bytes + 24, descriptor->remote_uba);
    mem_service_obmm_put_u64(bytes + 32, descriptor->size);
    mem_service_obmm_put_u32(bytes + 40, descriptor->token_id);
    mem_service_obmm_put_u32(bytes + 44, descriptor->export_cna);
    if (descriptor->strict_gsva) {
        mem_service_obmm_put_u64(bytes + 48, descriptor->segment_id);
        mem_service_obmm_put_u64(bytes + 56, descriptor->epoch);
        mem_service_obmm_put_u32(bytes + 64, descriptor->segment_flags);
        mem_service_obmm_put_u32(bytes + 68, descriptor->owner_node);
        mem_service_obmm_put_u32(bytes + 72, descriptor->node_count);
        mem_service_obmm_put_u32(bytes + 76, descriptor->cache_policy);
        mem_service_obmm_put_u32(bytes + 80, descriptor->p_tag);
        mem_service_obmm_put_u32(bytes + 84, descriptor->access_flags);
        mem_service_obmm_put_u32(bytes + 88, descriptor->gsva_token_id);
        mem_service_obmm_put_u32(bytes + 92, descriptor->gsva_token_value);
    }
    return 0;
}

static int mem_service_obmm_descriptor_decode(
    const struct mem_service_provider_descriptor *opaque,
    struct mem_service_obmm_descriptor_v1 *descriptor_out)
{
    struct mem_service_obmm_descriptor_v1 descriptor;
    const uint8_t *bytes;

    if (opaque == NULL || descriptor_out == NULL ||
        (opaque->len != MEM_SERVICE_OBMM_DESCRIPTOR_BYTES &&
         opaque->len != MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES)) {
        return -1;
    }
    bytes = opaque->bytes;
    if (mem_service_obmm_get_u32(bytes) !=
            MEM_SERVICE_OBMM_DESCRIPTOR_MAGIC ||
        mem_service_obmm_get_u32(bytes + 4) !=
            (opaque->len == MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES ?
             MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_VERSION : MEM_SERVICE_OBMM_DESCRIPTOR_VERSION) ||
        mem_service_obmm_get_u32(bytes + 8) != opaque->len ||
        mem_service_obmm_get_u32(bytes + 12) != 0) {
        return -1;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.export_mem_id = mem_service_obmm_get_u64(bytes + 16);
    descriptor.remote_uba = mem_service_obmm_get_u64(bytes + 24);
    descriptor.size = mem_service_obmm_get_u64(bytes + 32);
    descriptor.token_id = mem_service_obmm_get_u32(bytes + 40);
    descriptor.export_cna = mem_service_obmm_get_u32(bytes + 44);
    descriptor.strict_gsva = opaque->len == MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES;
    if (descriptor.strict_gsva) {
        descriptor.segment_id = mem_service_obmm_get_u64(bytes + 48);
        descriptor.epoch = mem_service_obmm_get_u64(bytes + 56);
        descriptor.segment_flags = mem_service_obmm_get_u32(bytes + 64);
        descriptor.owner_node = mem_service_obmm_get_u32(bytes + 68);
        descriptor.node_count = mem_service_obmm_get_u32(bytes + 72);
        descriptor.cache_policy = mem_service_obmm_get_u32(bytes + 76);
        descriptor.p_tag = mem_service_obmm_get_u32(bytes + 80);
        descriptor.access_flags = mem_service_obmm_get_u32(bytes + 84);
        descriptor.gsva_token_id = mem_service_obmm_get_u32(bytes + 88);
        descriptor.gsva_token_value = mem_service_obmm_get_u32(bytes + 92);
        if (!mem_service_obmm_gsva_valid(&descriptor)) return -1;
    }
    if (descriptor.export_mem_id == 0 || descriptor.size == 0 ||
        descriptor.token_id == 0 || descriptor.export_cna == 0) {
        return -1;
    }
    *descriptor_out = descriptor;
    return 0;
}

static bool mem_service_obmm_descriptor_equal(
    const struct mem_service_obmm_descriptor_v1 *left,
    const struct mem_service_obmm_descriptor_v1 *right)
{
    return left != NULL && right != NULL &&
           left->export_mem_id == right->export_mem_id &&
           left->remote_uba == right->remote_uba &&
           left->size == right->size && left->token_id == right->token_id &&
           left->export_cna == right->export_cna &&
           left->strict_gsva == right->strict_gsva &&
           (!left->strict_gsva ||
            (left->segment_id == right->segment_id && left->epoch == right->epoch &&
             left->segment_flags == right->segment_flags &&
             left->owner_node == right->owner_node && left->node_count == right->node_count &&
             left->cache_policy == right->cache_policy && left->p_tag == right->p_tag &&
             left->access_flags == right->access_flags &&
             left->gsva_token_id == right->gsva_token_id &&
             left->gsva_token_value == right->gsva_token_value));
}

#ifdef __linux__
static bool mem_service_obmm_parse_u32_file(const char *path,
                                            uint32_t *value_out)
{
    char text[128];
    char *end = NULL;
    unsigned long value;
    FILE *fp;

    if (path == NULL || value_out == NULL || (fp = fopen(path, "r")) == NULL) {
        return false;
    }
    if (fgets(text, sizeof(text), fp) == NULL) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || value == 0 || value > UINT32_MAX) {
        return false;
    }
    *value_out = (uint32_t)value;
    return true;
}
#endif

int mem_service_provider_obmm_encode_gsva(
    const struct obmm_gsva_segment_desc_v1 *segment,
    const struct obmm_cmd_export *exported,
    struct mem_service_provider_descriptor *descriptor_out)
{
#ifdef __linux__
    struct mem_service_obmm_descriptor_v1 d = {0};

    if (!segment || !exported || !descriptor_out ||
        segment->version != OBMM_GSVA_ABI_VERSION ||
        exported->length != 1 || exported->uba != segment->home_va ||
        exported->size[0] != segment->size ||
        !(exported->flags & OBMM_EXPORT_FLAG_GSVA_FIXED_UBA)) return -1;
    d.export_mem_id = exported->mem_id;
    d.remote_uba = segment->home_va;
    d.size = segment->size;
    d.token_id = exported->tokenid;
    d.export_cna = segment->home_cna;
    d.strict_gsva = true;
    d.segment_id = segment->segment_id;
    d.epoch = segment->epoch;
    d.segment_flags = segment->flags;
    d.owner_node = segment->owner_node_id;
    d.node_count = segment->node_count;
    d.cache_policy = segment->cache_policy;
    d.p_tag = segment->p_tag;
    d.access_flags = segment->access_flags;
    d.gsva_token_id = segment->token_id;
    d.gsva_token_value = segment->token_value;
    return mem_service_obmm_descriptor_encode(&d, descriptor_out);
#else
    (void)segment;
    (void)exported;
    (void)descriptor_out;
    errno = ENOTSUP;
    return -1;
#endif
}

int mem_service_provider_obmm_probe_device(const char *device_path,
                                           const char *primary_cna_path,
                                           char *detail,
                                           size_t detail_len)
{
#ifndef __linux__
    (void)device_path;
    (void)primary_cna_path;
    if (detail == NULL || detail_len == 0) {
        return -1;
    }
    snprintf(detail, detail_len, "platform=non-linux error=unsupported");
    return -1;
#else
    const char *device = device_path != NULL
                             ? device_path
                             : MEM_SERVICE_OBMM_DEFAULT_DEVICE;
    const char *cna_path = primary_cna_path != NULL
                               ? primary_cna_path
                               : MEM_SERVICE_OBMM_DEFAULT_CNA_PATH;
    uint32_t cna = 0;
    int fd;

    if (detail == NULL || detail_len == 0) {
        return -1;
    }
    fd = open(device, O_RDWR);
    if (fd < 0) {
        snprintf(detail,
                 detail_len,
                 "device=%s error=%s",
                 device,
                 strerror(errno));
        return -1;
    }
    close(fd);
    if (!mem_service_obmm_parse_u32_file(cna_path, &cna)) {
        snprintf(detail,
                 detail_len,
                 "device=%s cna_path=%s error=invalid-primary-cna",
                 device,
                 cna_path);
        return -1;
    }
    snprintf(detail,
             detail_len,
             "device=%s primary_cna=%#x mapping_path=sim-dec",
             device,
             cna);
    return 0;
#endif
}

#ifdef __linux__
static struct mem_service_obmm_region_slot *mem_service_obmm_find_region(
    struct mem_service_obmm_context *context,
    uint64_t handle)
{
    size_t i;

    if (context == NULL || handle == 0) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->regions[i].active &&
            context->regions[i].handle == handle) {
            return &context->regions[i];
        }
    }
    return NULL;
}

static struct mem_service_obmm_mapping_slot *mem_service_obmm_find_mapping(
    struct mem_service_obmm_context *context,
    uint64_t handle)
{
    size_t i;

    if (context == NULL || handle == 0) {
        return NULL;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->mappings[i].active &&
            context->mappings[i].handle == handle) {
            return &context->mappings[i];
        }
    }
    return NULL;
}

static int mem_service_obmm_provider_probe(
    void *opaque,
    enum mem_service_provider_state *state_out)
{
    struct mem_service_obmm_context *context = opaque;

    if (context == NULL || state_out == NULL) {
        return -1;
    }
    if (context->closing || context->obmm_fd < 0 ||
        fcntl(context->obmm_fd, F_GETFD) < 0) {
        *state_out = MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;
    } else if (!context->mapping_verified ||
               context->verified_peer_count <
                   context->required_peer_mappings) {
        *state_out = MEM_SERVICE_PROVIDER_STATE_DEGRADED;
    } else {
        *state_out = MEM_SERVICE_PROVIDER_STATE_READY;
    }
    return 0;
}

static int mem_service_obmm_provider_register_region(
    void *opaque,
    const struct mem_service_region_request *request,
    struct mem_service_region *region_out)
{
    struct mem_service_obmm_context *context = opaque;
    struct mem_service_obmm_region_slot *slot = NULL;
    struct obmm_helpers_meta meta;
    size_t i;

    if (context == NULL || context->closing || context->next_region_handle == UINT64_MAX ||
        request == NULL || region_out == NULL ||
        request->base != NULL || request->len == 0 ||
        request->memory_kind != MEM_SERVICE_MEMORY_HOST ||
        request->flags != MEM_SERVICE_REGION_FLAG_PROVIDER_ALLOCATED) {
        return -1;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (!context->regions[i].active) {
            slot = &context->regions[i];
            break;
        }
    }
    if (slot == NULL) {
        return -1;
    }
    memset(&meta, 0, sizeof(meta));
    meta.export_cna = context->local_cna;
    if (obmm_do_export(context->obmm_fd, &meta, request->len) != 0) {
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->handle = ++context->next_region_handle;
    slot->descriptor.export_mem_id = meta.export_mem_id;
    slot->descriptor.remote_uba = meta.remote_uba;
    slot->descriptor.size = meta.size;
    slot->descriptor.token_id = meta.token_id;
    slot->descriptor.export_cna = context->local_cna;
    memset(region_out, 0, sizeof(*region_out));
    region_out->handle = slot->handle;
    region_out->len = request->len;
    region_out->memory_kind = request->memory_kind;
    if (mem_service_obmm_descriptor_encode(&slot->descriptor,
                                           &region_out->descriptor) != 0) {
        if (obmm_do_unexport(context->obmm_fd, slot->descriptor.export_mem_id) == 0)
            memset(slot, 0, sizeof(*slot));
        else
            context->closing = true;
        memset(region_out, 0, sizeof(*region_out));
        return -1;
    }
    return 0;
}

static int mem_service_obmm_provider_deregister_region(void *opaque,
                                                       uint64_t region_handle)
{
    struct mem_service_obmm_context *context = opaque;
    struct mem_service_obmm_region_slot *slot =
        mem_service_obmm_find_region(context, region_handle);
    size_t i;

    if (slot == NULL) {
        return -1;
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->mappings[i].active &&
            mem_service_obmm_descriptor_equal(
                &context->mappings[i].descriptor, &slot->descriptor)) {
            return -1;
        }
    }
    if (obmm_do_unexport(context->obmm_fd,
                         slot->descriptor.export_mem_id) != 0) {
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    return 0;
}

static bool mem_service_obmm_descriptor_is_local(
    const struct mem_service_obmm_context *context,
    const struct mem_service_obmm_descriptor_v1 *descriptor)
{
    size_t i;

    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->regions[i].active &&
            mem_service_obmm_descriptor_equal(
                &context->regions[i].descriptor, descriptor)) {
            return true;
        }
    }
    return false;
}

static int mem_service_obmm_import_gsva(
    struct mem_service_obmm_context *context,
    const struct mem_service_obmm_descriptor_v1 *d,
    uint64_t local_pa, uint64_t *import_mem_id)
{
    struct {
        struct obmm_sim_dec_import_priv_v2 gsva;
        uint32_t gsva_token_id;
        uint32_t reserved;
    } priv = {0};
    struct obmm_mem_desc *request;
    mem_id imported;
    int numa = 0;

    /* Private import v4 preserves both identities. Older kernels reject it;
     * never substitute either token or retry with the legacy import. */
    priv.gsva.magic = OBMM_SIM_DEC_PRIV_MAGIC;
    priv.gsva.version = 4;
    priv.gsva.len = sizeof(priv);
    priv.gsva.remote_uba = d->remote_uba;
    priv.gsva.token_value = d->gsva_token_value;
    priv.gsva.map_source = OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER;
    priv.gsva.address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY;
    priv.gsva.cache_policy = d->cache_policy;
    priv.gsva.local_va = d->remote_uba;
    priv.gsva.home_va = d->remote_uba;
    priv.gsva.p_tag = d->p_tag;
    priv.gsva.access_flags = d->access_flags;
    priv.gsva.gva_id = d->segment_id;
    priv.gsva.segment_id = d->segment_id;
    priv.gsva.epoch = d->epoch;
    priv.gsva_token_id = d->gsva_token_id;
    request = calloc(1, sizeof(*request) + sizeof(priv));
    if (!request) return -1;
    request->addr = local_pa;
    request->length = d->size;
    request->tokenid = d->token_id;
    request->scna = context->local_cna;
    request->dcna = d->export_cna;
    request->priv_len = sizeof(priv);
    memcpy(request->priv, &priv, sizeof(priv));
    imported = obmm_import(request, OBMM_IMPORT_FLAG_ALLOW_MMAP |
                           MEM_SERVICE_OBMM_IMPORT_FLAG_GSVA_V4, 0, &numa);
    free(request);
    if (imported == OBMM_INVALID_MEMID) return -1;
    *import_mem_id = (uint64_t)imported;
    return 0;
}

static int mem_service_obmm_unmap_view(struct mem_service_obmm_view *view)
{
    int rc = 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (!view->parts[i].owned) continue;
        if (munmap(view->parts[i].address, view->parts[i].len) != 0) {
            rc = -1;
        } else {
            memset(&view->parts[i], 0, sizeof(view->parts[i]));
        }
    }
    return rc;
}

/* OBMM forbids VMA splitting and mprotect. Retain the exact ranges actually
 * created, including an unexpected mmap address, until cleanup is confirmed. */
static int mem_service_obmm_map_view(void *address, uint64_t size,
    uint64_t offset, uint64_t len, int prot, int flags, int fd,
    struct mem_service_obmm_view *view)
{
    long page_result = sysconf(_SC_PAGESIZE);
    uint64_t page, bounds[4];
    unsigned i;

    if (!view || !address || !len || page_result <= 0 || offset > size ||
        len > size - offset || size > SIZE_MAX ||
        size > UINTPTR_MAX - (uintptr_t)address) return -1;
    page = (uint64_t)page_result;
    if ((page & (page - 1)) || (uintptr_t)address % page || size % page) return -1;
    for (i = 0; i < 3; ++i) {
        if (view->parts[i].owned) return -1;
    }
    memset(view, 0, sizeof(*view));
    bounds[0] = 0;
    bounds[1] = offset & ~(page - 1);
    bounds[2] = offset + len;
    if (bounds[2] % page) bounds[2] += page - bounds[2] % page;
    bounds[3] = size;
    for (i = 0; i < 3; ++i) {
        void *wanted = (uint8_t *)address + bounds[i];
        uint64_t length = bounds[i + 1] - bounds[i];
        if (!length) continue;
        void *actual = mmap(wanted, length, i == 1 ? prot : PROT_NONE,
                            flags, fd, (off_t)bounds[i]);
        if (actual != MAP_FAILED) {
            view->parts[i].address = actual;
            view->parts[i].len = length;
            view->parts[i].owned = true;
        }
        if (actual == MAP_FAILED || actual != wanted) {
            int map_errno = actual == MAP_FAILED ? errno : EADDRNOTAVAIL;
            int cleanup_result;
            fprintf(stderr, "mem_service obmm-map: result=failed stage=mmap "
                    "fixed_va=0x%016" PRIxPTR " len=%" PRIu64 " errno=%d\n",
                    (uintptr_t)wanted, length, map_errno);
            cleanup_result = mem_service_obmm_unmap_view(view);
            errno = map_errno;
            return cleanup_result == 0 ? -1 : MEM_SERVICE_MAPPING_CLEANUP_REQUIRED;
        }
    }
    return 0;
}

static int mem_service_obmm_map_strict(uint64_t mem_id,
    const struct mem_service_obmm_descriptor_v1 *d, uint32_t access,
    uint64_t offset, uint64_t len, bool osync, struct obmm_helpers_region *region,
    struct mem_service_obmm_view *view)
{
    char path[128];
    int prot = 0;

    if (access & MEM_SERVICE_MAPPING_FLAG_READ) prot |= PROT_READ;
    if (access & MEM_SERVICE_MAPPING_FLAG_WRITE) prot |= PROT_WRITE;
    snprintf(path, sizeof(path), "/dev/obmm_shmdev%" PRIu64, mem_id);
    region->mem_id = mem_id;
    region->len = d->size;
    region->fd = open(path, ((prot & PROT_WRITE) ? O_RDWR : O_RDONLY) |
                            (osync ? O_SYNC : 0));
    if (region->fd < 0) return -1;
    region->addr = (void *)(uintptr_t)d->remote_uba;
    if (mem_service_obmm_map_view(region->addr, d->size, offset, len, prot,
          MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_GSVA, region->fd, view)) {
        /* The caller owns fd, import and any residual VMAs, even on failure. */
        region->addr = NULL;
        return -1;
    }
    return 0;
}

static int mem_service_obmm_provider_unmap_remote_region(void *opaque,
                                                        uint64_t mapping_handle);

static int mem_service_obmm_provider_map_remote_region(
    void *opaque,
    const struct mem_service_mapping_request *request,
    struct mem_service_mapping *mapping_out)
{
    struct mem_service_obmm_context *context = opaque;
    struct mem_service_obmm_mapping_slot *slot = NULL;
    struct mem_service_obmm_descriptor_v1 descriptor;
    struct obmm_helpers_meta meta;
    uint64_t import_mem_id = 0;
    bool local;
    bool map_osync;
    size_t slot_index = 0;
    size_t i;

    if (context == NULL || context->closing || request == NULL || mapping_out == NULL ||
        context->next_mapping_handle == UINT64_MAX ||
        request->memory_kind != MEM_SERVICE_MEMORY_HOST || request->len == 0 ||
        (request->flags & ~MEM_SERVICE_MAPPING_FLAG_VALID_MASK) != 0 ||
        (request->flags & (MEM_SERVICE_MAPPING_FLAG_READ |
                           MEM_SERVICE_MAPPING_FLAG_WRITE)) == 0 ||
        request->offset > request->remote_region_len ||
        request->len > request->remote_region_len - request->offset ||
        ((request->flags & MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS) != 0 &&
         (request->requested_address == NULL || request->offset != 0)) ||
        ((request->flags & MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS) == 0 &&
         request->requested_address != NULL) ||
        mem_service_obmm_descriptor_decode(&request->remote_descriptor,
                                           &descriptor) != 0 ||
        descriptor.size != request->remote_region_len) {
        return -1;
    }
    if (descriptor.strict_gsva &&
        ((request->requested_address != NULL &&
          (uintptr_t)request->requested_address != descriptor.remote_uba) ||
         ((request->flags & MEM_SERVICE_MAPPING_FLAG_READ) &&
          !(descriptor.access_flags & OBMM_GSVA_ACCESS_READ)) ||
         ((request->flags & MEM_SERVICE_MAPPING_FLAG_WRITE) &&
          !(descriptor.access_flags & OBMM_GSVA_ACCESS_WRITE)) ||
         descriptor.size > context->import_region_bytes)) {
        return -1;
    }
    for (i = 0; i < context->max_remote_mappings; ++i) {
        if (!context->mappings[i].active) {
            slot = &context->mappings[i];
            slot_index = i;
            break;
        }
    }
    if (slot == NULL) {
        return -1;
    }
    local = descriptor.strict_gsva ? descriptor.export_cna == context->local_cna :
                                    mem_service_obmm_descriptor_is_local(context, &descriptor);
    map_osync = context->force_osync ||
                (!local && context->import_osync[slot_index]);
    if (local) {
        import_mem_id = descriptor.export_mem_id;
    } else if (descriptor.strict_gsva) {
        if (mem_service_obmm_import_gsva(context, &descriptor,
                                         context->import_pas[slot_index],
                                         &import_mem_id) != 0) return -1;
    } else {
        memset(&meta, 0, sizeof(meta));
        meta.export_mem_id = descriptor.export_mem_id;
        meta.remote_uba = descriptor.remote_uba;
        meta.size = descriptor.size;
        meta.token_id = descriptor.token_id;
        meta.export_cna = descriptor.export_cna;
        if (obmm_do_import(context->obmm_fd,
                           &meta,
                           context->local_cna,
                           context->import_pas[slot_index],
                           descriptor.token_id,
                           &import_mem_id) != 0) {
            return -1;
        }
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->imported = !local;
    slot->map_osync = map_osync;
    slot->handle = ++context->next_mapping_handle;
    slot->descriptor = descriptor;
    slot->region.fd = -1;
    slot->region.mem_id = import_mem_id;
    if ((descriptor.strict_gsva ?
         mem_service_obmm_map_strict(import_mem_id, &descriptor, request->flags,
                                     request->offset, request->len, map_osync,
                                     &slot->region, &slot->view) :
         obmm_map_region_at(import_mem_id,
                           request->requested_address,
                           descriptor.size,
                           map_osync,
                           &slot->region)) != 0) {
        if (mem_service_obmm_provider_unmap_remote_region(context, slot->handle) != 0) {
            memset(mapping_out, 0, sizeof(*mapping_out));
            mapping_out->handle = slot->handle;
            mapping_out->memory_kind = request->memory_kind;
            return MEM_SERVICE_MAPPING_CLEANUP_REQUIRED;
        }
        return -1;
    }
    if (!descriptor.strict_gsva) {
        slot->view.parts[0].address = slot->region.addr;
        slot->view.parts[0].len = slot->region.len;
        slot->view.parts[0].owned = true;
    }
    slot->view_offset = request->offset;
    slot->view_len = request->len;
    slot->view_access = request->flags &
        (MEM_SERVICE_MAPPING_FLAG_READ | MEM_SERVICE_MAPPING_FLAG_WRITE);
    memset(mapping_out, 0, sizeof(*mapping_out));
    mapping_out->handle = slot->handle;
    mapping_out->base = (uint8_t *)slot->region.addr + request->offset;
    mapping_out->len = request->len;
    mapping_out->memory_kind = request->memory_kind;
    return 0;
}

static int mem_service_obmm_provider_unmap_remote_region(
    void *opaque,
    uint64_t mapping_handle)
{
    struct mem_service_obmm_context *context = opaque;
    struct mem_service_obmm_mapping_slot *slot =
        mem_service_obmm_find_mapping(context, mapping_handle);
    if (slot == NULL) {
        return -1;
    }
    if (slot->compute_pins) {
        return -EBUSY;
    }
    slot->view_len = 0;
    slot->view_access = 0;
    slot->region.addr = NULL;
    if (mem_service_obmm_unmap_view(&slot->probe_view) != 0 ||
        mem_service_obmm_unmap_view(&slot->view) != 0 || slot->close_uncertain)
        return -1;
    if (slot->region.fd >= 0) {
        int fd = slot->region.fd;
        slot->region.fd = -1;
        if (close(fd) != 0) {
            /* Linux may already have released fd: never retry its number. */
            slot->close_uncertain = true;
            return -1;
        }
    }
    if (slot->imported &&
        obmm_do_unimport(context->obmm_fd, slot->region.mem_id) != 0) {
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    slot->region.fd = -1;
    return 0;
}

static int mem_service_obmm_update_range(
    const struct mem_service_obmm_mapping_slot *slot,
    uint64_t offset,
    uint64_t len,
    uint8_t cache_op)
{
    struct obmm_cmd_update_range command;
    uintptr_t start;
    uintptr_t end;
    uintptr_t page_size;

    if (slot == NULL || slot->region.addr == NULL || slot->region.fd < 0 ||
        offset > slot->view_len || len == 0 || len > slot->view_len - offset) {
        return -1;
    }
    start = (uintptr_t)slot->region.addr + slot->view_offset + offset;
    end = start + len;
    page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    if (page_size == 0) {
        page_size = 4096;
    }
    memset(&command, 0, sizeof(command));
    command.start = start & ~(page_size - 1U);
    command.end = (end + page_size - 1U) & ~(page_size - 1U);
    command.mem_state = (slot->map_osync ? OBMM_SHM_MEM_NORMAL_NC
                                        : OBMM_SHM_MEM_NORMAL) |
                        OBMM_SHM_MEM_READWRITE;
    command.cache_ops = cache_op;
    return ioctl(slot->region.fd, OBMM_SHMDEV_UPDATE_RANGE, &command) == 0
               ? 0
               : -1;
}

static int mem_service_obmm_sync_import_range(
    const struct mem_service_obmm_mapping_slot *slot,
    uint64_t offset,
    uint64_t len)
{
    struct obmm_cmd_sync_import_range command;

    if (slot == NULL || !slot->imported || !slot->map_osync ||
        slot->region.fd < 0 || offset > slot->view_len || len == 0 ||
        len > slot->view_len - offset) {
        return -1;
    }
    memset(&command, 0, sizeof(command));
    command.offset = slot->view_offset + offset;
    command.length = len;
    return ioctl(slot->region.fd,
                 OBMM_SHMDEV_SYNC_IMPORT_RANGE,
                 &command) == 0
               ? 0
               : -1;
}

static int mem_service_obmm_complete_visibility(
    const struct mem_service_obmm_mapping_slot *slot,
    const struct mem_service_mapping_range_request *request,
    struct mem_service_visibility_completion *completion_out)
{
    const uint8_t *bytes;
    uint64_t checksum;

    if (slot == NULL || request == NULL || completion_out == NULL ||
        request->offset > slot->view_len || request->len == 0 ||
        request->len > slot->view_len - request->offset) {
        return -1;
    }
    bytes = (const uint8_t *)slot->region.addr + slot->view_offset +
            request->offset;
    checksum = mem_service_provider_checksum64(bytes, request->len);
    if (checksum != request->expected_checksum) {
        return -1;
    }
    memset(completion_out, 0, sizeof(*completion_out));
    completion_out->visible_bytes = request->len;
    completion_out->checksum = checksum;
    return 0;
}

static int mem_service_obmm_gsva_event(struct mem_service_obmm_context *context,
    const struct mem_service_obmm_mapping_slot *slot, uint32_t sub_op)
{
    const struct mem_service_obmm_descriptor_v1 *d = &slot->descriptor;
    struct obmm_cmd_gsva_event_v1 event = {0};

    if (!d->strict_gsva || !slot->imported ||
        d->cache_policy != GSVA_CACHE_POLICY_WRITE_THROUGH) return -1;
    event.version = OBMM_GSVA_ABI_VERSION;
    event.sub_op = sub_op;
    event.requester_cna = context->local_cna;
    event.token_id = d->gsva_token_id;
    event.token_value = d->gsva_token_value;
    event.key.version = OBMM_GSVA_ABI_VERSION;
    event.key.segment_id = d->segment_id;
    event.key.home_va = d->remote_uba;
    event.key.size = d->size;
    event.key.p_tag = d->p_tag;
    event.key.cache_policy = d->cache_policy;
    event.key.epoch = d->epoch;
    return ioctl(context->obmm_fd, OBMM_CMD_GSVA_EVENT_V1, &event) == 0 &&
           event.error == GSVA_OK ? 0 : -1;
}

static int mem_service_obmm_provider_publish_range(
    void *opaque,
    const struct mem_service_mapping_range_request *request,
    struct mem_service_visibility_completion *completion_out)
{
    struct mem_service_obmm_context *context = opaque;
    struct mem_service_obmm_mapping_slot *slot = request != NULL
                                                        ? mem_service_obmm_find_mapping(
                                                              context,
                                                              request->mapping_handle)
                                                        : NULL;

    if (slot == NULL || context->closing || completion_out == NULL ||
        request->offset > slot->view_len || !request->len ||
        request->len > slot->view_len - request->offset) {
        fprintf(stderr,
                "[mem_service_obmm] publish failed stage=mapping-lookup\n");
        return -1;
    }
    if (!slot->map_osync &&
        mem_service_obmm_update_range(slot,
                                      request->offset,
                                      request->len,
                                      OBMM_SHM_CACHE_WB_INVAL) != 0) {
        fprintf(stderr,
                "[mem_service_obmm] publish failed stage=update-range "
                "errno=%d\n",
                errno);
        return -1;
    }
    if (slot->descriptor.strict_gsva && slot->imported &&
        mem_service_obmm_gsva_event(context, slot, OBMM_GSVA_EVENT_FENCE)) return -1;
    /*
     * OBMM shmdev mappings do not implement msync(2). Cacheable exporter
     * mappings publish through UPDATE_RANGE above; O_SYNC SIM_DEC mappings
     * are already uncached. Treating msync(EINVAL) as a visibility failure
     * rejects a successfully published OBMM range.
     */
    if (mem_service_obmm_complete_visibility(
            slot, request, completion_out) != 0) {
        fprintf(stderr,
                "[mem_service_obmm] publish failed stage=checksum\n");
        return -1;
    }
    return 0;
}

static int mem_service_obmm_provider_invalidate_range(
    void *opaque,
    const struct mem_service_mapping_range_request *request,
    struct mem_service_visibility_completion *completion_out)
{
    struct mem_service_obmm_context *context = opaque;
    struct mem_service_obmm_mapping_slot *slot = request != NULL
                                                        ? mem_service_obmm_find_mapping(
                                                              context,
                                                              request->mapping_handle)
                                                        : NULL;
    int rc;

    if (slot == NULL || context->closing || completion_out == NULL ||
        request->offset > slot->view_len || !request->len ||
        request->len > slot->view_len - request->offset) {
        return -1;
    }
    /* Home clients also need to invalidate after a remote writer publishes. */
    if (slot->descriptor.strict_gsva && slot->imported) {
        /* GSVA CPU windows are routed IO mappings, not legacy shadow
         * windows. Their IDs belong to the GSVA namespace. */
        rc = mem_service_obmm_gsva_event(context, slot, OBMM_GSVA_EVENT_READ_ACQUIRE);
        if (!rc) rc = mem_service_obmm_gsva_event(context, slot, OBMM_GSVA_EVENT_FENCE);
        if (!rc && !slot->map_osync)
            rc = mem_service_obmm_update_range(slot, request->offset, request->len,
                                               OBMM_SHM_CACHE_INVAL);
    } else {
        rc = slot->map_osync && slot->imported
             ? mem_service_obmm_sync_import_range(
                   slot, request->offset, request->len)
             : slot->map_osync ? 0 : mem_service_obmm_update_range(slot,
                                              request->offset,
                                              request->len,
                                              OBMM_SHM_CACHE_INVAL);
    }
    if (rc != 0) {
        return -1;
    }
    return mem_service_obmm_complete_visibility(slot, request, completion_out);
}

static uint64_t mem_service_obmm_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int mem_service_obmm_provider_wait_range_visible(
    void *opaque,
    const struct mem_service_mapping_range_request *request,
    struct mem_service_visibility_completion *completion_out)
{
    struct mem_service_obmm_context *context = opaque;
    struct mem_service_obmm_mapping_slot *slot;
    const uint8_t *bytes;
    uint64_t observed_checksum = 0;
    uint64_t deadline;

    if (context == NULL || context->closing || request == NULL ||
        completion_out == NULL || request->timeout_ms == 0 || request->len == 0) {
        return -1;
    }
    slot = mem_service_obmm_find_mapping(context, request->mapping_handle);
    if (slot == NULL || slot->region.addr == NULL || request->offset > slot->view_len ||
        request->len > slot->view_len - request->offset) return -1;
    deadline = mem_service_obmm_now_ms() + request->timeout_ms;
    do {
        if (mem_service_obmm_provider_invalidate_range(
                opaque, request, completion_out) == 0) {
            return 0;
        }
        usleep(1000);
    } while (mem_service_obmm_now_ms() < deadline);
    slot = mem_service_obmm_find_mapping(context, request->mapping_handle);
    if (slot != NULL && request->offset <= slot->view_len &&
        request->len <= slot->view_len - request->offset) {
        bytes = (const uint8_t *)slot->region.addr + slot->view_offset +
                request->offset;
        observed_checksum = mem_service_provider_checksum64(
            bytes, request->len);
        fprintf(stderr,
                "[mem_service_obmm] visibility timeout "
                "expected=%" PRIu64 " observed=%" PRIu64 " "
                "mem_id=%" PRIu64 " token=%u cna=%u uba=%" PRIu64 " "
                "imported=%u osync=%u\n",
                request->expected_checksum,
                observed_checksum,
                slot->descriptor.export_mem_id,
                slot->descriptor.token_id,
                slot->descriptor.export_cna,
                slot->descriptor.remote_uba,
                slot->imported ? 1U : 0U,
                slot->map_osync ? 1U : 0U);
    }
    return -1;
}

static const struct mem_service_provider_ops mem_service_obmm_provider_ops = {
    .probe = mem_service_obmm_provider_probe,
    .register_region = mem_service_obmm_provider_register_region,
    .deregister_region = mem_service_obmm_provider_deregister_region,
    .map_remote_region = mem_service_obmm_provider_map_remote_region,
    .unmap_remote_region = mem_service_obmm_provider_unmap_remote_region,
    .publish_range = mem_service_obmm_provider_publish_range,
    .invalidate_range = mem_service_obmm_provider_invalidate_range,
    .wait_range_visible = mem_service_obmm_provider_wait_range_visible,
};

int mem_service_provider_obmm_endpoint_open(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_obmm_config *config)
{
    struct mem_service_obmm_context *context;
    const char *device;
    const char *cna_path;
    size_t i;

    if (endpoint == NULL || endpoint->implementation != NULL || config == NULL ||
        config->max_remote_mappings == 0 ||
        config->max_remote_mappings > MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS ||
        config->required_peer_mappings == 0 ||
        config->required_peer_mappings > config->max_remote_mappings ||
        config->import_region_bytes == 0) {
        return -1;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return -1;
    }
    context->obmm_fd = -1;
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        context->mappings[i].region.fd = -1;
    }
    device = config->device_path != NULL
                 ? config->device_path
                 : MEM_SERVICE_OBMM_DEFAULT_DEVICE;
    cna_path = config->primary_cna_path != NULL
                   ? config->primary_cna_path
                   : MEM_SERVICE_OBMM_DEFAULT_CNA_PATH;
    context->obmm_fd = open(device, O_RDWR);
    if (context->obmm_fd < 0 ||
        !mem_service_obmm_parse_u32_file(cna_path, &context->local_cna) ||
        !obmm_alloc_import_pas((int)config->max_remote_mappings,
                               config->import_region_bytes,
                               context->import_pas,
                               context->import_osync,
                               obmm_parse_import_cache_mode())) {
        if (context->obmm_fd >= 0) {
            close(context->obmm_fd);
        }
        free(context);
        return -1;
    }
    for (i = 0; i < config->max_remote_mappings; ++i) {
        if (UINT64_MAX - context->import_pas[i] < config->import_pa_bias) {
            close(context->obmm_fd);
            free(context);
            return -1;
        }
        context->import_pas[i] += config->import_pa_bias;
    }
    context->max_remote_mappings = config->max_remote_mappings;
    context->import_region_bytes = config->import_region_bytes;
    context->required_peer_mappings = config->required_peer_mappings;
    context->force_osync = config->force_osync;
    snprintf(context->instance,
             sizeof(context->instance),
             "%s",
             config->instance != NULL ? config->instance : "obmm-0");
    endpoint->implementation = context;
    return 0;
}

int mem_service_provider_obmm_endpoint_registration(
    struct mem_service_provider_obmm_endpoint *endpoint,
    struct mem_service_provider_registration *registration_out)
{
    struct mem_service_obmm_context *context;

    if (endpoint == NULL || endpoint->implementation == NULL ||
        registration_out == NULL) {
        return -1;
    }
    context = endpoint->implementation;
    memset(registration_out, 0, sizeof(*registration_out));
    registration_out->name = "obmm";
    registration_out->instance = context->instance;
    registration_out->capabilities =
        MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
        MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING;
    registration_out->ops = &mem_service_obmm_provider_ops;
    registration_out->context = context;
    return 0;
}

int mem_service_provider_obmm_endpoint_create_region(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint64_t len,
    struct mem_service_region *region_out)
{
    struct mem_service_region_request request;

    if (endpoint == NULL || endpoint->implementation == NULL || len == 0 ||
        region_out == NULL) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.len = len;
    request.memory_kind = MEM_SERVICE_MEMORY_HOST;
    request.flags = MEM_SERVICE_REGION_FLAG_PROVIDER_ALLOCATED;
    return mem_service_obmm_provider_register_region(
        endpoint->implementation, &request, region_out);
}

static int mem_service_obmm_remote_region_from_region(
    const struct mem_service_region *region,
    struct mem_service_provider_remote_region *remote_out)
{
    if (region == NULL || remote_out == NULL || region->handle == 0 ||
        region->len == 0 || region->memory_kind != MEM_SERVICE_MEMORY_HOST ||
        region->descriptor.len == 0 ||
        region->descriptor.len > MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN) {
        return -1;
    }
    memset(remote_out, 0, sizeof(*remote_out));
    snprintf(remote_out->provider_name,
             sizeof(remote_out->provider_name),
             "%s",
             "obmm");
    remote_out->len = region->len;
    remote_out->memory_kind = region->memory_kind;
    remote_out->descriptor = region->descriptor;
    return 0;
}

int mem_service_provider_obmm_endpoint_prepare_canary_region(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint64_t region_len,
    uint64_t visible_len,
    uint8_t seed,
    struct mem_service_region *region_out,
    struct mem_service_provider_remote_region *remote_out,
    uint64_t *checksum_out)
{
    struct mem_service_obmm_context *context;
    struct mem_service_region region;
    struct mem_service_mapping_request mapping_request;
    struct mem_service_mapping mapping;
    struct mem_service_mapping_range_request range_request;
    struct mem_service_visibility_completion completion;
    uint8_t *bytes;
    uint64_t checksum;
    uint64_t i;
    const char *failure_stage = "register";
    int rc = -1;

    if (endpoint == NULL || endpoint->implementation == NULL ||
        region_len == 0 || visible_len == 0 || visible_len > region_len ||
        region_out == NULL || remote_out == NULL || checksum_out == NULL) {
        return -1;
    }
    context = endpoint->implementation;
    memset(&region, 0, sizeof(region));
    memset(&mapping, 0, sizeof(mapping));
    if (mem_service_provider_obmm_endpoint_create_region(
            endpoint, region_len, &region) != 0) {
        return -1;
    }
    memset(&mapping_request, 0, sizeof(mapping_request));
    mapping_request.remote_descriptor = region.descriptor;
    mapping_request.remote_region_len = region.len;
    mapping_request.len = visible_len;
    mapping_request.memory_kind = region.memory_kind;
    mapping_request.flags = MEM_SERVICE_MAPPING_FLAG_READ |
                            MEM_SERVICE_MAPPING_FLAG_WRITE;
    failure_stage = "map-local";
    if (mem_service_obmm_provider_map_remote_region(
            context, &mapping_request, &mapping) != 0) {
        goto done;
    }
    bytes = mapping.base;
    for (i = 0; i < visible_len; ++i) {
        bytes[i] = (uint8_t)(seed + i * 29U);
    }
    checksum = mem_service_provider_checksum64(bytes, visible_len);
    failure_stage = "checksum";
    if (checksum == 0) {
        goto done;
    }
    memset(&range_request, 0, sizeof(range_request));
    range_request.mapping_handle = mapping.handle;
    range_request.len = visible_len;
    range_request.expected_checksum = checksum;
    failure_stage = "publish";
    if (mem_service_obmm_provider_publish_range(
            context, &range_request, &completion) != 0 ||
        completion.visible_bytes != visible_len ||
        completion.checksum != checksum) {
        goto done;
    }
    failure_stage = "export-descriptor";
    if (mem_service_obmm_remote_region_from_region(
            &region, remote_out) != 0) {
        goto done;
    }
    *region_out = region;
    *checksum_out = checksum;
    rc = 0;

done:
    failure_stage = rc == 0 ? "unmap-local" : failure_stage;
    if (mapping.handle != 0 &&
        mem_service_obmm_provider_unmap_remote_region(
            context, mapping.handle) != 0) {
        rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr,
                "[mem_service_obmm] canary prepare failed stage=%s "
                "errno=%d\n",
                failure_stage,
                errno);
        if (region.handle != 0) {
            (void)mem_service_obmm_provider_deregister_region(
                context, region.handle);
        }
        memset(region_out, 0, sizeof(*region_out));
        memset(remote_out, 0, sizeof(*remote_out));
        *checksum_out = 0;
    }
    return rc;
}

int mem_service_provider_obmm_endpoint_exchange_remote_regions(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint32_t local_node,
    uint32_t node_count,
    uint64_t generation,
    const struct mem_service_provider_remote_region *local,
    struct mem_service_provider_remote_region *regions_out,
    size_t region_capacity)
{
    struct mem_service_obmm_context *context;
    struct mem_service_obmm_descriptor_v1 local_descriptor;
    struct obmm_helpers_meta publish_meta;
    struct obmm_helpers_meta peer_metas[OBMM_POOL_HELPERS_MAX_NODES];
    bool got[OBMM_POOL_HELPERS_MAX_NODES];
    uint32_t i;

    if (endpoint == NULL || endpoint->implementation == NULL || local == NULL ||
        regions_out == NULL || node_count < 2 ||
        node_count > OBMM_POOL_HELPERS_MAX_NODES || local_node >= node_count ||
        region_capacity < node_count || generation == 0 ||
        strcmp(local->provider_name, "obmm") != 0 ||
        local->memory_kind != MEM_SERVICE_MEMORY_HOST || local->len == 0 ||
        mem_service_obmm_descriptor_decode(
            &local->descriptor, &local_descriptor) != 0 ||
        local_descriptor.size != local->len || local_descriptor.strict_gsva) {
        return -1;
    }
    context = endpoint->implementation;
    memset(&publish_meta, 0, sizeof(publish_meta));
    publish_meta.export_mem_id = local_descriptor.export_mem_id;
    publish_meta.remote_uba = local_descriptor.remote_uba;
    publish_meta.size = local_descriptor.size;
    publish_meta.token_id = local_descriptor.token_id;
    publish_meta.export_cna = local_descriptor.export_cna;
    memset(peer_metas, 0, sizeof(peer_metas));
    memset(got, 0, sizeof(got));
    if (obmm_bootstrap_publish(context->obmm_fd,
                               (int)local_node,
                               (int)node_count,
                               generation,
                               &publish_meta) != 0 ||
        obmm_bootstrap_lookup(context->obmm_fd,
                              context->local_cna,
                              (int)node_count,
                              generation,
                              peer_metas,
                              got) != 0) {
        return -1;
    }
    memset(regions_out, 0, node_count * sizeof(*regions_out));
    for (i = 0; i < node_count; ++i) {
        struct mem_service_obmm_descriptor_v1 descriptor = {0};

        if (!got[i]) {
            return -1;
        }
        descriptor.export_mem_id = peer_metas[i].export_mem_id;
        descriptor.remote_uba = peer_metas[i].remote_uba;
        descriptor.size = peer_metas[i].size;
        descriptor.token_id = peer_metas[i].token_id;
        descriptor.export_cna = peer_metas[i].export_cna;
        snprintf(regions_out[i].provider_name,
                 sizeof(regions_out[i].provider_name),
                 "%s",
                 "obmm");
        regions_out[i].len = descriptor.size;
        regions_out[i].memory_kind = MEM_SERVICE_MEMORY_HOST;
        if (mem_service_obmm_descriptor_encode(
                &descriptor, &regions_out[i].descriptor) != 0) {
            memset(regions_out, 0, node_count * sizeof(*regions_out));
            return -1;
        }
    }
    return 0;
}

int mem_service_provider_obmm_endpoint_verify_mapping(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_remote_region *remote,
    uint64_t offset,
    uint64_t len,
    uint64_t expected_checksum,
    uint64_t timeout_ms)
{
    struct mem_service_obmm_context *context;
    struct mem_service_mapping_request mapping_request;
    struct mem_service_mapping mapping;
    struct mem_service_mapping_range_request range_request;
    struct mem_service_visibility_completion completion;
    struct mem_service_obmm_descriptor_v1 descriptor;
    uint32_t i;
    int rc;

    if (endpoint == NULL || endpoint->implementation == NULL || remote == NULL ||
        strcmp(remote->provider_name, "obmm") != 0 || remote->len == 0 ||
        remote->memory_kind != MEM_SERVICE_MEMORY_HOST ||
        len == 0 || offset > remote->len || len > remote->len - offset ||
        expected_checksum == 0 || timeout_ms == 0 ||
        mem_service_obmm_descriptor_decode(&remote->descriptor, &descriptor) !=
            0) {
        return -1;
    }
    context = endpoint->implementation;
    memset(&mapping_request, 0, sizeof(mapping_request));
    mapping_request.remote_descriptor = remote->descriptor;
    mapping_request.remote_region_len = remote->len;
    mapping_request.offset = offset;
    mapping_request.len = len;
    mapping_request.memory_kind = remote->memory_kind;
    mapping_request.flags = MEM_SERVICE_MAPPING_FLAG_READ;
    memset(&mapping, 0, sizeof(mapping));
    if (mem_service_obmm_provider_map_remote_region(
            context, &mapping_request, &mapping) != 0) {
        return -1;
    }
    memset(&range_request, 0, sizeof(range_request));
    range_request.mapping_handle = mapping.handle;
    range_request.len = len;
    range_request.expected_checksum = expected_checksum;
    range_request.timeout_ms = timeout_ms;
    rc = mem_service_obmm_provider_wait_range_visible(
        context, &range_request, &completion);
    if (mem_service_obmm_provider_unmap_remote_region(
            context, mapping.handle) != 0) {
        rc = -1;
    }
    if (rc == 0) {
        for (i = 0; i < context->verified_peer_count; ++i) {
            if (mem_service_obmm_descriptor_equal(
                    &context->verified_peer_descriptors[i], &descriptor)) {
                break;
            }
        }
        if (i == context->verified_peer_count &&
            context->verified_peer_count <
                MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS) {
            context->verified_peer_descriptors[
                context->verified_peer_count] = descriptor;
            context->verified_peer_count += 1U;
        }
        context->mapping_verified =
            context->verified_peer_count >= context->required_peer_mappings;
    }
    return rc;
}

int mem_service_provider_obmm_endpoint_close_checked(
    struct mem_service_provider_obmm_endpoint *endpoint)
{
    struct mem_service_obmm_context *context;
    size_t i;
    int rc = 0;

    if (endpoint == NULL || endpoint->implementation == NULL) {
        return 0;
    }
    context = endpoint->implementation;
    context->closing = true;
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->mappings[i].active) {
            if (mem_service_obmm_provider_unmap_remote_region(
                    context, context->mappings[i].handle) != 0) rc = -1;
        }
    }
    if (rc != 0) return rc;
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->regions[i].active) {
            if (mem_service_obmm_provider_deregister_region(
                    context, context->regions[i].handle) != 0) rc = -1;
        }
    }
    if (rc != 0 || context->close_uncertain) return -1;
    if (context->obmm_fd >= 0) {
        int fd = context->obmm_fd;
        context->obmm_fd = -1;
        if (close(fd) != 0) {
            context->close_uncertain = true;
            return -1;
        }
    }
    free(context);
    endpoint->implementation = NULL;
    return 0;
}
#else
int mem_service_provider_obmm_endpoint_open(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_obmm_config *config)
{
    (void)endpoint;
    (void)config;
    return -1;
}

int mem_service_provider_obmm_endpoint_registration(
    struct mem_service_provider_obmm_endpoint *endpoint,
    struct mem_service_provider_registration *registration_out)
{
    (void)endpoint;
    (void)registration_out;
    return -1;
}

int mem_service_provider_obmm_endpoint_create_region(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint64_t len,
    struct mem_service_region *region_out)
{
    (void)endpoint;
    (void)len;
    (void)region_out;
    return -1;
}

int mem_service_provider_obmm_endpoint_prepare_canary_region(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint64_t region_len,
    uint64_t visible_len,
    uint8_t seed,
    struct mem_service_region *region_out,
    struct mem_service_provider_remote_region *remote_out,
    uint64_t *checksum_out)
{
    (void)endpoint;
    (void)region_len;
    (void)visible_len;
    (void)seed;
    (void)region_out;
    (void)remote_out;
    (void)checksum_out;
    return -1;
}

int mem_service_provider_obmm_endpoint_exchange_remote_regions(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint32_t local_node,
    uint32_t node_count,
    uint64_t generation,
    const struct mem_service_provider_remote_region *local,
    struct mem_service_provider_remote_region *regions_out,
    size_t region_capacity)
{
    (void)endpoint;
    (void)local_node;
    (void)node_count;
    (void)generation;
    (void)local;
    (void)regions_out;
    (void)region_capacity;
    return -1;
}

int mem_service_provider_obmm_endpoint_verify_mapping(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_remote_region *remote,
    uint64_t offset,
    uint64_t len,
    uint64_t expected_checksum,
    uint64_t timeout_ms)
{
    (void)endpoint;
    (void)remote;
    (void)offset;
    (void)len;
    (void)expected_checksum;
    (void)timeout_ms;
    return -1;
}

int mem_service_provider_obmm_endpoint_close_checked(
    struct mem_service_provider_obmm_endpoint *endpoint)
{
    return endpoint == NULL || endpoint->implementation == NULL ? 0 : -1;
}
#endif

int mem_service_provider_obmm_mapping_pin_acquire(
    const struct mem_service_provider_mapping_binding *binding,
    uint64_t access_flags,
    struct mem_service_provider_obmm_mapping_pin **pin_out,
    struct mem_service_provider_obmm_pinned_mapping *view_out)
{
    if (!pin_out || *pin_out || !view_out) return -EINVAL;
    memset(view_out, 0, sizeof(*view_out));
    view_out->obmm_fd = -1;
#ifdef __linux__
    const uint64_t rights = MEM_SERVICE_MAPPING_FLAG_READ | MEM_SERVICE_MAPPING_FLAG_WRITE;
    struct mem_service_obmm_context *context;
    struct mem_service_obmm_mapping_slot *slot;
    struct mem_service_provider_obmm_mapping_pin *pin;

    if (!binding || !binding->mapped || !binding->owner || !access_flags ||
        (access_flags & ~rights)) return -EINVAL;
    if (binding->owner->ops != &mem_service_obmm_provider_ops) return -EOPNOTSUPP;
    context = binding->owner->context;
    if (!context || context->obmm_fd < 0 || context->closing) return -EBUSY;
    slot = mem_service_obmm_find_mapping(context, binding->mapping.handle);
    if (!slot || slot->close_uncertain || slot->region.fd < 0 || !slot->region.mem_id ||
        !slot->region.addr || !slot->view_len) return -ESTALE;
    if (!slot->descriptor.strict_gsva || !slot->imported) return -EOPNOTSUPP;
    if (!mem_service_obmm_gsva_valid(&slot->descriptor) ||
        (uintptr_t)slot->region.addr != slot->descriptor.remote_uba ||
        slot->view_offset > slot->descriptor.size ||
        slot->view_len > slot->descriptor.size - slot->view_offset ||
        binding->mapping.memory_kind != MEM_SERVICE_MEMORY_HOST ||
        binding->mapping.base != (uint8_t *)slot->region.addr + slot->view_offset ||
        binding->mapping.len != slot->view_len) return -ESTALE;
    if (access_flags & ~slot->view_access) return -EACCES;
    if (slot->compute_pins == UINT64_MAX) return -EOVERFLOW;
    pin = calloc(1, sizeof(*pin));
    if (!pin) return -ENOMEM;
    pin->context = context;
    pin->mapping_handle = slot->handle;
    ++slot->compute_pins;
    *view_out = (struct mem_service_provider_obmm_pinned_mapping) {
        .obmm_fd = context->obmm_fd, .mem_id = slot->region.mem_id,
        .base = binding->mapping.base, .len = slot->view_len,
        .access_flags = access_flags,
    };
    *pin_out = pin;
    return 0;
#else
    (void)binding;
    (void)access_flags;
    return -EOPNOTSUPP;
#endif
}

int mem_service_provider_obmm_mapping_pin_release(
    struct mem_service_provider_obmm_mapping_pin **pin_inout)
{
    if (!pin_inout) return -EINVAL;
    if (!*pin_inout) return 0;
#ifdef __linux__
    struct mem_service_provider_obmm_mapping_pin *pin = *pin_inout;
    struct mem_service_obmm_mapping_slot *slot =
        mem_service_obmm_find_mapping(pin->context, pin->mapping_handle);
    if (!slot || !slot->compute_pins) return -ESTALE;
    --slot->compute_pins;
    free(pin);
    *pin_inout = NULL;
    return 0;
#else
    return -EOPNOTSUPP;
#endif
}

int mem_service_provider_obmm_endpoint_probe_conflict(
    struct mem_service_provider_obmm_endpoint *endpoint, uint64_t mapping_handle)
{
#ifdef __linux__
    struct mem_service_obmm_context *context;
    struct mem_service_obmm_mapping_slot *slot;
    int rc, map_errno;

    if (!endpoint || !(context = endpoint->implementation) || context->closing)
        return -1;
    slot = mem_service_obmm_find_mapping(context, mapping_handle);
    if (!slot || !slot->descriptor.strict_gsva || slot->close_uncertain ||
        slot->region.fd < 0 || !slot->region.addr || !slot->view_len)
        return -1;
    for (unsigned i = 0; i < 3; ++i)
        if (slot->probe_view.parts[i].owned) return -1;
    errno = 0;
    rc = mem_service_obmm_map_view(slot->region.addr, slot->descriptor.size,
        slot->view_offset, slot->view_len, PROT_READ,
        MAP_SHARED | MAP_FIXED_NOREPLACE | MAP_GSVA, slot->region.fd,
        &slot->probe_view);
    map_errno = errno;
    if (mem_service_obmm_unmap_view(&slot->probe_view) != 0) {
        context->closing = true;
        return -1;
    }
    return rc == -1 && map_errno == EEXIST ? 0 : -1;
#else
    (void)endpoint;
    (void)mapping_handle;
    return -1;
#endif
}

int mem_service_provider_obmm_endpoint_probe_descriptor(
    struct mem_service_provider_obmm_endpoint *endpoint, uint64_t mapping_handle)
{
#ifdef __linux__
    static const struct {
        const char *name;
        unsigned offset, width;
        uint64_t value;
    } mutations[] = {
        {"magic", 0, 4, 0}, {"version", 4, 4, 0}, {"length", 8, 4, 0},
        {"reserved", 12, 4, 1}, {"export_id", 16, 8, 0},
        {"uba", 24, 8, 0}, {"size", 32, 8, 0}, {"export_token", 40, 4, 0},
        {"home_cna", 44, 4, 0}, {"segment", 48, 8, 0}, {"epoch", 56, 8, 0},
        {"segment_flags", 64, 4, 0}, {"owner", 68, 4, UINT32_MAX},
        {"nodes", 72, 4, 0}, {"cache", 76, 4, UINT32_MAX},
        {"access_empty", 84, 4, 0}, {"access_unknown", 84, 4, 4},
        {"segment_token_id", 88, 4, 0}, {"segment_token_value", 92, 4, 0},
    };
    const unsigned field_count = sizeof(mutations) / sizeof(mutations[0]);
    const char *const extra_names[] = {
        "short_descriptor", "long_descriptor", "unaligned_uba",
        "unaligned_size", "range_overflow",
    };
    struct mem_service_obmm_context *context;
    struct mem_service_obmm_mapping_slot *slot;
    struct mem_service_mapping_request original = {0};
    struct mem_service_provider_obmm_resources_v1 before, after;
    bool available = false;
    uint64_t next_handle;

    if (!endpoint || !(context = endpoint->implementation) || context->closing ||
        context->next_mapping_handle == UINT64_MAX) return -1;
    slot = mem_service_obmm_find_mapping(context, mapping_handle);
    if (!slot || !slot->descriptor.strict_gsva || slot->close_uncertain ||
        slot->region.fd < 0 || !slot->region.addr || !slot->view_len ||
        !(slot->descriptor.access_flags & OBMM_GSVA_ACCESS_READ)) return -1;
    for (size_t i = 0; i < context->max_remote_mappings; ++i)
        if (!context->mappings[i].active) available = true;
    if (!available || slot->descriptor.size > context->import_region_bytes ||
        slot->view_offset > slot->descriptor.size ||
        slot->view_len > slot->descriptor.size - slot->view_offset ||
        mem_service_obmm_descriptor_encode(&slot->descriptor, &original.remote_descriptor) ||
        mem_service_provider_obmm_endpoint_resources_v1(endpoint, &before)) return -1;
    original.remote_region_len = slot->descriptor.size;
    original.offset = slot->view_offset;
    original.len = slot->view_len;
    original.memory_kind = MEM_SERVICE_MEMORY_HOST;
    original.flags = MEM_SERVICE_MAPPING_FLAG_READ;
    next_handle = context->next_mapping_handle;
    for (unsigned i = 0; i < field_count + 5; ++i) {
        struct mem_service_mapping_request request = original;
        struct mem_service_mapping mapping = {0};
        const char *name;
        int rc;
        if (i < field_count) {
            name = mutations[i].name;
            uint8_t *field = request.remote_descriptor.bytes + mutations[i].offset;
            if (mutations[i].width == 8)
                mem_service_obmm_put_u64(field, mutations[i].value);
            else
                mem_service_obmm_put_u32(field, (uint32_t)mutations[i].value);
        } else {
            unsigned extra = i - field_count;
            name = extra_names[extra];
            if (extra < 2) request.remote_descriptor.len =
                MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES + (extra ? 1 : -1);
            else if (extra == 2)
                mem_service_obmm_put_u64(request.remote_descriptor.bytes + 24,
                                        slot->descriptor.remote_uba | 1U);
            else if (extra == 3)
                mem_service_obmm_put_u64(request.remote_descriptor.bytes + 32,
                                        slot->descriptor.size | 1U);
            else
                mem_service_obmm_put_u64(request.remote_descriptor.bytes + 24,
                                        UINT64_MAX & ~UINT64_C(4095));
        }
        /* This is the production provider callback, without SDK prevalidation. */
        rc = mem_service_obmm_provider_map_remote_region(context, &request, &mapping);
        if (rc != -1 || mapping.handle || context->next_mapping_handle != next_handle ||
            mem_service_provider_obmm_endpoint_resources_v1(endpoint, &after) ||
            before.export_handles != after.export_handles || before.export_bytes != after.export_bytes ||
            before.import_handles != after.import_handles || before.import_bytes != after.import_bytes ||
            before.vma_count != after.vma_count || before.vma_bytes != after.vma_bytes ||
            before.accessible_views != after.accessible_views ||
            before.accessible_bytes != after.accessible_bytes ||
            before.cleanup_mappings != after.cleanup_mappings || before.closing != after.closing ||
            before.control_close_uncertain != after.control_close_uncertain) {
            context->closing = true;
            fprintf(stderr, "obmm-descriptor-probe: result=fail field=%s rc=%d\n", name, rc);
            return -1;
        }
        fprintf(stderr, "obmm-descriptor-probe: field=%s rejected=1 preserved=1\n", name);
    }
    fprintf(stderr, "obmm-descriptor-probe: result=pass checks=%u source=retained_handle\n",
            field_count + 5);
    return 0;
#else
    (void)endpoint;
    (void)mapping_handle;
    return -1;
#endif
}

int mem_service_provider_obmm_endpoint_resources_v1(
    const struct mem_service_provider_obmm_endpoint *endpoint,
    struct mem_service_provider_obmm_resources_v1 *resources_out)
{
    if (resources_out == NULL) return -1;
    memset(resources_out, 0, sizeof(*resources_out));
#ifdef __linux__
    const struct mem_service_obmm_context *context;
    struct mem_service_provider_obmm_resources_v1 result = {0};

    if (endpoint == NULL || endpoint->implementation == NULL) return -1;
    context = endpoint->implementation;
    result.closing = context->closing;
    result.control_close_uncertain = context->close_uncertain;
    for (size_t i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        const struct mem_service_obmm_region_slot *region = &context->regions[i];
        const struct mem_service_obmm_mapping_slot *mapping = &context->mappings[i];
        if (region->active) {
            if (region->descriptor.size > UINT64_MAX - result.export_bytes) return -1;
            ++result.export_handles;
            result.export_bytes += region->descriptor.size;
        }
        if (!mapping->active) continue;
        if (mapping->imported) {
            if (mapping->descriptor.size > UINT64_MAX - result.import_bytes) return -1;
            ++result.import_handles;
            result.import_bytes += mapping->descriptor.size;
        }
        const struct mem_service_obmm_view *views[] = {
            &mapping->view, &mapping->probe_view,
        };
        for (size_t view = 0; view < 2; ++view) {
            for (size_t part = 0; part < 3; ++part) {
                if (!views[view]->parts[part].owned) continue;
                if (views[view]->parts[part].len > UINT64_MAX - result.vma_bytes) return -1;
                ++result.vma_count;
                result.vma_bytes += views[view]->parts[part].len;
            }
        }
        bool accessible = !mapping->close_uncertain &&
            mapping->view_len != 0 && mapping->region.addr != NULL;
        if (context->closing || !accessible)
            ++result.cleanup_mappings;
        if (accessible) {
            if (mapping->view_len > UINT64_MAX - result.accessible_bytes) return -1;
            ++result.accessible_views;
            result.accessible_bytes += mapping->view_len;
        }
    }
    *resources_out = result;
    return 0;
#else
    (void)endpoint;
    return -1;
#endif
}

void mem_service_provider_obmm_endpoint_close(
    struct mem_service_provider_obmm_endpoint *endpoint)
{
    if (mem_service_provider_obmm_endpoint_close_checked(endpoint) != 0)
        fprintf(stderr, "[mem_service_obmm] endpoint cleanup_pending\n");
}

int mem_service_provider_obmm_run_protocol_fixture(void)
{
    struct mem_service_obmm_descriptor_v1 source = {
        .export_mem_id = 0x1020304050607080ULL,
        .remote_uba = 0x1122334455667788ULL,
        .size = 0x200000ULL,
        .token_id = 0x1234U,
        .export_cna = 0x45U,
    };
    struct mem_service_obmm_descriptor_v1 colliding_peer = source;
    struct mem_service_obmm_descriptor_v1 decoded;
    struct mem_service_provider_descriptor opaque;

    if (mem_service_obmm_descriptor_encode(&source, &opaque) != 0 ||
        mem_service_obmm_descriptor_decode(&opaque, &decoded) != 0 ||
        !mem_service_obmm_descriptor_equal(&source, &decoded)) {
        return 1;
    }
    colliding_peer.export_cna++;
    if (mem_service_obmm_descriptor_equal(&source, &colliding_peer)) {
        return 1;
    }
    opaque.bytes[4] = 0xffU;
    if (mem_service_obmm_descriptor_decode(&opaque, &decoded) == 0) {
        return 1;
    }
    opaque.bytes[4] = 0;
    opaque.bytes[12] = 1U;
    if (mem_service_obmm_descriptor_decode(&opaque, &decoded) == 0) {
        return 1;
    }
    source.strict_gsva = true;
    source.remote_uba = 0x700000000000ULL;
    source.segment_id = 73;
    source.epoch = 19;
    source.segment_flags = 7;
    source.owner_node = 1;
    source.node_count = 3;
    source.cache_policy = 4;
    source.access_flags = 3;
    source.gsva_token_id = 11;
    source.gsva_token_value = 53;
    if (mem_service_obmm_descriptor_encode(&source, &opaque) != 0 ||
        opaque.len != MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES ||
        mem_service_obmm_descriptor_decode(&opaque, &decoded) != 0 ||
        !mem_service_obmm_descriptor_equal(&source, &decoded) ||
        decoded.token_id == decoded.gsva_token_id) return 1;
    colliding_peer = source;
    colliding_peer.epoch++;
    if (mem_service_obmm_descriptor_equal(&source, &colliding_peer)) return 1;
    colliding_peer = source;
    colliding_peer.gsva_token_value++;
    if (mem_service_obmm_descriptor_equal(&source, &colliding_peer)) return 1;
#ifdef __linux__
    {
        struct obmm_gsva_segment_desc_v1 segment = {
            .version = OBMM_GSVA_ABI_VERSION, .flags = source.segment_flags,
            .segment_id = source.segment_id, .home_va = source.remote_uba,
            .size = source.size, .epoch = source.epoch, .home_cna = source.export_cna,
            .owner_node_id = source.owner_node, .node_count = source.node_count,
            .cache_policy = source.cache_policy, .p_tag = source.p_tag,
            .access_flags = source.access_flags, .token_id = source.gsva_token_id,
            .token_value = source.gsva_token_value,
        };
        struct obmm_cmd_export exported = {0};
        struct mem_service_provider_descriptor native;
        exported.length = 1;
        exported.size[0] = source.size;
        exported.flags = OBMM_EXPORT_FLAG_GSVA_FIXED_UBA | OBMM_EXPORT_FLAG_ALLOW_MMAP;
        exported.uba = source.remote_uba;
        exported.mem_id = source.export_mem_id;
        exported.tokenid = source.token_id;
        if (mem_service_provider_obmm_encode_gsva(&segment, &exported, &native) != 0 ||
            native.len != opaque.len || memcmp(native.bytes, opaque.bytes, native.len)) return 1;
        exported.uba++;
        if (mem_service_provider_obmm_encode_gsva(&segment, &exported, &native) == 0) return 1;
    }
#endif
    opaque.len = MEM_SERVICE_OBMM_DESCRIPTOR_BYTES;
    if (mem_service_obmm_descriptor_decode(&opaque, &decoded) == 0) return 1;
    opaque.len = MEM_SERVICE_OBMM_GSVA_DESCRIPTOR_BYTES;
    mem_service_obmm_put_u32(opaque.bytes + 92, 0);
    if (mem_service_obmm_descriptor_decode(&opaque, &decoded) == 0) return 1;
    source.remote_uba = UINT64_MAX - 4095;
    if (mem_service_obmm_descriptor_encode(&source, &opaque) == 0) return 1;
    source.remote_uba = 0x700000000000ULL;
    source.owner_node = source.node_count;
    if (mem_service_obmm_descriptor_encode(&source, &opaque) == 0) return 1;
    source.owner_node = 1;
    source.segment_flags |= 8;
    if (mem_service_obmm_descriptor_encode(&source, &opaque) == 0) return 1;
    printf("mem_service obmm-provider-fixtures: status=ok "
           "descriptor_version=1 corruption=fail-closed "
           "gsva_descriptor_version=2 gsva_identity=checked "
           "node_local_id_collision=fail-closed "
           "mapping_path=sim-dec urma_dependency=none\n");
    return 0;
}
