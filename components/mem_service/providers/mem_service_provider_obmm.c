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
#define MEM_SERVICE_OBMM_DEFAULT_DEVICE "/dev/obmm"
#define MEM_SERVICE_OBMM_DEFAULT_CNA_PATH \
    "/sys/bus/ub/devices/00001/primary_cna"

struct mem_service_obmm_descriptor_v1 {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

#ifdef __linux__
struct mem_service_obmm_region_slot {
    bool active;
    uint64_t handle;
    struct mem_service_obmm_descriptor_v1 descriptor;
};

struct mem_service_obmm_mapping_slot {
    bool active;
    bool imported;
    bool map_osync;
    uint64_t handle;
    uint64_t view_offset;
    uint64_t view_len;
    struct mem_service_obmm_descriptor_v1 descriptor;
    struct obmm_helpers_region region;
};

struct mem_service_obmm_context {
    int obmm_fd;
    uint32_t local_cna;
    bool mapping_verified;
    bool force_osync;
    uint32_t max_remote_mappings;
    uint32_t required_peer_mappings;
    uint32_t verified_peer_count;
    uint64_t next_region_handle;
    uint64_t next_mapping_handle;
    char instance[MEM_SERVICE_PROVIDER_INSTANCE_LEN];
    uint64_t import_pas[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    bool import_osync[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    uint64_t verified_peer_mem_ids[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    struct mem_service_obmm_region_slot
        regions[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    struct mem_service_obmm_mapping_slot
        mappings[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
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

static int mem_service_obmm_descriptor_encode(
    const struct mem_service_obmm_descriptor_v1 *descriptor,
    struct mem_service_provider_descriptor *opaque_out)
{
    uint8_t *bytes;

    if (descriptor == NULL || opaque_out == NULL ||
        descriptor->export_mem_id == 0 || descriptor->size == 0 ||
        descriptor->token_id == 0 || descriptor->export_cna == 0) {
        return -1;
    }
    memset(opaque_out, 0, sizeof(*opaque_out));
    opaque_out->len = MEM_SERVICE_OBMM_DESCRIPTOR_BYTES;
    bytes = opaque_out->bytes;
    mem_service_obmm_put_u32(bytes, MEM_SERVICE_OBMM_DESCRIPTOR_MAGIC);
    mem_service_obmm_put_u32(bytes + 4, MEM_SERVICE_OBMM_DESCRIPTOR_VERSION);
    mem_service_obmm_put_u32(bytes + 8, MEM_SERVICE_OBMM_DESCRIPTOR_BYTES);
    mem_service_obmm_put_u32(bytes + 12, 0);
    mem_service_obmm_put_u64(bytes + 16, descriptor->export_mem_id);
    mem_service_obmm_put_u64(bytes + 24, descriptor->remote_uba);
    mem_service_obmm_put_u64(bytes + 32, descriptor->size);
    mem_service_obmm_put_u32(bytes + 40, descriptor->token_id);
    mem_service_obmm_put_u32(bytes + 44, descriptor->export_cna);
    return 0;
}

static int mem_service_obmm_descriptor_decode(
    const struct mem_service_provider_descriptor *opaque,
    struct mem_service_obmm_descriptor_v1 *descriptor_out)
{
    struct mem_service_obmm_descriptor_v1 descriptor;
    const uint8_t *bytes;

    if (opaque == NULL || descriptor_out == NULL ||
        opaque->len != MEM_SERVICE_OBMM_DESCRIPTOR_BYTES) {
        return -1;
    }
    bytes = opaque->bytes;
    if (mem_service_obmm_get_u32(bytes) !=
            MEM_SERVICE_OBMM_DESCRIPTOR_MAGIC ||
        mem_service_obmm_get_u32(bytes + 4) !=
            MEM_SERVICE_OBMM_DESCRIPTOR_VERSION ||
        mem_service_obmm_get_u32(bytes + 8) !=
            MEM_SERVICE_OBMM_DESCRIPTOR_BYTES ||
        mem_service_obmm_get_u32(bytes + 12) != 0) {
        return -1;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.export_mem_id = mem_service_obmm_get_u64(bytes + 16);
    descriptor.remote_uba = mem_service_obmm_get_u64(bytes + 24);
    descriptor.size = mem_service_obmm_get_u64(bytes + 32);
    descriptor.token_id = mem_service_obmm_get_u32(bytes + 40);
    descriptor.export_cna = mem_service_obmm_get_u32(bytes + 44);
    if (descriptor.export_mem_id == 0 || descriptor.size == 0 ||
        descriptor.token_id == 0 || descriptor.export_cna == 0) {
        return -1;
    }
    *descriptor_out = descriptor;
    return 0;
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
    if (context->obmm_fd < 0 || fcntl(context->obmm_fd, F_GETFD) < 0) {
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

    if (context == NULL || request == NULL || region_out == NULL ||
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
        (void)obmm_do_unexport(context->obmm_fd,
                               slot->descriptor.export_mem_id);
        memset(slot, 0, sizeof(*slot));
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
            context->mappings[i].descriptor.export_mem_id ==
                slot->descriptor.export_mem_id) {
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
            context->regions[i].descriptor.export_mem_id ==
                descriptor->export_mem_id &&
            context->regions[i].descriptor.token_id == descriptor->token_id) {
            return true;
        }
    }
    return false;
}

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

    if (context == NULL || request == NULL || mapping_out == NULL ||
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
    local = mem_service_obmm_descriptor_is_local(context, &descriptor);
    map_osync = !local &&
                (context->force_osync || context->import_osync[slot_index]);
    if (local) {
        import_mem_id = descriptor.export_mem_id;
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
    slot->region.fd = -1;
    if (obmm_map_region_at(import_mem_id,
                           request->requested_address,
                           descriptor.size,
                           map_osync,
                           &slot->region) != 0) {
        if (!local) {
            (void)obmm_do_unimport(context->obmm_fd, import_mem_id);
        }
        return -1;
    }
    slot->active = true;
    slot->imported = !local;
    slot->map_osync = map_osync;
    slot->handle = ++context->next_mapping_handle;
    slot->view_offset = request->offset;
    slot->view_len = request->len;
    slot->descriptor = descriptor;
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
    uint64_t mem_id;
    bool imported;

    if (slot == NULL) {
        return -1;
    }
    mem_id = slot->region.mem_id;
    imported = slot->imported;
    obmm_unmap_region(&slot->region);
    if (imported && obmm_do_unimport(context->obmm_fd, mem_id) != 0) {
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

    if (slot == NULL) {
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
    if (msync(slot->region.addr, slot->region.len, MS_SYNC) != 0) {
        fprintf(stderr,
                "[mem_service_obmm] publish failed stage=msync errno=%d\n",
                errno);
        return -1;
    }
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

    if (slot == NULL || !slot->imported) {
        return -1;
    }
    rc = slot->map_osync
             ? mem_service_obmm_sync_import_range(
                   slot, request->offset, request->len)
             : mem_service_obmm_update_range(slot,
                                              request->offset,
                                              request->len,
                                              OBMM_SHM_CACHE_INVAL);
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
    uint64_t deadline;

    if (request == NULL || request->timeout_ms == 0) {
        return -1;
    }
    deadline = mem_service_obmm_now_ms() + request->timeout_ms;
    do {
        if (mem_service_obmm_provider_invalidate_range(
                opaque, request, completion_out) == 0) {
            return 0;
        }
        usleep(1000);
    } while (mem_service_obmm_now_ms() < deadline);
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
        local_descriptor.size != local->len) {
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
        struct mem_service_obmm_descriptor_v1 descriptor;

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
            if (context->verified_peer_mem_ids[i] ==
                descriptor.export_mem_id) {
                break;
            }
        }
        if (i == context->verified_peer_count &&
            context->verified_peer_count <
                MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS) {
            context->verified_peer_mem_ids[context->verified_peer_count] =
                descriptor.export_mem_id;
            context->verified_peer_count += 1U;
        }
        context->mapping_verified =
            context->verified_peer_count >= context->required_peer_mappings;
    }
    return rc;
}

void mem_service_provider_obmm_endpoint_close(
    struct mem_service_provider_obmm_endpoint *endpoint)
{
    struct mem_service_obmm_context *context;
    size_t i;

    if (endpoint == NULL || endpoint->implementation == NULL) {
        return;
    }
    context = endpoint->implementation;
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->mappings[i].active) {
            (void)mem_service_obmm_provider_unmap_remote_region(
                context, context->mappings[i].handle);
        }
    }
    for (i = 0; i < MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS; ++i) {
        if (context->regions[i].active) {
            (void)obmm_do_unexport(
                context->obmm_fd,
                context->regions[i].descriptor.export_mem_id);
        }
    }
    if (context->obmm_fd >= 0) {
        close(context->obmm_fd);
    }
    free(context);
    endpoint->implementation = NULL;
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

void mem_service_provider_obmm_endpoint_close(
    struct mem_service_provider_obmm_endpoint *endpoint)
{
    if (endpoint != NULL) {
        endpoint->implementation = NULL;
    }
}
#endif

int mem_service_provider_obmm_run_protocol_fixture(void)
{
    struct mem_service_obmm_descriptor_v1 source = {
        .export_mem_id = 0x1020304050607080ULL,
        .remote_uba = 0x1122334455667788ULL,
        .size = 0x200000ULL,
        .token_id = 0x1234U,
        .export_cna = 0x45U,
    };
    struct mem_service_obmm_descriptor_v1 decoded;
    struct mem_service_provider_descriptor opaque;

    if (mem_service_obmm_descriptor_encode(&source, &opaque) != 0 ||
        mem_service_obmm_descriptor_decode(&opaque, &decoded) != 0 ||
        memcmp(&source, &decoded, sizeof(source)) != 0) {
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
    printf("mem_service obmm-provider-fixtures: status=ok "
           "descriptor_version=1 corruption=fail-closed "
           "mapping_path=sim-dec urma_dependency=none\n");
    return 0;
}
