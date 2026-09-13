#define _POSIX_C_SOURCE 200809L
#include "mem_service_provider_obmm.h"
#include "../mem_service_client.h"
#include "../mem_service_allocation.h"
#include "gva_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

struct worker_config {
    char connect[256];
    char node[MEM_SERVICE_CLIENT_PROVIDER_NODE_ID_LEN];
    char state[1024];
    uint64_t incarnation;
    uint64_t readiness_generation;
    uint64_t allocation_granularity_bytes;
    bool fast_allocation;
};

struct worker_reservation {
    bool occupied;
    bool export_no_backing;
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    uint64_t generation;
    struct obmm_gsva_segment_desc_v1 segment;
    struct obmm_cmd_export exported;
    struct mem_service_provider_descriptor descriptor;
};

enum worker_allocate_result {
    WORKER_ALLOCATE_ERROR = -1,
    WORKER_ALLOCATE_OK = 0,
    /* Proven before any resource-creating ioctl or reservation mutation. */
    WORKER_ALLOCATE_CAPACITY = 1,
    WORKER_ALLOCATE_BACKING_EMPTY = 2,
};

static volatile sig_atomic_t worker_stop;

static void stop_worker(int signal_number)
{
    (void)signal_number;
    worker_stop = 1;
}

static int read_config(const char *path, struct worker_config *config)
{
    static const char *names[] = {
        "connect", "node_id", "state_file", "incarnation", "readiness_generation",
        "allocation_granularity_bytes", "fast_allocation"
    };
    FILE *file = fopen(path, "r");
    char line[2048];
    unsigned seen = 0;
    int result = -1;

    if (!file) return -1;
    memset(config, 0, sizeof(*config));
    while (fgets(line, sizeof(line), file)) {
        char *value, *end;
        size_t i, len = strlen(line);
        if (!len || (line[len - 1] != '\n' && !feof(file))) goto done;
        if (len && line[len - 1] == '\n') line[--len] = 0;
        if (!len || line[0] == '#') continue;
        value = strchr(line, '=');
        if (!value) goto done;
        *value++ = 0;
        if (!*value || strchr(value, '\r')) goto done;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(line, names[i])) break;
        if (i == sizeof(names) / sizeof(names[0]) || (seen & (1U << i))) goto done;
        seen |= 1U << i;
        if (i < 3) {
            char *target = i == 0 ? config->connect : i == 1 ? config->node : config->state;
            size_t capacity = i == 0 ? sizeof(config->connect) :
                              i == 1 ? sizeof(config->node) : sizeof(config->state);
            if (strlen(value) >= capacity) goto done;
            strcpy(target, value);
        } else {
            uint64_t number;
            if (*value < '0' || *value > '9') goto done;
            errno = 0;
            number = strtoull(value, &end, 0);
            if (errno || *end || (i != 6 && !number) || (i == 6 && number > 1)) goto done;
            if (i == 3) config->incarnation = number;
            else if (i == 4) config->readiness_generation = number;
            else if (i == 5) config->allocation_granularity_bytes = number;
            else config->fast_allocation = number != 0;
        }
    }
    if (!ferror(file) && (seen & 63U) == 63U && config->state[0] == '/' &&
        !(config->allocation_granularity_bytes & (config->allocation_granularity_bytes - 1)) &&
        sysconf(_SC_PAGESIZE) > 0 &&
        config->allocation_granularity_bytes >= (uint64_t)sysconf(_SC_PAGESIZE)) result = 0;
done:
    fclose(file);
    return result;
}

