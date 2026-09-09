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
        "connect", "node_id", "state_file", "incarnation", "readiness_generation"
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
            if (errno || *end || !number) goto done;
            if (i == 3) config->incarnation = number;
            else config->readiness_generation = number;
        }
    }
    if (!ferror(file) && seen == 31U && config->state[0] == '/') result = 0;
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
    const struct mem_service_client_allocation *work)
{
    struct obmm_cmd_gsva_alloc_segment_v1 request = {0};
    struct obmm_cmd_export exported = {0};
    struct mem_service_provider_descriptor descriptor;
    struct mem_service_client_allocation published;
    enum mem_service_wire_status status;

    if (strcmp(work->state, "allocating") || work->capabilities != MEM_SERVICE_MANAGED_CAP_MAP ||
        strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation) return -1;
    if (record_phase(journal, work->generation, "reserve-intent", 0, 0)) return -1;
    request.version = OBMM_GSVA_ABI_VERSION;
    request.size = work->size_bytes;
    request.alignment = work->alignment_bytes;
    request.home_node_id = aperture->node_id;
    request.cache_policy = GSVA_CACHE_POLICY_WRITE_THROUGH;
    request.requested_p_tag = OBMM_GSVA_P_TAG_AUTO;
    request.access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE;
    if (gva_manager_allocate_segment(device, &request)) {
        (void)record_phase(journal, work->generation, "reserve-unknown",
                           request.desc.segment_id, 0);
        return -1;
    }
    if (record_phase(journal, work->generation, "reserved", request.desc.segment_id, 0)) return -1;
    if (request.desc.home_va < aperture->base ||
        request.desc.home_va - aperture->base > aperture->size ||
        request.desc.size > aperture->size - (request.desc.home_va - aperture->base)) return -1;
    if (gva_manager_export_segment(device, &request.desc, &exported)) {
        (void)record_phase(journal, work->generation, "export-unknown",
                           request.desc.segment_id, exported.mem_id);
        return -1;
    }
    if (record_phase(journal, work->generation, "exported",
                      request.desc.segment_id, exported.mem_id) ||
        mem_service_provider_obmm_encode_gsva(&request.desc, &exported, &descriptor)) return -1;
    if (mem_service_client_publish_allocation(client, work->key, config->node,
            config->incarnation, work->generation, descriptor.bytes, descriptor.len,
            request.desc.home_va, request.desc.size, &published, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK || strcmp(published.state, "active") ||
        published.generation != work->generation ||
        published.address != request.desc.home_va || published.address_len != request.desc.size ||
        published.descriptor_len != descriptor.len ||
        memcmp(published.descriptor, descriptor.bytes, descriptor.len)) return -1;
    if (record_phase(journal, work->generation, "published",
                      request.desc.segment_id, exported.mem_id)) return -1;
    printf("obmm-worker generation=%" PRIu64 " state=active address=%#" PRIx64
           " size=%" PRIu64 "\n", work->generation,
           (uint64_t)request.desc.home_va, (uint64_t)request.desc.size);
    fflush(stdout);
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
    journal = create_state(config.state);
    if (journal < 0) goto done;
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
        if (mem_service_client_provider_refresh(&client, config.node,
                config.incarnation, config.readiness_generation, &directory, &status) ||
            status != MEM_SERVICE_WIRE_STATUS_OK) goto done;
        refreshed = true;
        if (!directory.data_plane_ready) {
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
        if (allocate_work(device, journal, &config, &client, &aperture, &work)) goto done;
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
    fprintf(stderr, "obmm-worker stopped result=%d reconciliation_required=%d\n",
            result, journal >= 0);
    return result;
}
