/* No device access: exercise the production adapter with wrapped boundaries. */
#include "mem_service_provider_obmm.c"
#include <assert.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

static unsigned import_calls, event_calls;
static bool import_fails, ioctl_fails, event_fails;
static uint32_t last_event;
static unsigned update_calls;
static bool update_readonly;
static struct obmm_cmd_update_range last_update;

static bool cleanup_mode, open_fails, unimport_fails, unexport_fails, close_fails;
static unsigned cleanup_imports, cleanup_unimports, cleanup_unexports, cleanup_closes;
static unsigned cleanup_unmaps;
static unsigned cleanup_opens, cleanup_maps;
static void *failed_unmap_address;
static int mapping_fd = -1;
static int view_backing_fd = -1;
static unsigned cleanup_exports;
static int mapping_error;
static bool unexpected_mapping;
int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
void *__real_mmap(void *address, size_t len, int prot, int flags, int fd, off_t offset);
int __real_munmap(void *address, size_t len);

int __wrap_open(const char *path, int flags, ...)
{
    if (cleanup_mode && !strncmp(path, "/dev/obmm_shmdev", 16)) {
        ++cleanup_opens;
        if (open_fails) { errno = ENOENT; return -1; }
        mapping_fd = view_backing_fd >= 0 ? dup(view_backing_fd) :
                     __real_open("/dev/null", O_RDONLY);
        return mapping_fd;
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

void *__wrap_mmap(void *address, size_t len, int prot, int flags, int fd, off_t offset)
{
    if (cleanup_mode && fd == mapping_fd && fd >= 0) {
        ++cleanup_maps;
        if (mapping_error) { errno = mapping_error; return MAP_FAILED; }
        if (unexpected_mapping) {
            void *actual = __real_mmap(NULL, len, prot,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            assert(actual != MAP_FAILED && actual != address);
            failed_unmap_address = actual;
            return actual;
        }
        if (view_backing_fd >= 0)
            return __real_mmap(address, len, prot, MAP_SHARED | MAP_FIXED_NOREPLACE,
                               fd, offset);
        return __real_mmap(address, len, prot,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    }
    return __real_mmap(address, len, prot, flags, fd, offset);
}

int __wrap_munmap(void *address, size_t len)
{
    if (cleanup_mode) {
        ++cleanup_unmaps;
        if (address == failed_unmap_address) { errno = EIO; return -1; }
    }
    return __real_munmap(address, len);
}

int __wrap_close(int fd)
{
    if (cleanup_mode && fd == mapping_fd && fd >= 0) {
        ++cleanup_closes;
        int rc = __real_close(fd);
        mapping_fd = -1;
        if (close_fails) { errno = EIO; return -1; }
        return rc;
    }
    return __real_close(fd);
}

int __wrap_obmm_unimport(mem_id id, unsigned long flags)
{
    assert(cleanup_mode && id == 31 && flags == 0);
    ++cleanup_unimports;
    return unimport_fails ? -1 : 0;
}

int __wrap_obmm_unexport(mem_id id, unsigned long flags)
{
    assert(cleanup_mode && id == 17 && flags == 0);
    ++cleanup_unexports;
    return unexport_fails ? -1 : 0;
}

mem_id __wrap_obmm_export(const size_t length[OBMM_MAX_LOCAL_NUMA_NODES],
                         unsigned long flags, struct obmm_mem_desc *desc)
{
    assert(cleanup_mode && length[0] && flags == OBMM_EXPORT_FLAG_ALLOW_MMAP);
    ++cleanup_exports;
    memset(desc, 0, sizeof(*desc)); /* Export succeeded, descriptor token is invalid. */
    return 17;
}

mem_id __wrap_obmm_import(const struct obmm_mem_desc *desc, unsigned long flags,
                         int base_dist, int *numa)
{
    if (cleanup_mode) {
        assert(desc && flags == (OBMM_IMPORT_FLAG_ALLOW_MMAP | 0x8UL));
        ++cleanup_imports;
        return 31;
    }
    struct private_v4 {
        struct obmm_sim_dec_import_priv_v2 gsva;
        uint32_t gsva_token_id, reserved;
    } priv;
    import_calls++;
    assert(flags == (OBMM_IMPORT_FLAG_ALLOW_MMAP | 0x8UL));
    assert(!base_dist && numa);
    assert(desc->tokenid == 97 && desc->scna == 8 && desc->dcna == 7);
    assert(desc->length == 2097152 && desc->addr == 0x90000000);
    assert(desc->priv_len == sizeof(priv));
    memcpy(&priv, desc->priv, sizeof(priv));
    assert(priv.gsva.magic == OBMM_SIM_DEC_PRIV_MAGIC);
    assert(priv.gsva.version == 4 && priv.gsva.len == sizeof(priv));
    assert(priv.gsva_token_id == 2 && !priv.reserved);
    assert(priv.gsva.token_value == 3 && priv.gsva.segment_id == 9);
    assert(priv.gsva.epoch == 5 && priv.gsva.remote_uba == 0x700000000000ULL);
    assert(priv.gsva.local_va == priv.gsva.remote_uba);
    assert(priv.gsva.home_va == priv.gsva.remote_uba);
    assert(priv.gsva.address_profile == OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY);
    return import_fails ? OBMM_INVALID_MEMID : 31;
}

int __wrap_ioctl(int fd, unsigned long op, ...)
{
    va_list args;
    struct obmm_cmd_gsva_event_v1 *event;
    va_start(args, op);
    if (op == OBMM_SHMDEV_UPDATE_RANGE) {
        assert(fd == 43);
        last_update = *va_arg(args, struct obmm_cmd_update_range *);
        va_end(args);
        ++update_calls;
        if (ioctl_fails || (update_readonly &&
            (last_update.mem_state & OBMM_SHM_MEM_READWRITE))) {
            errno = EACCES;
            return -1;
        }
        return 0;
    }
    assert(fd == 42 && op == OBMM_CMD_GSVA_EVENT_V1);
    event = va_arg(args, struct obmm_cmd_gsva_event_v1 *);
    va_end(args);
    event_calls++;
    last_event = event->sub_op;
    assert(event->requester_cna == 8 && event->token_id == 2 && event->token_value == 3);
    assert(event->key.segment_id == 9 && event->key.epoch == 5);
    assert(event->key.size == 2097152 && event->key.home_va == 0x700000000000ULL);
    event->error = event_fails ? GSVA_ERR_TOKEN_DENIED : GSVA_OK;
    return ioctl_fails ? -1 : 0;
}

static struct mem_service_obmm_context *cleanup_context(
    struct mem_service_provider_obmm_endpoint *endpoint, size_t size)
{
    cleanup_mode = true;
    open_fails = unimport_fails = unexport_fails = close_fails = false;
    cleanup_imports = cleanup_unimports = cleanup_unexports = cleanup_closes = 0;
    cleanup_unmaps = 0;
    cleanup_opens = cleanup_maps = 0;
    failed_unmap_address = NULL;
    mapping_error = 0;
    unexpected_mapping = false;
    mapping_fd = -1;
    struct mem_service_obmm_context *context = calloc(1, sizeof(*context));
    assert(context);
    context->obmm_fd = __real_open("/dev/null", O_RDONLY);
    assert(context->obmm_fd >= 0);
    context->local_cna = 8;
    context->max_remote_mappings = 1;
    context->import_region_bytes = size;
    context->import_pas[0] = 0x90000000;
    context->mapping_verified = true;
    endpoint->implementation = context;
    return context;
}

static struct mem_service_mapping_request cleanup_request(void *base, size_t page)
{
    struct mem_service_obmm_descriptor_v1 descriptor = {
        .strict_gsva = true, .export_mem_id = 17, .token_id = 97, .export_cna = 7,
        .remote_uba = (uint64_t)(uintptr_t)base, .size = page * 4,
        .segment_id = 9, .epoch = 5, .gsva_token_id = 2, .gsva_token_value = 3,
        .access_flags = 3, .cache_policy = GSVA_CACHE_POLICY_WRITE_THROUGH,
        .segment_flags = 7, .node_count = 2,
    };
    struct mem_service_mapping_request request = {
        .remote_region_len = page * 4, .offset = page, .len = page,
        .requested_address = (uint8_t *)base + page,
        .memory_kind = MEM_SERVICE_MEMORY_HOST,
        .flags = MEM_SERVICE_MAPPING_FLAG_READ | MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS,
    };
    assert(!mem_service_obmm_descriptor_encode(&descriptor, &request.remote_descriptor));
    return request;
}

static void test_compute_mapping_pins(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *base = __real_mmap(NULL, page * 4, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(base != MAP_FAILED && !__real_munmap(base, page * 4));
    struct mem_service_mapping_request request = cleanup_request(base, page);
    struct mem_service_mapping mapping = {0};
    struct mem_service_provider_obmm_endpoint endpoint = {0};
    struct mem_service_obmm_context *context = cleanup_context(&endpoint, page * 4);
    assert(!mem_service_obmm_provider_map_remote_region(context, &request, &mapping));
    struct mem_service_provider provider = {
        .ops = &mem_service_obmm_provider_ops, .context = context,
    };
    struct mem_service_provider_channel channel = {.provider = &provider};
    struct mem_service_provider_mapping_binding binding = {
        .mapping = mapping, .owner = &provider, .mapped = true,
    };
    struct mem_service_provider_obmm_mapping_pin *first = NULL, *second = NULL, *denied = NULL;
    struct mem_service_provider_obmm_pinned_mapping view, other;
    struct mem_service_obmm_mapping_slot *slot = &context->mappings[0];
    assert(!mem_service_provider_obmm_mapping_pin_acquire(&binding,
        MEM_SERVICE_MAPPING_FLAG_READ, &first, &view));
    assert(first && slot->compute_pins == 1 && view.base == mapping.base && view.len == page);
    assert(view.mem_id == 31 && view.obmm_fd == context->obmm_fd &&
           view.access_flags == MEM_SERVICE_MAPPING_FLAG_READ);
    assert(!mem_service_provider_obmm_mapping_pin_acquire(&binding,
        MEM_SERVICE_MAPPING_FLAG_READ, &second, &other));
    assert(second && second != first && slot->compute_pins == 2);
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding,
        MEM_SERVICE_MAPPING_FLAG_WRITE, &denied, &other) == -EACCES);
    assert(!denied && !other.base && !other.len && other.obmm_fd == -1);
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding,
        MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS, &denied, &other) == -EINVAL);
    --binding.mapping.len;
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -ESTALE);
    ++binding.mapping.len;
    ++binding.mapping.handle;
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -ESTALE);
    --binding.mapping.handle;
    provider.ops = NULL;
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -EOPNOTSUPP);
    provider.ops = &mem_service_obmm_provider_ops;
    slot->imported = false;
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -EOPNOTSUPP);
    slot->imported = true;
    slot->descriptor.strict_gsva = false;
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -EOPNOTSUPP);
    slot->descriptor.strict_gsva = true;
    slot->compute_pins = UINT64_MAX;
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -EOVERFLOW);
    slot->compute_pins = 2;
    assert(mem_service_obmm_provider_unmap_remote_region(context, mapping.handle) == -EBUSY);
    assert(mem_service_provider_channel_unmap_remote_region(&channel, &binding));
    assert(binding.mapped && !binding.mapping.base && !binding.mapping.len);
    assert(slot->region.addr == base && slot->view_len == page && slot->compute_pins == 2);
    assert(!cleanup_unmaps && !cleanup_closes && !cleanup_unimports && cleanup_imports == 1);
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -ESTALE);
    assert(mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(endpoint.implementation == context && context->closing && slot->compute_pins == 2);
    assert(mem_service_provider_obmm_mapping_pin_acquire(&binding, 1, &denied, &other) == -EBUSY);
    assert(!mem_service_provider_obmm_mapping_pin_release(&first) && !first && slot->compute_pins == 1);
    assert(!mem_service_provider_obmm_mapping_pin_release(&first));
    assert(mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    ++second->mapping_handle;
    assert(mem_service_provider_obmm_mapping_pin_release(&second) == -ESTALE && second);
    --second->mapping_handle;
    assert(!mem_service_provider_obmm_mapping_pin_release(&second) && !second && !slot->compute_pins);
    assert(!cleanup_unmaps && !cleanup_closes && !cleanup_unimports);
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(!endpoint.implementation && cleanup_unmaps == 3 && cleanup_closes == 1 && cleanup_unimports == 1);
    cleanup_mode = false;
    puts("obmm_compute_mapping_pins=pass no_alias=1 deferred_cleanup=1");
}

static void test_import_and_partial_view_cleanup(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *base = __real_mmap(NULL, page * 4, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(base != MAP_FAILED && !__real_munmap(base, page * 4));
    struct mem_service_mapping_request request = cleanup_request(base, page);
    struct mem_service_mapping mapping = {0};
    struct mem_service_provider_obmm_endpoint endpoint = {0};
    struct mem_service_obmm_context *context = cleanup_context(&endpoint, page * 4);
    open_fails = unimport_fails = true;
    assert(mem_service_obmm_provider_map_remote_region(context, &request, &mapping) ==
           MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
    assert(cleanup_imports == 1 && cleanup_unimports == 1 && mapping.handle);
    assert(context->mappings[0].active && context->mappings[0].imported);
    assert(context->mappings[0].region.mem_id == 31 && !mapping.base && !mapping.len);
    assert(mem_service_obmm_provider_map_remote_region(context, &request, &mapping));
    assert(cleanup_imports == 1); /* No slot reuse over an unresolved import. */
    assert(mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(endpoint.implementation == context && context->closing);
    unimport_fails = false;
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(!endpoint.implementation && cleanup_unimports == 3);

    context = cleanup_context(&endpoint, page * 4);
    uint8_t *foreign = __real_mmap((uint8_t *)base + page, page, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(foreign == (uint8_t *)base + page);
    foreign[0] = 0x79;
    failed_unmap_address = base;
    assert(mem_service_obmm_provider_map_remote_region(context, &request, &mapping) ==
           MEM_SERVICE_MAPPING_CLEANUP_REQUIRED);
    assert(mapping.handle && !mapping.base && !mapping.len);
    assert(context->mappings[0].view.parts[0].owned);
    assert(context->mappings[0].view.parts[0].address == base);
    assert(!cleanup_unimports && !cleanup_closes && foreign[0] == 0x79);
    assert(mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(endpoint.implementation == context && foreign[0] == 0x79);
    enum mem_service_provider_state state;
    assert(!mem_service_obmm_provider_probe(context, &state));
    assert(state == MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE);
    assert(mem_service_obmm_provider_map_remote_region(context, &request, &mapping));
    unsigned attempts = cleanup_unmaps;
    failed_unmap_address = NULL;
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(cleanup_unmaps == attempts + 1 && cleanup_unimports == 1);
    assert(foreign[0] == 0x79 && !__real_munmap(foreign, page));
    cleanup_mode = false;
}

static void test_retained_handle_conflict_probe(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *base = __real_mmap(NULL, page * 4, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(base != MAP_FAILED && !__real_munmap(base, page * 4));
    struct mem_service_mapping_request request = cleanup_request(base, page);
    request.flags |= MEM_SERVICE_MAPPING_FLAG_WRITE;
    struct mem_service_mapping mapping = {0};
    struct mem_service_provider_obmm_endpoint endpoint = {0};
    struct mem_service_obmm_context *context = cleanup_context(&endpoint, page * 4);
    struct mem_service_provider_obmm_resources_v1 resources;
    assert(mem_service_provider_obmm_endpoint_probe_conflict(NULL, 1));
    assert(mem_service_provider_obmm_endpoint_probe_conflict(&endpoint, 1));
    assert(!mem_service_obmm_provider_map_remote_region(context, &request, &mapping));
    memset(mapping.base, 0x59, mapping.len);
    for (unsigned i = 0; i < 2; ++i) {
        assert(!mem_service_provider_obmm_endpoint_probe_conflict(&endpoint, mapping.handle));
        assert(((uint8_t *)mapping.base)[page - 1] == 0x59);
        assert(cleanup_imports == 1 && !cleanup_unimports && !cleanup_closes);
        assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
        assert(resources.vma_count == 3 && resources.vma_bytes == page * 4);
        assert(resources.import_handles == 1 && !resources.cleanup_mappings);
    }
    /* A generic error must never count as an address conflict. */
    mapping_error = ENOMEM;
    assert(mem_service_provider_obmm_endpoint_probe_conflict(&endpoint, mapping.handle));
    assert(!context->closing && ((uint8_t *)mapping.base)[0] == 0x59);
    mapping_error = 0;
    context->mappings[0].descriptor.strict_gsva = false;
    assert(mem_service_provider_obmm_endpoint_probe_conflict(&endpoint, mapping.handle));
    context->mappings[0].descriptor.strict_gsva = true;
    /* Retain an unexpected VMA if rollback fails; never drop the original fd. */
    unexpected_mapping = true;
    assert(mem_service_provider_obmm_endpoint_probe_conflict(&endpoint, mapping.handle));
    assert(context->closing && ((uint8_t *)mapping.base)[0] == 0x59);
    assert(mem_service_provider_obmm_endpoint_probe_conflict(&endpoint, mapping.handle));
    assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(resources.vma_count == 4 && resources.vma_bytes == page * 5);
    assert(resources.cleanup_mappings == 1 && resources.import_handles == 1);
    assert(mem_service_obmm_provider_unmap_remote_region(context, mapping.handle));
    assert(!cleanup_unimports && !cleanup_closes);
    assert(((uint8_t *)mapping.base)[0] == 0x59);
    failed_unmap_address = NULL;
    unexpected_mapping = false;
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(!endpoint.implementation && cleanup_unimports == 1 && cleanup_closes == 1);
    cleanup_mode = false;
    puts("retained_handle_conflict=pass");
}

static void test_failed_export_encoding_retains_resources(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    for (unsigned fail_cleanup = 0; fail_cleanup < 2; ++fail_cleanup) {
        struct mem_service_provider_obmm_endpoint endpoint = {0};
        struct mem_service_obmm_context *context = cleanup_context(&endpoint, page * 4);
        struct mem_service_region_request request = {
            .len = page * 4, .memory_kind = MEM_SERVICE_MEMORY_HOST,
            .flags = MEM_SERVICE_REGION_FLAG_PROVIDER_ALLOCATED,
        };
        struct mem_service_region region = {0};
        struct mem_service_provider_obmm_resources_v1 resources;
        cleanup_exports = 0;
        context->next_region_handle = UINT64_MAX;
        assert(mem_service_obmm_provider_register_region(context, &request, &region));
        assert(!cleanup_exports);
        context->next_region_handle = 0;
        unexport_fails = fail_cleanup;
        assert(mem_service_obmm_provider_register_region(context, &request, &region));
        assert(cleanup_exports == 1 && !region.handle);
        assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
        assert(resources.export_handles == fail_cleanup);
        assert(resources.export_bytes == fail_cleanup * page * 4);
        assert(resources.closing == (bool)fail_cleanup);
        unexport_fails = false;
        assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    }
}

static void test_retained_descriptor_probe(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *base = __real_mmap(NULL, page * 4, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(base != MAP_FAILED && !__real_munmap(base, page * 4));
    struct mem_service_mapping_request request = cleanup_request(base, page);
    struct mem_service_mapping mapping = {0};
    struct mem_service_provider_obmm_endpoint endpoint = {0};
    struct mem_service_obmm_context *context = cleanup_context(&endpoint, page * 4);
    assert(!mem_service_obmm_provider_map_remote_region(context, &request, &mapping));
    assert(mem_service_provider_obmm_endpoint_probe_descriptor(&endpoint, mapping.handle));
    context->max_remote_mappings = 2;
    context->import_pas[1] = 0xa0000000;
    unsigned opens = cleanup_opens, maps = cleanup_maps, events = event_calls;
    assert(!mem_service_provider_obmm_endpoint_probe_descriptor(&endpoint, mapping.handle));
    assert(!mem_service_provider_obmm_endpoint_probe_descriptor(&endpoint, mapping.handle));
    assert(cleanup_imports == 1 && !cleanup_unimports && !cleanup_unmaps && !cleanup_closes);
    assert(cleanup_opens == opens && cleanup_maps == maps && event_calls == events);
    assert(context->next_mapping_handle == mapping.handle && context->mappings[0].active);
    assert(!context->mappings[1].active && !context->closing);
    assert(!*(const uint8_t *)mapping.base);
    assert(mem_service_provider_obmm_endpoint_probe_descriptor(&endpoint, mapping.handle + 1));
    context->closing = true;
    assert(mem_service_provider_obmm_endpoint_probe_descriptor(&endpoint, mapping.handle));
    context->closing = false;
    context->next_mapping_handle = UINT64_MAX;
    assert(mem_service_provider_obmm_endpoint_probe_descriptor(&endpoint, mapping.handle));
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    puts("retained_descriptor_probe=pass checks=24 device_operations=0");
}

static void test_unmap_and_endpoint_failure_retention(void)
{
    struct mem_service_provider_obmm_resources_v1 resources;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *base = __real_mmap(NULL, page * 4, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(base != MAP_FAILED && !__real_munmap(base, page * 4));
    struct mem_service_mapping_request request = cleanup_request(base, page);
    struct mem_service_mapping mapping = {0};
    struct mem_service_provider_obmm_endpoint endpoint = {0};
    struct mem_service_obmm_context *context = cleanup_context(&endpoint, page * 4);
    assert(!mem_service_obmm_provider_map_remote_region(context, &request, &mapping));
    assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(resources.import_handles == 1 && resources.import_bytes == page * 4);
    assert(resources.vma_bytes == page * 4 && resources.accessible_views == 1);
    assert(resources.accessible_bytes == page && !resources.cleanup_mappings);
    context->closing = true;
    assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(resources.accessible_views == 1 && resources.cleanup_mappings == 1);
    context->closing = false;
    failed_unmap_address = base;
    assert(mem_service_obmm_provider_unmap_remote_region(context, mapping.handle));
    assert(context->mappings[0].active && !context->mappings[0].view_len);
    assert(!context->mappings[0].region.addr && !cleanup_unimports && !cleanup_closes);
    assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(resources.import_handles == 1 && resources.vma_bytes == page);
    assert(resources.vma_count == 1 && resources.cleanup_mappings == 1);
    assert(!resources.accessible_views && !resources.accessible_bytes);
    struct mem_service_mapping_range_request range = {
        .mapping_handle = mapping.handle, .len = 1, .expected_checksum = 1, .timeout_ms = 1,
    };
    struct mem_service_visibility_completion completion;
    assert(mem_service_obmm_provider_publish_range(context, &range, &completion));
    assert(mem_service_obmm_provider_invalidate_range(context, &range, &completion));
    assert(mem_service_obmm_provider_wait_range_visible(context, &range, &completion));
    unsigned attempts = cleanup_unmaps;
    failed_unmap_address = NULL;
    assert(!mem_service_obmm_provider_unmap_remote_region(context, mapping.handle));
    assert(cleanup_unmaps == attempts + 1 && cleanup_unimports == 1 && cleanup_closes == 1);
    assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(!resources.import_handles && !resources.vma_count && !resources.cleanup_mappings);
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(!resources.import_handles && !resources.vma_bytes);

    context = cleanup_context(&endpoint, page * 4);
    assert(!mem_service_obmm_provider_map_remote_region(context, &request, &mapping));
    close_fails = true;
    assert(mem_service_obmm_provider_unmap_remote_region(context, mapping.handle));
    assert(context->mappings[0].close_uncertain && context->mappings[0].region.fd == -1);
    int reused_fd = __real_open("/dev/null", O_RDONLY);
    assert(reused_fd >= 0);
    assert(mem_service_obmm_provider_unmap_remote_region(context, mapping.handle));
    assert(cleanup_closes == 1 && cleanup_unimports == 0 && fcntl(reused_fd, F_GETFD) >= 0);
    assert(mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(endpoint.implementation == context && fcntl(reused_fd, F_GETFD) >= 0);
    assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(resources.closing && resources.import_handles == 1);
    assert(!resources.vma_bytes && resources.cleanup_mappings == 1);
    assert(!__real_close(reused_fd));
    /* The imported ID is synthetic; release only this fixture's real fd and
     * heap after proving production cleanup retained the quarantine. */
    assert(!__real_close(context->obmm_fd));
    free(context);
    endpoint.implementation = NULL;

    context = cleanup_context(&endpoint, page * 4);
    context->regions[0].active = true;
    context->regions[0].handle = 9;
    context->regions[0].descriptor.export_mem_id = 17;
    context->regions[0].descriptor.size = page * 4;
    unexport_fails = true;
    assert(mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(endpoint.implementation == context && context->regions[0].active);
    assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
    assert(resources.closing && resources.export_handles == 1 && resources.export_bytes == page * 4);
    mem_service_provider_obmm_endpoint_close(&endpoint);
    assert(endpoint.implementation == context && cleanup_unexports == 2);
    unexport_fails = false;
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    assert(!endpoint.implementation && cleanup_unexports == 3);
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    cleanup_mode = false;
}

static void assert_access_fault(const uint8_t *address, bool write)
{
    int status;
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        if (write) {
            *(volatile uint8_t *)address = 0x91;
        } else {
            volatile uint8_t value = *(const volatile uint8_t *)address;
            (void)value;
        }
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}

static void assert_page_fault(const uint8_t *address)
{
    assert_access_fault(address, false);
}

static void test_fixed_subrange_mapping(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE), size = page * 4;
    FILE *backing = tmpfile();
    assert(backing && !ftruncate(fileno(backing), (off_t)size));
    uint8_t *owner = __real_mmap(NULL, size, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fileno(backing), 0);
    void *base = __real_mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(owner != MAP_FAILED && base != MAP_FAILED && !__real_munmap(base, size));
    struct mem_service_provider_obmm_endpoint endpoint = {0};
    struct mem_service_obmm_context *context = cleanup_context(&endpoint, size);
    view_backing_fd = fileno(backing);
    struct mem_service_provider provider = {
        .name = "obmm", .ops = &mem_service_obmm_provider_ops, .context = context,
        .capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
    };
    struct mem_service_provider_channel channel = {.provider = &provider};
    struct mem_service_mapping_request request = cleanup_request(base, page);
    struct mem_service_provider_remote_region remote = {
        .provider_name = "obmm", .descriptor = request.remote_descriptor,
        .len = size, .memory_kind = MEM_SERVICE_MEMORY_HOST,
    };
    struct mem_service_provider_mapping_binding binding = {0};
    struct mem_service_provider_obmm_resources_v1 resources;
    const uint64_t offsets[] = {page, page + 13, page * 3 + 17, 0};
    const uint64_t lengths[] = {page, page - 26, page - 17, size};
    for (unsigned i = 0; i < 4; ++i) {
        memset(owner, 0x39 + i, size);
        uint8_t *wanted = (uint8_t *)base + offsets[i];
        assert(!mem_service_provider_channel_map_remote_region(&channel, &remote,
            offsets[i], lengths[i], wanted, request.flags, &binding));
        assert(binding.mapped && binding.owner == &provider && binding.mapping.base == wanted);
        assert(binding.mapping.len == lengths[i]);
        assert(wanted[0] == 0x39 + i && wanted[lengths[i] - 1] == 0x39 + i);
        owner[offsets[i]] = 0x6b;
        owner[offsets[i] + lengths[i] - 1] = 0x7c;
        assert(wanted[0] == 0x6b && wanted[lengths[i] - 1] == 0x7c);
        assert_access_fault(wanted, true);
        if (offsets[i] >= page) assert_page_fault(base);
        if (offsets[i] + lengths[i] <= page * 3)
            assert_page_fault((uint8_t *)base + page * 3);
        assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
        assert(resources.import_handles == 1 && resources.import_bytes == size);
        assert(resources.vma_bytes == size && resources.accessible_bytes == lengths[i]);
        assert(context->mappings[0].view_offset == offsets[i]);
        assert(!mem_service_provider_channel_unmap_remote_region(&channel, &binding));
        assert(!mem_service_provider_obmm_endpoint_resources_v1(&endpoint, &resources));
        assert(!resources.import_handles && !resources.vma_count && !resources.cleanup_mappings);
    }

    /* Rejections happen before imports, file opens, VMA allocation or handle changes. */
    for (unsigned i = 0; i < 10; ++i) {
        struct mem_service_mapping_request denied = request;
        struct mem_service_mapping output, saved;
        memset(&output, 0xa5, sizeof(output));
        saved = output;
        struct mem_service_obmm_descriptor_v1 descriptor;
        assert(!mem_service_obmm_descriptor_decode(&denied.remote_descriptor, &descriptor));
        switch (i) {
        case 0: denied.requested_address = base; break;
        case 1: denied.requested_address = (uint8_t *)base + page + 1; break;
        case 2: denied.requested_address = NULL; break;
        case 3: denied.offset = size; break;
        case 4: denied.len = size; break;
        case 5: denied.offset = UINT64_MAX; break;
        case 6: denied.flags &= ~MEM_SERVICE_MAPPING_FLAG_FIXED_ADDRESS; break;
        case 7:
            descriptor.strict_gsva = false;
            assert(!mem_service_obmm_descriptor_encode(&descriptor, &denied.remote_descriptor));
            break;
        case 8:
            descriptor.access_flags = OBMM_GSVA_ACCESS_READ;
            denied.flags |= MEM_SERVICE_MAPPING_FLAG_WRITE;
            assert(!mem_service_obmm_descriptor_encode(&descriptor, &denied.remote_descriptor));
            break;
        case 9: denied.len = 0; break;
        }
        unsigned imports = cleanup_imports, opens = cleanup_opens, maps = cleanup_maps;
        uint64_t next = context->next_mapping_handle;
        assert(mem_service_obmm_provider_map_remote_region(context, &denied, &output));
        assert(!memcmp(&output, &saved, sizeof(output)));
        assert(cleanup_imports == imports && cleanup_opens == opens && cleanup_maps == maps);
        assert(context->next_mapping_handle == next && !context->mappings[0].active);
    }
    assert(cleanup_imports == 4 && cleanup_unimports == 4 && cleanup_closes == 4);
    assert(!mem_service_provider_obmm_endpoint_close_checked(&endpoint));
    view_backing_fd = -1;
    cleanup_mode = false;
    assert(!__real_munmap(owner, size) && !fclose(backing));
    puts("obmm_fixed_subrange=pass views=4 rejected=10 file_alias=1 readonly=1 resources=0");
}

static void test_cached_visibility_permissions(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uint8_t *bytes = mmap(NULL, page * 3, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(bytes != MAP_FAILED);
    memset(bytes, 0x63, page * 3);
    struct mem_service_obmm_context context = {0};
    struct mem_service_obmm_mapping_slot *slot = &context.mappings[0];
    *slot = (struct mem_service_obmm_mapping_slot){
        .active = true, .handle = 1, .view_offset = page + 17,
        .view_len = page, .region = {.addr = bytes, .len = page * 3, .fd = 43},
        .descriptor = {.strict_gsva = true, .access_flags = 3},
    };
    struct mem_service_mapping_range_request range = {
        .mapping_handle = 1, .offset = 13, .len = page - 26, .timeout_ms = 2,
        .expected_checksum = mem_service_provider_checksum64(
            bytes + slot->view_offset + 13, page - 26),
    };
    for (unsigned writable = 0; writable < 2; ++writable) {
        slot->view_access = MEM_SERVICE_MAPPING_FLAG_READ |
            (writable ? MEM_SERVICE_MAPPING_FLAG_WRITE : 0);
        update_readonly = !writable;
        struct mem_service_visibility_completion completion = {0};
        unsigned calls = update_calls;
        assert(!mem_service_obmm_provider_wait_range_visible(&context, &range, &completion));
        assert(update_calls == calls + 1);
        assert(last_update.start == (uintptr_t)bytes + page);
        assert(last_update.end == (uintptr_t)bytes + page * 3);
        assert(last_update.mem_state == (OBMM_SHM_MEM_NORMAL |
            (writable ? OBMM_SHM_MEM_READWRITE : OBMM_SHM_MEM_READONLY)));
        assert(last_update.cache_ops == OBMM_SHM_CACHE_INVAL);
        assert(completion.visible_bytes == range.len &&
               completion.checksum == range.expected_checksum);
        if (writable) {
            assert(!mem_service_obmm_provider_publish_range(&context, &range, &completion));
            assert(last_update.cache_ops == OBMM_SHM_CACHE_WB_INVAL);
        }
        memset(&completion, 0xa5, sizeof(completion));
        struct mem_service_visibility_completion saved = completion;
        ioctl_fails = true;
        assert(mem_service_obmm_provider_invalidate_range(&context, &range, &completion));
        assert(!memcmp(&saved, &completion, sizeof(saved)));
        ioctl_fails = false;
        calls = update_calls;
        range.len = page;
        assert(mem_service_obmm_provider_invalidate_range(&context, &range, &completion));
        assert(update_calls == calls);
        range.len = page - 26;
    }
    update_readonly = false;
    assert(!munmap(bytes, page * 3));
    puts("obmm_cached_visibility=pass readonly=1 readwrite=1 bounds=1 ioctl_failure=1");
}

static void test_logical_view_page_guards(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    struct obmm_helpers_region region = {.len = page * 4};
    struct mem_service_obmm_view view = {0};
    region.addr = mmap(NULL, region.len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region.addr != MAP_FAILED);
    assert(munmap(region.addr, region.len) == 0);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
    assert(!mem_service_obmm_map_view(region.addr, region.len, page + 13,
                                      page - 26, PROT_READ | PROT_WRITE, flags, -1, &view));
    memset((uint8_t *)region.addr + page, 0x53, page);
    assert(((uint8_t *)region.addr)[page + 13] == 0x53);
    assert(((uint8_t *)region.addr)[page * 2 - 14] == 0x53);
    assert_page_fault(region.addr);
    assert_page_fault((uint8_t *)region.addr + page * 2);
    assert(mem_service_obmm_unmap_view(&view) == 0);
    assert(!mem_service_obmm_map_view(region.addr, region.len, 0, region.len,
                                      PROT_READ | PROT_WRITE, flags, -1, &view));
    assert(mem_service_obmm_map_view(region.addr, region.len, region.len, 1,
                                     PROT_READ, flags, -1, &view));
    assert(mem_service_obmm_map_view(region.addr, region.len, 0, 0,
                                     PROT_READ, flags, -1, &view));
    assert(mem_service_obmm_unmap_view(&view) == 0);
    uint8_t *occupied = mmap((uint8_t *)region.addr + page, page,
                             PROT_READ | PROT_WRITE, flags, -1, 0);
    assert(occupied == (uint8_t *)region.addr + page);
    occupied[0] = 0x79;
    assert(mem_service_obmm_map_view(region.addr, region.len, page, page,
                                     PROT_READ, flags, -1, &view));
    assert(occupied[0] == 0x79);
    /* A collision rolls back the new prefix, preserving the preexisting VMA. */
    assert(mmap(region.addr, page, PROT_NONE, flags, -1, 0) == region.addr);
    assert(munmap(region.addr, page) == 0);
    assert(munmap(occupied, page) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--cached-visibility")) {
        test_cached_visibility_permissions();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--fixed-subrange")) {
        test_fixed_subrange_mapping();
        return 0;
    }
    if (argc != 1) return 2;
    struct mem_service_obmm_context context = {.obmm_fd = 42, .local_cna = 8};
    struct mem_service_obmm_mapping_slot *slot = &context.mappings[0];
    struct mem_service_visibility_completion completion;
    uint8_t bytes[17] = {1, 2, 3};
    struct mem_service_mapping_range_request range = {
        .mapping_handle = 1, .len = sizeof(bytes),
        .expected_checksum = mem_service_provider_checksum64(bytes, sizeof(bytes)),
    };
    uint64_t imported = 0;

    *slot = (struct mem_service_obmm_mapping_slot){
        .active = true, .imported = true, .map_osync = true, .handle = 1,
        .view_len = sizeof(bytes), .region = {.addr = bytes, .fd = 43},
        .descriptor = {.strict_gsva = true, .export_mem_id = 17,
            .token_id = 97, .export_cna = 7, .remote_uba = 0x700000000000ULL,
            .size = 2097152, .segment_id = 9, .epoch = 5,
            .gsva_token_id = 2, .gsva_token_value = 3, .access_flags = 3,
            .cache_policy = GSVA_CACHE_POLICY_WRITE_THROUGH},
    };
    assert(!mem_service_obmm_import_gsva(&context, &slot->descriptor, 0x90000000, &imported));
    assert(imported == 31 && import_calls == 1);
    import_fails = true;
    imported = 0;
    assert(mem_service_obmm_import_gsva(&context, &slot->descriptor, 0x90000000, &imported));
    assert(!imported && import_calls == 2); /* No legacy retry. */
    assert(!mem_service_obmm_provider_invalidate_range(&context, &range, &completion));
    assert(event_calls == 2 && last_event == OBMM_GSVA_EVENT_FENCE);
    assert(completion.visible_bytes == sizeof(bytes));
    assert(!mem_service_obmm_provider_publish_range(&context, &range, &completion));
    assert(event_calls == 3 && last_event == OBMM_GSVA_EVENT_FENCE);
    ioctl_fails = true;
    assert(mem_service_obmm_provider_invalidate_range(&context, &range, &completion));
    assert(event_calls == 4 && last_event == OBMM_GSVA_EVENT_READ_ACQUIRE);
    ioctl_fails = false;
    event_fails = true;
    assert(mem_service_obmm_provider_publish_range(&context, &range, &completion));
    event_fails = false;
    range.expected_checksum++;
    assert(mem_service_obmm_provider_invalidate_range(&context, &range, &completion));
    event_calls = 0;
    range.len++;
    assert(mem_service_obmm_provider_publish_range(&context, &range, &completion));
    assert(mem_service_obmm_provider_invalidate_range(&context, &range, &completion));
    assert(!event_calls); /* Invalid ranges cannot start coherence operations. */
    test_cached_visibility_permissions();
    test_logical_view_page_guards();
    test_fixed_subrange_mapping();
    test_import_and_partial_view_cleanup();
    test_retained_handle_conflict_probe();
    test_retained_descriptor_probe();
    test_unmap_and_endpoint_failure_retention();
    test_failed_export_encoding_retains_resources();
    test_compute_mapping_pins();
    puts("obmm_cleanup_ownership=pass");
    puts("gsva_import_dual_token=pass gsva_visibility_fail_closed=pass page_guards=pass");
    return 0;
}