static int record_phase(int fd, uint64_t generation, const char *phase,
                         uint64_t segment, uint64_t exported)
{
    char line[192];
    int length = snprintf(line, sizeof(line),
        "generation=%" PRIu64 " phase=%s segment=%" PRIu64 " export=%" PRIu64 "\n",
        generation, phase, segment, exported);
    size_t written = 0;
    if (length < 0 || (size_t)length >= sizeof(line)) return -1;
    printf("obmm-worker %s", line);
    fflush(stdout);
    while (written < (size_t)length) {
        ssize_t n = write(fd, line + written, (size_t)length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return fsync(fd);
}

static int create_state(const char *path)
{
    char parent[1024];
    char *slash;
    int fd, directory;

    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return -1;
    if (slash == parent) slash[1] = 0;
    else *slash = 0;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0 || fsync(directory) != 0 ||
        record_phase(fd, 0, "worker-start", 0, 0) != 0) {
        if (directory >= 0) close(directory);
        close(fd);
        return -1;
    }
    close(directory);
    return fd;
}

static int allocate_work(int device, int journal,
    const struct worker_config *config, const struct mem_service_client *client,
    const struct obmm_cmd_gsva_aperture *aperture,
    const struct mem_service_client_allocation *work,
    struct worker_reservation *reservation, uint64_t *next_address)
{
    struct obmm_cmd_gsva_alloc_segment_v1 request = {0};
    struct obmm_cmd_export exported = {0};
    struct mem_service_provider_descriptor descriptor;
    struct mem_service_client_allocation published;
    enum mem_service_wire_status status;
    uint64_t alignment = work->alignment_bytes;
    uint64_t granularity = config->allocation_granularity_bytes;
    uint64_t backing_size;
    long page_size = sysconf(_SC_PAGESIZE);
    int export_result;

    if (strcmp(work->state, "allocating") || work->capabilities != MEM_SERVICE_MANAGED_CAP_MAP ||
        strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation || reservation->occupied ||
        !work->generation || !work->key[0] || page_size <= 0 ||
        work->provider_backed || work->descriptor_len || work->address || work->address_len ||
        work->holder_count || work->live_refs || !aperture->base || !aperture->size ||
        aperture->size > UINT64_MAX - aperture->base || *next_address < aperture->base ||
        *next_address - aperture->base > aperture->size) return WORKER_ALLOCATE_ERROR;
    if (!granularity || (granularity & (granularity - 1)) ||
        granularity < (uint64_t)page_size || !work->size_bytes)
        return WORKER_ALLOCATE_ERROR;
    if (work->size_bytes > UINT64_MAX - (granularity - 1))
        return WORKER_ALLOCATE_CAPACITY;
    backing_size = (work->size_bytes + granularity - 1) & ~(granularity - 1);
    if (alignment && (alignment & (alignment - 1))) return -1;
    if (alignment < granularity) alignment = granularity;
    if (*next_address > UINT64_MAX - (alignment - 1))
        return WORKER_ALLOCATE_CAPACITY;
    request.requested_home_va = (*next_address + alignment - 1) & ~(alignment - 1);
    if (request.requested_home_va < aperture->base ||
        request.requested_home_va - aperture->base > aperture->size ||
        backing_size > UINT64_MAX - request.requested_home_va ||
        backing_size > aperture->size - (request.requested_home_va - aperture->base))
        return WORKER_ALLOCATE_CAPACITY;
    if (record_phase(journal, work->generation, "reserve-intent", 0, 0)) return -1;
    reservation->occupied = true;
    strcpy(reservation->key, work->key);
    reservation->generation = work->generation;
    request.version = OBMM_GSVA_ABI_VERSION;
    request.size = backing_size;
    request.alignment = alignment;
    request.home_node_id = aperture->node_id;
    request.cache_policy = GSVA_CACHE_POLICY_WRITE_THROUGH;
    request.requested_p_tag = OBMM_GSVA_P_TAG_AUTO;
    request.access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE;
    if (gva_manager_allocate_segment(device, &request)) {
        reservation->segment = request.desc;
        (void)record_phase(journal, work->generation, "reserve-unknown",
                           request.desc.segment_id, 0);
        return -1;
    }
    reservation->segment = request.desc;
    *next_address = request.desc.home_va + request.desc.size;
    if (record_phase(journal, work->generation, "reserved", request.desc.segment_id, 0)) return -1;
    if (request.desc.home_va < aperture->base ||
        request.desc.home_va - aperture->base > aperture->size ||
        request.desc.size > aperture->size - (request.desc.home_va - aperture->base)) return -1;
    export_result = gva_manager_export_segment_checked(device, &request.desc,
                                                      config->fast_allocation, &exported);
    if (export_result) {
        reservation->exported = exported;
        if (export_result == 1) {
            reservation->export_no_backing = true;
            if (record_phase(journal, work->generation, "export-no-backing",
                             request.desc.segment_id, 0)) return -1;
            return WORKER_ALLOCATE_BACKING_EMPTY;
        }
        (void)record_phase(journal, work->generation, "export-unknown",
                           request.desc.segment_id, exported.mem_id);
        return -1;
    }
    reservation->exported = exported;
    if (record_phase(journal, work->generation, "exported",
                      request.desc.segment_id, exported.mem_id) ||
        mem_service_provider_obmm_encode_gsva(&request.desc, &exported, &descriptor)) return -1;
    reservation->descriptor = descriptor;
    if (mem_service_client_publish_allocation(client, work->key, config->node,
            config->incarnation, work->generation, descriptor.bytes, descriptor.len,
            request.desc.home_va, request.desc.size, &published, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK ||
        (strcmp(published.state, "active") && strcmp(published.state, "retiring")) ||
        published.generation != work->generation ||
        published.address != request.desc.home_va || published.address_len != request.desc.size ||
        published.descriptor_len != descriptor.len ||
        memcmp(published.descriptor, descriptor.bytes, descriptor.len)) return -1;
    if (record_phase(journal, work->generation, "published",
                      request.desc.segment_id, exported.mem_id)) return -1;
    printf("obmm-worker generation=%" PRIu64 " state=%s address=%#" PRIx64
           " size=%" PRIu64 "\n", work->generation, published.state,
           (uint64_t)request.desc.home_va, (uint64_t)request.desc.size);
    fflush(stdout);
    return 0;
}

static int reclaim_work(int device, int journal,
    const struct worker_config *config, const struct mem_service_client *client,
    const struct mem_service_client_allocation *work,
    struct worker_reservation *reservation)
{
    struct obmm_cmd_gsva_retire_segment_v1 retire = {0};
    struct mem_service_client_allocation reclaimed;
    enum mem_service_wire_status status;

    if (!reservation->occupied || strcmp(work->state, "retiring") ||
        strcmp(work->key, reservation->key) || work->generation != reservation->generation ||
        strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation ||
        work->live_refs || work->holder_count || !work->provider_backed ||
        work->address != reservation->segment.home_va ||
        work->address_len != reservation->segment.size ||
        work->descriptor_len != reservation->descriptor.len ||
        !work->descriptor_len || work->descriptor_len > sizeof(work->descriptor) ||
        memcmp(work->descriptor, reservation->descriptor.bytes, work->descriptor_len)) return -1;
    if (record_phase(journal, work->generation, "release-intent",
                     reservation->segment.segment_id, reservation->exported.mem_id) ||
        gva_manager_unexport_segment(device, &reservation->segment, &reservation->exported) ||
        record_phase(journal, work->generation, "unexported",
                     reservation->segment.segment_id, reservation->exported.mem_id)) return -1;
    retire.version = OBMM_GSVA_ABI_VERSION;
    retire.segment_id = reservation->segment.segment_id;
    retire.epoch = reservation->segment.epoch;
    retire.timeout_ms = 5000;
    if (gva_manager_retire_segment(device, &retire) ||
        record_phase(journal, work->generation, "retired", retire.segment_id, 0)) return -1;
    if (mem_service_client_reclaim_allocation(client, work->key, config->node,
            config->incarnation, work->generation, true, &reclaimed, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK || strcmp(reclaimed.state, "retired") ||
        reclaimed.generation != work->generation ||
        record_phase(journal, work->generation, "reclaimed", retire.segment_id, 0)) return -1;
    printf("obmm-worker generation=%" PRIu64 " state=retired size=%" PRIu64 "\n",
           work->generation, (uint64_t)reservation->segment.size);
    fflush(stdout);
    memset(reservation, 0, sizeof(*reservation));
    return 0;
}

static int cancel_unreserved_work(int journal, const struct worker_config *config,
    const struct mem_service_client *client, const struct mem_service_client_allocation *work)
{
    struct mem_service_client_allocation reclaimed;
    enum mem_service_wire_status status;
    if (strcmp(work->state, "retiring") || work->provider_backed ||
        work->holder_count || work->live_refs || work->descriptor_len ||
        work->address || work->address_len || !work->generation ||
        strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation) return -1;
    if (record_phase(journal, work->generation, "cancel-empty", 0, 0) ||
        mem_service_client_reclaim_allocation(client, work->key, config->node,
            config->incarnation, work->generation, true, &reclaimed, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK || strcmp(reclaimed.state, "retired") ||
        reclaimed.generation != work->generation ||
        record_phase(journal, work->generation, "cancel-confirmed", 0, 0)) return -1;
    return 0;
}

static int cancel_unbacked_work(int journal, const struct worker_config *config,
    const struct mem_service_client *client, const struct mem_service_client_allocation *work,
    const char *reason)
{
    struct mem_service_client_allocation cancelled;
    enum mem_service_wire_status status;
    char operation[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    int length;

    /* Requires no resource-creating attempt, or a NO_BACKING receipt followed
     * by confirmed segment retirement. An errno alone never authorizes this. */
    if (strcmp(work->state, "allocating") || work->provider_backed ||
        work->descriptor_len || work->address || work->address_len ||
        work->holder_count || work->live_refs || !work->generation ||
        strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation) return -1;
    length = snprintf(operation, sizeof(operation), "obmm-capacity-%" PRIu64 "-%" PRIu64,
                      config->incarnation, work->generation);
    if (length < 0 || (size_t)length >= sizeof(operation) ||
        record_phase(journal, work->generation, "capacity-reject-intent", 0, 0) ||
        mem_service_client_retire_object(client, work->key, operation, true,
                                        work->generation, &cancelled, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK ||
        strcmp(cancelled.key, work->key) || cancelled.generation != work->generation ||
        cancel_unreserved_work(journal, config, client, &cancelled)) return -1;
    printf("obmm-worker generation=%" PRIu64 " state=retired reason=%s "
           "size=%" PRIu64 " alignment=%" PRIu64 "\n",
           work->generation, reason, work->size_bytes, work->alignment_bytes);
    fflush(stdout);
    return 0;
}

static int reject_capacity_work(int journal, const struct worker_config *config,
    const struct mem_service_client *client, const struct mem_service_client_allocation *work)
{
    return cancel_unbacked_work(journal, config, client, work, "address_capacity");
}

static int rollback_unbacked_work(int device, int journal, const struct worker_config *config,
    const struct mem_service_client *client, const struct mem_service_client_allocation *work,
    struct worker_reservation *reservation)
{
    struct obmm_cmd_gsva_retire_segment_v1 retire = {0};

    if (strcmp(work->state, "allocating") || strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation ||
        !reservation->occupied || !reservation->export_no_backing ||
        strcmp(reservation->key, work->key) || reservation->generation != work->generation ||
        reservation->descriptor.len || reservation->exported.mem_id ||
        reservation->exported.tokenid || reservation->exported.uba ||
        reservation->exported.size[0]) return -1;
    retire.version = OBMM_GSVA_ABI_VERSION;
    retire.segment_id = reservation->segment.segment_id;
    retire.epoch = reservation->segment.epoch;
    retire.timeout_ms = 5000;
    if (record_phase(journal, work->generation, "unbacked-retire-intent", retire.segment_id, 0) ||
        gva_manager_retire_segment(device, &retire) ||
        record_phase(journal, work->generation, "unbacked-retired", retire.segment_id, 0) ||
        cancel_unbacked_work(journal, config, client, work, "backing_allocation")) return -1;
    memset(reservation, 0, sizeof(*reservation));
    return 0;
}

int mem_service_provider_obmm_serve_allocations(const char *config_path)
{
    struct worker_config config;
    struct mem_service_client client;
    struct mem_service_client_provider_directory directory;
    struct mem_service_client_allocation work;
    struct obmm_cmd_gsva_aperture aperture = {0};
    struct sigaction action = {0}, old_int, old_term;
    const struct timespec interval = {.tv_nsec = 100000000};
    enum mem_service_wire_status status;
    int device = -1, journal = -1, result = 1;
    bool signals = false, refreshed = false;
    struct worker_reservation *reservations = NULL;
    uint64_t next_address;

    if (read_config(config_path, &config)) {
        fprintf(stderr, "obmm-worker invalid config\n");
        return 2;
    }
    if (access(config.state, F_OK) == 0 || errno != ENOENT) {
        fprintf(stderr, "obmm-worker reconciliation required state=%s\n", config.state);
        return 1;
    }
    mem_service_client_init(&client, config.connect);
    device = open("/dev/obmm", O_RDWR | O_CLOEXEC);
    if (device < 0 || ioctl(device, OBMM_CMD_GSVA_APERTURE_QUERY, &aperture) ||
        !(aperture.flags & OBMM_GSVA_APERTURE_F_ACTIVE)) goto done;
    next_address = aperture.base;
    reservations = calloc(MEM_SERVICE_MANAGED_MAX_ALLOCATIONS, sizeof(*reservations));
    if (!reservations) goto done;
    journal = create_state(config.state);
    if (journal < 0) goto done;
    printf("obmm-worker address_reuse=disabled home_policy=single-owner "
           "forced_revoke=unsupported recovery=quarantine\n");
    printf("obmm-worker starting provider refresh node=%s\n", config.node);
    fflush(stdout);
    worker_stop = 0;
    action.sa_handler = stop_worker;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int)) goto done;
    if (sigaction(SIGTERM, &action, &old_term)) {
        sigaction(SIGINT, &old_int, NULL);
        goto done;
    }
    signals = true;
    while (!worker_stop) {
        int poll_result;
        struct worker_reservation *owned = NULL, *available = NULL;
        size_t i;
        if (mem_service_client_provider_refresh(&client, config.node,
                config.incarnation, config.readiness_generation, &directory, &status) ||
            status != MEM_SERVICE_WIRE_STATUS_OK) goto done;
        if (!refreshed) {
            printf("obmm-worker initial refresh provider_directory_ready=%u\n",
                   directory.directory_ready ? 1U : 0U);
            fflush(stdout);
        }
        refreshed = true;
        /* Refresh reports directory readiness, not the daemon's optional
         * in-process data plane. Backing operations belong to this worker. */
        if (!directory.directory_ready) {
            nanosleep(&interval, NULL);
            continue;
        }
        poll_result = mem_service_client_poll_allocation(&client, config.node,
                config.incarnation, 0, &work, &status);
        if (worker_stop) break;
        if (poll_result >= 0 && status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            nanosleep(&interval, NULL);
            continue;
        }
        if (poll_result || status != MEM_SERVICE_WIRE_STATUS_OK) goto done;
        printf("obmm-worker work key=%s generation=%" PRIu64 " state=%s\n",
               work.key, work.generation, work.state);
        fflush(stdout);
        for (i = 0; i < MEM_SERVICE_MANAGED_MAX_ALLOCATIONS; i++) {
            if (!reservations[i].occupied) {
                if (!available) available = &reservations[i];
            } else if (reservations[i].generation == work.generation &&
                       !strcmp(reservations[i].key, work.key)) {
                owned = &reservations[i];
            }
        }
        if (!strcmp(work.state, "allocating")) {
            int allocation_result;
            if (owned || !available) goto done;
            allocation_result = allocate_work(device, journal, &config,
                    &client, &aperture, &work, available, &next_address);
            if (allocation_result == WORKER_ALLOCATE_CAPACITY) {
                if (available->occupied || reject_capacity_work(journal, &config, &client, &work))
                    goto done;
            } else if (allocation_result == WORKER_ALLOCATE_BACKING_EMPTY) {
                if (rollback_unbacked_work(device, journal, &config, &client, &work, available))
                    goto done;
            } else if (allocation_result != WORKER_ALLOCATE_OK) goto done;
        } else if (!strcmp(work.state, "retiring")) {
            if (owned ? reclaim_work(device, journal, &config, &client, &work, owned)
                      : cancel_unreserved_work(journal, &config, &client, &work)) goto done;
        } else goto done;
    }
    result = 0;
done:
    if (refreshed &&
        (mem_service_client_provider_deregister(&client, config.node, config.incarnation,
                                               &directory, &status) ||
         status != MEM_SERVICE_WIRE_STATUS_OK)) result = 1;
    if (signals) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
    }
    if (journal >= 0) close(journal);
    if (device >= 0) close(device);
    free(reservations);
    fprintf(stderr, "obmm-worker stopped result=%d reconciliation_required=%d\n",
            result, journal >= 0);
    return result;
}
