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

mem_id __wrap_obmm_import(const struct obmm_mem_desc *desc, unsigned long flags,
                         int base_dist, int *numa)
{
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
    assert(fd == 42 && op == OBMM_CMD_GSVA_EVENT_V1);
    va_start(args, op);
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

static void assert_page_fault(const uint8_t *address)
{
    int status;
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        volatile uint8_t value = *(const volatile uint8_t *)address;
        (void)value;
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
}

static void test_logical_view_page_guards(void)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    struct obmm_helpers_region region = {.len = page * 4};
    region.addr = mmap(NULL, region.len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(region.addr != MAP_FAILED);
    assert(munmap(region.addr, region.len) == 0);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE;
    assert(!mem_service_obmm_map_view(region.addr, region.len, page + 13,
                                      page - 26, PROT_READ | PROT_WRITE, flags, -1));
    memset((uint8_t *)region.addr + page, 0x53, page);
    assert(((uint8_t *)region.addr)[page + 13] == 0x53);
    assert(((uint8_t *)region.addr)[page * 2 - 14] == 0x53);
    assert_page_fault(region.addr);
    assert_page_fault((uint8_t *)region.addr + page * 2);
    assert(munmap(region.addr, region.len) == 0);
    assert(!mem_service_obmm_map_view(region.addr, region.len, 0, region.len,
                                      PROT_READ | PROT_WRITE, flags, -1));
    assert(mem_service_obmm_map_view(region.addr, region.len, region.len, 1,
                                     PROT_READ, flags, -1));
    assert(mem_service_obmm_map_view(region.addr, region.len, 0, 0,
                                     PROT_READ, flags, -1));
    assert(munmap(region.addr, region.len) == 0);
    uint8_t *occupied = mmap((uint8_t *)region.addr + page, page,
                             PROT_READ | PROT_WRITE, flags, -1, 0);
    assert(occupied == (uint8_t *)region.addr + page);
    occupied[0] = 0x79;
    assert(mem_service_obmm_map_view(region.addr, region.len, page, page,
                                     PROT_READ, flags, -1));
    assert(occupied[0] == 0x79);
    /* A collision rolls back the new prefix, preserving the preexisting VMA. */
    assert(mmap(region.addr, page, PROT_NONE, flags, -1, 0) == region.addr);
    assert(munmap(region.addr, page) == 0);
    assert(munmap(occupied, page) == 0);
}

int main(void)
{
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
    test_logical_view_page_guards();
    puts("gsva_import_dual_token=pass gsva_visibility_fail_closed=pass page_guards=pass");
    return 0;
}
