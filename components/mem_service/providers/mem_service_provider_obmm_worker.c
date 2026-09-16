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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mem_service_provider_obmm_worker_ledger.h"

enum worker_allocate_result {
    WORKER_ALLOCATE_ERROR = -1,
    WORKER_ALLOCATE_OK = 0,
    /* No segment: preflight rejection or the platform's ENOSPC contract. */
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

static int record_phase(int fd, const struct worker_config *config,
                         const struct mem_service_client_allocation *work,
                         const struct worker_reservation *reservation,
                         uint64_t generation, const char *phase,
                         uint64_t segment, uint64_t exported)
{
    struct worker_ledger_entry entry = {0};
    unsigned char frame[WORKER_LEDGER_FRAME_BYTES];
    off_t end = lseek(fd, 0, SEEK_END);
    size_t written = 0;
    if (!config || !phase || strlen(phase) >= sizeof(entry.phase) ||
        end < 0 || end % WORKER_LEDGER_FRAME_BYTES ||
        (work && work->generation != generation)) return -1;
    entry.sequence = (uint64_t)end / WORKER_LEDGER_FRAME_BYTES + 1;
    entry.config = *config;
    if (work) entry.work = *work;
    if (reservation) entry.reservation = *reservation;
    strcpy(entry.phase, phase);
    if (ledger_encode(frame, &entry)) return -1;
    while (written < sizeof(frame)) {
        ssize_t n = write(fd, frame + written, sizeof(frame) - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    if (fsync(fd)) return -1;
    printf("obmm-worker generation=%" PRIu64 " phase=%s segment=%" PRIu64
           " export=%" PRIu64 "\n", generation, phase, segment, exported);
    fflush(stdout);
    return 0;
}

static int create_state(const struct worker_config *config)
{
    const char *path = config->state;
    char parent[1024];
    char *slash;
    int fd, directory;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};

    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return -1;
    if (slash == parent) slash[1] = 0;
    else *slash = 0;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETLK, &lock)) { close(fd); return -1; }
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0 || fsync(directory) != 0 ||
        record_phase(fd, config, NULL, NULL, 0, "worker-start", 0, 0) != 0) {
        if (directory >= 0) close(directory);
        close(fd);
        return -1;
    }
    close(directory);
    return fd;
}

static int ledger_read_entry(int fd, uint64_t sequence,
    struct worker_config *config, struct worker_ledger_entry *entry)
{
    static const char *phases[] = {
        "worker-start", "reserve-intent", "reserve-empty", "reserve-unknown",
        "reserved", "export-no-backing", "export-unknown", "exported", "published",
        "release-intent", "unexported", "retired", "reclaimed", "cancel-empty",
        "cancel-confirmed", "capacity-reject-intent", "unbacked-retire-intent",
        "unbacked-retired"
    };
    unsigned char frame[WORKER_LEDGER_FRAME_BYTES];
    size_t received = 0, i;
    while (received < sizeof(frame)) {
        ssize_t n = read(fd, frame + received, sizeof(frame) - received);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        received += (size_t)n;
    }
    if (ledger_decode(frame, entry) || entry->sequence != sequence ||
        strcmp(entry->config.node, config->node) ||
        entry->config.incarnation != config->incarnation ||
        entry->config.readiness_generation != config->readiness_generation ||
        entry->config.allocation_granularity_bytes != config->allocation_granularity_bytes)
        return -1;
    for (i = 0; i < sizeof(phases) / sizeof(phases[0]); i++)
        if (!strcmp(entry->phase, phases[i])) break;
    if (i == sizeof(phases) / sizeof(phases[0])) return -1;
    if (sequence == 1) {
        if (i != 0 || entry->work.generation || entry->work.key[0] ||
            entry->reservation.segment.segment_id || entry->reservation.exported.mem_id)
            return -1;
        memcpy(config->kernel_instance, entry->config.kernel_instance, 16);
    } else if (!i || !entry->work.key[0] || !entry->work.generation ||
               !entry->work.size_bytes || entry->work.capabilities != MEM_SERVICE_MANAGED_CAP_MAP ||
               memcmp(config->kernel_instance, entry->config.kernel_instance, 16))
        return -1;
    return 0;
}

int mem_service_provider_obmm_inspect_allocation_state(const char *config_path)
{
    struct worker_config config;
    struct worker_ledger_entry entry;
    struct stat before, after;
    struct flock lock = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
    uint64_t count, sequence;
    int fd, pass, result = 1;

    if (read_config(config_path, &config)) return 2;
    fd = open(config.state, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) goto done;
    if (fcntl(fd, F_SETLK, &lock) || fstat(fd, &before) || !S_ISREG(before.st_mode) ||
        before.st_size <= 0 || before.st_size % WORKER_LEDGER_FRAME_BYTES) goto done;
    count = (uint64_t)before.st_size / WORKER_LEDGER_FRAME_BYTES;
    /* Validate the entire file before emitting any resource inventory. */
    for (pass = 0; pass < 2; pass++) {
        if (lseek(fd, 0, SEEK_SET) != 0) goto done;
        for (sequence = 1; sequence <= count; sequence++) {
            size_t i;
            if (ledger_read_entry(fd, sequence, &config, &entry)) goto done;
            if (!pass) continue;
            printf("obmm-worker-ledger: sequence=%" PRIu64 " phase=%s node=%s "
                   "incarnation=%" PRIu64 " key=%s generation=%" PRIu64
                   " logical_bytes=%" PRIu64 " alignment=%" PRIu64
                   " segment=%" PRIu64 " epoch=%" PRIu64 " address=%" PRIu64
                   " bytes=%" PRIu64 " export=%" PRIu64 " export_token=%u descriptor_hex=",
                   sequence, entry.phase, entry.config.node, entry.config.incarnation,
                   entry.work.key[0] ? entry.work.key : "-", entry.work.generation,
                   entry.work.size_bytes, entry.work.alignment_bytes,
                   (uint64_t)entry.reservation.segment.segment_id,
                   (uint64_t)entry.reservation.segment.epoch,
                   (uint64_t)entry.reservation.segment.home_va,
                   (uint64_t)entry.reservation.segment.size,
                   (uint64_t)entry.reservation.exported.mem_id,
                   entry.reservation.exported.tokenid);
            for (i = 0; i < entry.reservation.descriptor.len; i++)
                printf("%02x", entry.reservation.descriptor.bytes[i]);
            putchar('\n');
        }
        if (fstat(fd, &after) || before.st_size != after.st_size ||
            before.st_mtime != after.st_mtime || before.st_ctime != after.st_ctime) goto done;
    }
    if (ferror(stdout)) goto done;
    printf("obmm-worker-ledger: status=ok version=2 entries=%" PRIu64
           " physical_state=unknown reconciliation_required=1 kernel_instance=", count);
    for (pass = 0; pass < 16; pass++) printf("%02x", config.kernel_instance[pass]);
    putchar('\n');
    result = 0;
done:
    if (fd >= 0) close(fd);
    if (result) fprintf(stderr, "obmm-worker-ledger: status=failed physical_state=unknown "
                        "reconciliation_required=1\n");
    return result;
}

static int ledger_order(const void *left, const void *right)
{
    const struct worker_ledger_entry *a = left, *b = right;
    if (a->work.generation != b->work.generation)
        return a->work.generation < b->work.generation ? -1 : 1;
    return a->sequence < b->sequence ? -1 : a->sequence != b->sequence;
}

struct worker_recovery_object {
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    char phase[32];
    uint64_t generation;
    bool recovered;
};

static int worker_u64_order(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return a < b ? -1 : a != b;
}

static int worker_recovery_archive_path(const struct worker_config *config,
                                        const struct worker_config *replacement,
                                        char *path,
                                        size_t path_len)
{
    int length;

    if (!config || !replacement || !path || !path_len) return -1;
    length = snprintf(path, path_len,
                      "%s.recovered-%" PRIu64 "-by-%" PRIu64,
                      config->state, config->incarnation,
                      replacement->incarnation);
    return length < 0 || (size_t)length >= path_len ? -1 : 0;
}

static int worker_sync_state_parent(const char *path)
{
    char parent[1200];
    char *slash;
    int directory;

    if (!path || strlen(path) >= sizeof(parent)) return -1;
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return -1;
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return -1;
    if (fsync(directory)) {
        close(directory);
        return -1;
    }
    return close(directory);
}

static bool worker_replacement_config_valid(
    const struct worker_config *old_config,
    const struct worker_config *replacement)
{
    return old_config && replacement &&
           !strcmp(old_config->connect, replacement->connect) &&
           !strcmp(old_config->node, replacement->node) &&
           !strcmp(old_config->state, replacement->state) &&
           old_config->allocation_granularity_bytes ==
               replacement->allocation_granularity_bytes &&
           old_config->fast_allocation == replacement->fast_allocation &&
           old_config->incarnation != replacement->incarnation;
}

/* Matching is evidence only: it never authorizes release, publish or reuse. */
static const char *reconcile_resource(const struct worker_ledger_entry *last,
    const struct worker_reservation *saved, const struct obmm_cmd_gsva_enumerate_v1 *actual)
{
    struct obmm_gsva_segment_desc_v1 expected = saved->segment;
    struct mem_service_provider_descriptor encoded;
    bool retired = !strcmp(last->phase, "retired") || !strcmp(last->phase, "reclaimed") ||
        !strcmp(last->phase, "unbacked-retired") || !strcmp(last->phase, "cancel-confirmed");
    bool exported = !strcmp(last->phase, "exported") || !strcmp(last->phase, "published");
    bool reserved = !strcmp(last->phase, "reserved") ||
        !strcmp(last->phase, "export-no-backing") || !strcmp(last->phase, "unexported");
    if (!actual || (!retired && !exported && !reserved)) return NULL;
    if (retired) expected.flags = (expected.flags & ~OBMM_GSVA_SEG_F_ACTIVE) |
                                  OBMM_GSVA_SEG_F_RETIRED;
    if (memcmp(&expected, &actual->desc, sizeof(expected)) ||
        (actual->resource_flags & OBMM_GSVA_RESOURCE_EXPORT_BUSY)) return NULL;
    if (retired)
        return !actual->resource_flags && !actual->export_mem_id ? "retired-record" : NULL;
    if (actual->resource_flags != OBMM_GSVA_RESOURCE_ADDRESS_RESERVED) return NULL;
    if (reserved) return !actual->export_mem_id ? "reserved-no-export" : NULL;
    if (!saved->exported.mem_id || actual->export_mem_id != saved->exported.mem_id ||
        actual->export_token_id != saved->exported.tokenid ||
        mem_service_provider_obmm_encode_gsva(&saved->segment, &saved->exported, &encoded) ||
        encoded.len != saved->descriptor.len ||
        memcmp(encoded.bytes, saved->descriptor.bytes, encoded.len)) return NULL;
    return "export-bound";
}

int mem_service_provider_obmm_reconcile_allocation_state(const char *config_path)
{
    enum { limit = 65536 };
    struct worker_config config;
    struct worker_ledger_entry *entries = NULL;
    struct obmm_cmd_gsva_enumerate_v1 *inventory = NULL;
    struct obmm_cmd_gsva_enumerate_v1 cursor = {.version = OBMM_GSVA_ABI_VERSION};
    struct stat before, after;
    struct flock lock = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
    unsigned char *claimed = NULL;
    const char **matches = NULL;
    const char *reason = "journal-invalid";
    size_t count = 0, records = 0, groups = 0, unclaimed = 0, i, j;
    int fd = -1, device = -1, result = 1;

    if (read_config(config_path, &config)) return 2;
    fd = open(config.state, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fcntl(fd, F_SETLK, &lock) || fstat(fd, &before) ||
        !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        before.st_size % WORKER_LEDGER_FRAME_BYTES ||
        before.st_size / WORKER_LEDGER_FRAME_BYTES > limit) goto done;
    count = (size_t)before.st_size / WORKER_LEDGER_FRAME_BYTES;
    entries = calloc(count, sizeof(*entries));
    inventory = calloc(limit, sizeof(*inventory));
    claimed = calloc(limit, 1);
    matches = calloc(count, sizeof(*matches));
    if (!entries || !inventory || !claimed || !matches) goto done;
    for (i = 0; i < count; i++)
        if (ledger_read_entry(fd, i + 1, &config, &entries[i])) goto done;
    device = open("/dev/obmm", O_RDONLY | O_CLOEXEC);
    reason = "inventory-unavailable";
    if (device < 0) goto done;
    for (;;) {
        if (gva_manager_enumerate_segments(device, &cursor)) goto done;
        reason = "kernel-instance-mismatch";
        if (memcmp(config.kernel_instance, cursor.kernel_instance, 16)) goto done;
        reason = "inventory-unavailable";
        if (cursor.flags == OBMM_GSVA_ENUM_END) break;
        if (records == limit) goto done;
        inventory[records++] = cursor;
    }
    qsort(entries, count, sizeof(*entries), ledger_order);
    reason = "resource-unresolved";
    for (i = 1; i < count;) {
        struct worker_reservation saved = {0};
        size_t begin = i, found = records;
        const struct worker_ledger_entry *last;
        while (i < count && entries[i].work.generation == entries[begin].work.generation) {
            const struct worker_ledger_entry *e = &entries[i];
            if (strcmp(e->work.key, entries[begin].work.key) ||
                e->work.size_bytes != entries[begin].work.size_bytes ||
                e->work.alignment_bytes != entries[begin].work.alignment_bytes) goto done;
            if (e->reservation.segment.segment_id) {
                if (saved.segment.segment_id &&
                    memcmp(&saved.segment, &e->reservation.segment, sizeof(saved.segment))) goto done;
                saved = e->reservation;
            }
            i++;
        }
        last = &entries[i - 1];
        if (!saved.segment.segment_id) {
            if (strcmp(last->phase, "cancel-confirmed") &&
                strcmp(last->phase, "reserve-empty") &&
                strcmp(last->phase, "capacity-reject-intent") &&
                strcmp(last->phase, "cancel-empty")) goto done;
            matches[i - 1] = "no-recorded-segment";
        } else {
            for (j = 0; j < records; j++)
                if (inventory[j].desc.segment_id == saved.segment.segment_id) {
                    if (found != records) goto done;
                    found = j;
                }
            if (found == records || claimed[found]) goto done;
            matches[i - 1] = reconcile_resource(last, &saved, &inventory[found]);
            if (!matches[i - 1]) goto done;
            claimed[found] = 1;
        }
        groups++;
    }
    /* Check both sources once more before publishing any matching result. */
    reason = "snapshot-changed";
    if (gva_manager_enumerate_segments(device, &cursor) ||
        cursor.flags != OBMM_GSVA_ENUM_END || fstat(fd, &after) ||
        before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) goto done;
    for (j = 0; j < records; j++) if (!claimed[j]) unclaimed++;
    for (i = 1; i < count; i++) if (matches[i])
        printf("obmm-worker-reconcile: key=%s generation=%" PRIu64
               " phase=%s match=%s\n", entries[i].work.key, entries[i].work.generation,
               entries[i].phase, matches[i]);
    printf("obmm-worker-reconcile: status=matched objects=%zu kernel_records=%zu "
           "unclaimed_records=%zu revision=%" PRIu64 " kernel_instance=",
           groups, records, unclaimed, (uint64_t)cursor.revision);
    for (i = 0; i < 16; i++) printf("%02x", config.kernel_instance[i]);
    printf(" scope=segment-export-records fencing_verified=0 reconciliation_required=1\n");
    if (!ferror(stdout)) result = 0;
done:
    if (device >= 0) close(device);
    if (fd >= 0) close(fd);
    free(entries); free(inventory); free(claimed); free(matches);
    if (result) fprintf(stderr, "obmm-worker-reconcile: status=failed reason=%s "
                        "fencing_verified=0 reconciliation_required=1\n", reason);
    return result;
}

int mem_service_provider_obmm_recover_allocation_state(
    const char *config_path,
    const char *replacement_config_path)
{
    enum { limit = 65536 };
    struct worker_config old_config, replacement;
    struct worker_ledger_entry *entries = NULL;
    struct worker_recovery_object *objects = NULL;
    uint64_t *old_segments = NULL;
    struct mem_service_client client;
    struct mem_service_client_provider_directory directory;
    struct mem_service_client_allocation work, recovered;
    struct obmm_cmd_gsva_enumerate_v1 cursor = {
        .version = OBMM_GSVA_ABI_VERSION,
    };
    struct stat before, after, path_stat;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    enum mem_service_wire_status status;
    unsigned char current_kernel_instance[16] = {0};
    char archive[1200];
    const char *ledger_path;
    const char *reason = "invalid-config";
    size_t count, object_count = 0, segment_count = 0;
    size_t inventory_count = 0, recovered_count = 0, i, j;
    uint64_t after_generation = 0, revision = 0;
    int fd = -1, device = -1, result = 1;
    bool state_exists, archive_exists, already_archived;
    bool current_kernel_seen = false;

    if (read_config(config_path, &old_config) ||
        read_config(replacement_config_path, &replacement)) return 2;
    if (!worker_replacement_config_valid(&old_config, &replacement) ||
        worker_recovery_archive_path(&old_config, &replacement,
                                     archive, sizeof(archive))) return 2;
    state_exists = lstat(old_config.state, &path_stat) == 0;
    if (!state_exists && errno != ENOENT) goto done;
    archive_exists = lstat(archive, &path_stat) == 0;
    if (!archive_exists && errno != ENOENT) goto done;
    reason = "ledger-location-conflict";
    if (state_exists == archive_exists) goto done;
    already_archived = archive_exists;
    ledger_path = already_archived ? archive : old_config.state;
    fd = open(ledger_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    reason = "journal-invalid";
    if (fd < 0 || fcntl(fd, F_SETLK, &lock) || fstat(fd, &before) ||
        !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        before.st_size % WORKER_LEDGER_FRAME_BYTES ||
        before.st_size / WORKER_LEDGER_FRAME_BYTES > limit) goto done;
    count = (size_t)before.st_size / WORKER_LEDGER_FRAME_BYTES;
    entries = calloc(count, sizeof(*entries));
    objects = calloc(count, sizeof(*objects));
    old_segments = calloc(count, sizeof(*old_segments));
    if (!entries || !objects || !old_segments) goto done;
    for (i = 0; i < count; ++i)
        if (ledger_read_entry(fd, i + 1, &old_config, &entries[i])) goto done;
    qsort(entries, count, sizeof(*entries), ledger_order);
    if (strcmp(entries[0].phase, "worker-start") || entries[0].work.generation)
        goto done;
    for (i = 1; i < count;) {
        size_t begin = i;

        if (!entries[begin].work.key[0] || !entries[begin].work.generation)
            goto done;
        while (i < count &&
               entries[i].work.generation == entries[begin].work.generation) {
            if (strcmp(entries[i].work.key, entries[begin].work.key) ||
                entries[i].work.size_bytes != entries[begin].work.size_bytes ||
                entries[i].work.alignment_bytes !=
                    entries[begin].work.alignment_bytes ||
                entries[i].work.capabilities != entries[begin].work.capabilities)
                goto done;
            if (entries[i].reservation.segment.segment_id)
                old_segments[segment_count++] =
                    entries[i].reservation.segment.segment_id;
            ++i;
        }
        strcpy(objects[object_count].key, entries[begin].work.key);
        strcpy(objects[object_count].phase, entries[i - 1].phase);
        objects[object_count].generation = entries[begin].work.generation;
        ++object_count;
    }
    qsort(old_segments, segment_count, sizeof(*old_segments), worker_u64_order);

    reason = "inventory-unavailable";
    device = open("/dev/obmm", O_RDONLY | O_CLOEXEC);
    if (device < 0) goto done;
    for (;;) {
        if (gva_manager_enumerate_segments(device, &cursor)) goto done;
        reason = "kernel-instance-unchanged";
        if (!memcmp(old_config.kernel_instance, cursor.kernel_instance, 16))
            goto done;
        reason = "inventory-inconsistent";
        if (!current_kernel_seen) {
            memcpy(current_kernel_instance, cursor.kernel_instance, 16);
            current_kernel_seen = true;
        } else if (memcmp(current_kernel_instance,
                          cursor.kernel_instance, 16)) {
            goto done;
        }
        if (cursor.flags == OBMM_GSVA_ENUM_END) {
            revision = cursor.revision;
            break;
        }
        if (cursor.flags != OBMM_GSVA_ENUM_ENTRY || inventory_count++ == limit)
            goto done;
        reason = "old-segment-identity-reused";
        if (bsearch(&cursor.desc.segment_id, old_segments, segment_count,
                    sizeof(*old_segments), worker_u64_order)) goto done;
    }
    reason = "snapshot-changed";
    if (gva_manager_enumerate_segments(device, &cursor) ||
        cursor.flags != OBMM_GSVA_ENUM_END || cursor.revision != revision ||
        memcmp(current_kernel_instance, cursor.kernel_instance, 16)) goto done;

    mem_service_client_init(&client, replacement.connect);
    reason = "replacement-provider-not-ready";
    if (mem_service_client_provider_refresh(
            &client, replacement.node, replacement.incarnation,
            replacement.readiness_generation, &directory, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK || !directory.directory_ready)
        goto done;
    for (;;) {
        int poll_result = mem_service_client_poll_recovery_allocation(
            &client, replacement.node, replacement.incarnation,
            old_config.incarnation, after_generation, &work, &status);

        if (poll_result >= 0 && status == MEM_SERVICE_WIRE_STATUS_NOT_FOUND)
            break;
        reason = "recovery-poll-failed";
        if (poll_result || status != MEM_SERVICE_WIRE_STATUS_OK ||
            strcmp(work.state, "quarantined") ||
            strcmp(work.home_node, old_config.node) ||
            work.provider_incarnation != old_config.incarnation ||
            work.generation <= after_generation) goto done;
        for (j = 0; j < object_count; ++j) {
            if (objects[j].generation != work.generation) continue;
            if (strcmp(objects[j].key, work.key)) {
                reason = "ledger-generation-conflict";
                goto done;
            }
            objects[j].recovered = true;
            break;
        }
        reason = "recovery-reclaim-failed";
        if (mem_service_client_recover_allocation(
                &client, work.key, replacement.node,
                replacement.incarnation, work.generation,
                old_config.incarnation, true, &recovered, &status) ||
            status != MEM_SERVICE_WIRE_STATUS_OK ||
            strcmp(recovered.key, work.key) ||
            recovered.generation != work.generation ||
            strcmp(recovered.state, "retired") ||
            strcmp(recovered.home_node, old_config.node) ||
            recovered.provider_incarnation != old_config.incarnation ||
            recovered.provider_backed || recovered.holder_count ||
            recovered.live_refs || recovered.descriptor_len ||
            recovered.address || recovered.address_len) goto done;
        after_generation = work.generation;
        ++recovered_count;
    }
    for (i = 0; i < object_count; ++i) {
        bool superseded = false;

        if (objects[i].recovered) continue;
        for (j = 0; j < object_count; ++j) {
            if (!strcmp(objects[i].key, objects[j].key) &&
                objects[j].generation > objects[i].generation) {
                superseded = true;
                break;
            }
        }
        if (superseded &&
            (!strcmp(objects[i].phase, "reclaimed") ||
             !strcmp(objects[i].phase, "cancel-confirmed"))) {
            continue;
        }
        reason = "ledger-object-not-retired";
        if (mem_service_client_inspect_allocation(
                &client, objects[i].key, &work, &status) ||
            status != MEM_SERVICE_WIRE_STATUS_OK ||
            strcmp(work.key, objects[i].key) ||
            work.generation != objects[i].generation ||
            strcmp(work.state, "retired") ||
            strcmp(work.home_node, old_config.node) ||
            work.provider_incarnation != old_config.incarnation ||
            work.provider_backed || work.holder_count || work.live_refs ||
            work.descriptor_len || work.address || work.address_len) goto done;
    }
    reason = "journal-changed";
    if (fstat(fd, &after) || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) goto done;
    if (!already_archived) {
        reason = "archive-conflict";
        if (lstat(archive, &path_stat) == 0 || errno != ENOENT ||
            lstat(old_config.state, &path_stat) ||
            path_stat.st_dev != before.st_dev || path_stat.st_ino != before.st_ino)
            goto done;
        reason = "archive-failed";
        if (rename(old_config.state, archive) ||
            worker_sync_state_parent(old_config.state)) goto done;
    }
    printf("obmm-worker-recovery: status=complete old_incarnation=%" PRIu64
           " current_incarnation=%" PRIu64 " ledger_objects=%zu"
           " recovered_objects=%zu kernel_records=%zu revision=%" PRIu64
           " archive=%s\n",
           old_config.incarnation, replacement.incarnation, object_count,
           recovered_count, inventory_count, revision, archive);
    if (!ferror(stdout)) result = 0;
done:
    if (device >= 0) close(device);
    if (fd >= 0) close(fd);
    free(entries);
    free(objects);
    free(old_segments);
    if (result)
        fprintf(stderr,
                "obmm-worker-recovery: status=failed reason=%s "
                "reconciliation_required=1\n",
                reason);
    return result;
}

static int allocate_work(int device, int journal,
    const struct worker_config *config, const struct mem_service_client *client,
    const struct obmm_cmd_gsva_aperture *aperture,
    const struct mem_service_client_allocation *work,
    struct worker_reservation *reservation)
{
    struct obmm_cmd_gsva_alloc_segment_v1 request = {0};
    struct obmm_cmd_export exported = {0};
    struct mem_service_provider_descriptor descriptor;
    struct mem_service_client_allocation published;
    enum mem_service_wire_status status;
    uint64_t alignment = work->alignment_bytes;
    uint64_t granularity = config->allocation_granularity_bytes;
    uint64_t backing_size, first_aligned;
    long page_size = sysconf(_SC_PAGESIZE);
    int export_result;

    if (strcmp(work->state, "allocating") || work->capabilities != MEM_SERVICE_MANAGED_CAP_MAP ||
        strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation || reservation->occupied ||
        !work->generation || !work->key[0] || page_size <= 0 ||
        work->provider_backed || work->descriptor_len || work->address || work->address_len ||
        work->holder_count || work->live_refs || !aperture->base || !aperture->size ||
        aperture->size > UINT64_MAX - aperture->base) return WORKER_ALLOCATE_ERROR;
    if (!granularity || (granularity & (granularity - 1)) ||
        granularity < (uint64_t)page_size || !work->size_bytes)
        return WORKER_ALLOCATE_ERROR;
    if (work->size_bytes > UINT64_MAX - (granularity - 1))
        return WORKER_ALLOCATE_CAPACITY;
    backing_size = (work->size_bytes + granularity - 1) & ~(granularity - 1);
    if (alignment && (alignment & (alignment - 1))) return -1;
    if (alignment < granularity) alignment = granularity;
    if (aperture->base > UINT64_MAX - (alignment - 1))
        return WORKER_ALLOCATE_CAPACITY;
    first_aligned = (aperture->base + alignment - 1) & ~(alignment - 1);
    if (first_aligned - aperture->base > aperture->size ||
        backing_size > aperture->size - (first_aligned - aperture->base))
        return WORKER_ALLOCATE_CAPACITY;
    /* The kernel owns interval selection and reuse. This zero request must
     * not be replaced by a second worker-side address allocator. */
    request.requested_home_va = 0;
    if (record_phase(journal, config, work, reservation,
                     work->generation, "reserve-intent", 0, 0)) return -1;
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
        const struct obmm_gsva_segment_desc_v1 empty = {0};
        /* Only this direct allocation API guarantees ENOSPC before reserving
         * an interval. Lost output (EFAULT) and contradictory output remain
         * unknown, even if no segment ID is visible to this process. */
        if (errno == ENOSPC && !memcmp(&request.desc, &empty, sizeof(empty))) {
            if (record_phase(journal, config, work, reservation,
                             work->generation, "reserve-empty", 0, 0))
                return WORKER_ALLOCATE_ERROR;
            memset(reservation, 0, sizeof(*reservation));
            return WORKER_ALLOCATE_CAPACITY;
        }
        reservation->segment = request.desc;
        (void)record_phase(journal, config, work, reservation,
                           work->generation, "reserve-unknown",
                           request.desc.segment_id, 0);
        return -1;
    }
    reservation->segment = request.desc;
    if (record_phase(journal, config, work, reservation,
                     work->generation, "reserved", request.desc.segment_id, 0)) return -1;
    if (request.desc.home_va < aperture->base ||
        request.desc.home_va - aperture->base > aperture->size ||
        request.desc.size > aperture->size - (request.desc.home_va - aperture->base)) return -1;
    export_result = gva_manager_export_segment_checked(device, &request.desc,
                                                      config->fast_allocation, &exported);
    if (export_result) {
        reservation->exported = exported;
        if (export_result == 1) {
            reservation->export_no_backing = true;
            if (record_phase(journal, config, work, reservation,
                             work->generation, "export-no-backing",
                             request.desc.segment_id, 0)) return -1;
            return WORKER_ALLOCATE_BACKING_EMPTY;
        }
        (void)record_phase(journal, config, work, reservation,
                           work->generation, "export-unknown",
                           request.desc.segment_id, exported.mem_id);
        return -1;
    }
    reservation->exported = exported;
    if (mem_service_provider_obmm_encode_gsva(&request.desc, &exported, &descriptor)) {
        (void)record_phase(journal, config, work, reservation,
                           work->generation, "export-unknown",
                           request.desc.segment_id, exported.mem_id);
        return -1;
    }
    reservation->descriptor = descriptor;
    if (record_phase(journal, config, work, reservation, work->generation, "exported",
                     request.desc.segment_id, exported.mem_id)) return -1;
    if (mem_service_client_publish_allocation(client, work->key, config->node,
            config->incarnation, work->generation, descriptor.bytes, descriptor.len,
            request.desc.home_va, request.desc.size, &published, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK ||
        (strcmp(published.state, "active") && strcmp(published.state, "retiring")) ||
        published.generation != work->generation ||
        published.address != request.desc.home_va || published.address_len != request.desc.size ||
        published.descriptor_len != descriptor.len ||
        memcmp(published.descriptor, descriptor.bytes, descriptor.len)) return -1;
    if (record_phase(journal, config, work, reservation, work->generation, "published",
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
    if (record_phase(journal, config, work, reservation, work->generation, "release-intent",
                     reservation->segment.segment_id, reservation->exported.mem_id) ||
        gva_manager_unexport_segment(device, &reservation->segment, &reservation->exported) ||
        record_phase(journal, config, work, reservation, work->generation, "unexported",
                     reservation->segment.segment_id, reservation->exported.mem_id)) return -1;
    retire.version = OBMM_GSVA_ABI_VERSION;
    retire.segment_id = reservation->segment.segment_id;
    retire.epoch = reservation->segment.epoch;
    retire.timeout_ms = 5000;
    if (gva_manager_retire_segment(device, &retire) ||
        record_phase(journal, config, work, reservation,
                     work->generation, "retired", retire.segment_id, 0)) return -1;
    if (mem_service_client_reclaim_allocation(client, work->key, config->node,
            config->incarnation, work->generation, true, &reclaimed, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK || strcmp(reclaimed.state, "retired") ||
        reclaimed.generation != work->generation ||
        record_phase(journal, config, work, reservation,
                     work->generation, "reclaimed", retire.segment_id, 0)) return -1;
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
    if (record_phase(journal, config, work, NULL, work->generation, "cancel-empty", 0, 0) ||
        mem_service_client_reclaim_allocation(client, work->key, config->node,
            config->incarnation, work->generation, true, &reclaimed, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK || strcmp(reclaimed.state, "retired") ||
        reclaimed.generation != work->generation ||
        record_phase(journal, config, work, NULL,
                     work->generation, "cancel-confirmed", 0, 0)) return -1;
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

    /* Requires confirmed no segment, or a NO_BACKING receipt followed by
     * confirmed segment retirement. Unknown ioctl errors never authorize it. */
    if (strcmp(work->state, "allocating") || work->provider_backed ||
        work->descriptor_len || work->address || work->address_len ||
        work->holder_count || work->live_refs || !work->generation ||
        strcmp(work->home_node, config->node) ||
        work->provider_incarnation != config->incarnation) return -1;
    length = snprintf(operation, sizeof(operation), "obmm-capacity-%" PRIu64 "-%" PRIu64,
                      config->incarnation, work->generation);
    if (length < 0 || (size_t)length >= sizeof(operation) ||
        record_phase(journal, config, work, NULL,
                     work->generation, "capacity-reject-intent", 0, 0) ||
        mem_service_client_retire_object(client, work->key, operation, true,
                                        work->generation, &cancelled, &status) ||
        status != MEM_SERVICE_WIRE_STATUS_OK ||
        strcmp(cancelled.key, work->key) || cancelled.generation != work->generation ||
        cancelled.size_bytes != work->size_bytes ||
        cancelled.alignment_bytes != work->alignment_bytes ||
        cancelled.capabilities != work->capabilities ||
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
    if (record_phase(journal, config, work, reservation,
                     work->generation, "unbacked-retire-intent", retire.segment_id, 0) ||
        gva_manager_retire_segment(device, &retire) ||
        record_phase(journal, config, work, reservation,
                     work->generation, "unbacked-retired", retire.segment_id, 0) ||
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
    struct obmm_cmd_gsva_enumerate_v1 birth = {.version = OBMM_GSVA_ABI_VERSION};
    struct sigaction action = {0}, old_int, old_term;
    const struct timespec interval = {.tv_nsec = 100000000};
    enum mem_service_wire_status status;
    int device = -1, journal = -1, result = 1;
    bool signals = false, refreshed = false;
    struct worker_reservation *reservations = NULL;

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
    if (gva_manager_enumerate_segments(device, &birth)) goto done;
    memcpy(config.kernel_instance, birth.kernel_instance, 16);
    reservations = calloc(MEM_SERVICE_MANAGED_MAX_ALLOCATIONS, sizeof(*reservations));
    if (!reservations) goto done;
    journal = create_state(&config);
    if (journal < 0) goto done;
    printf("obmm-worker address_reuse=kernel-confirmed home_policy=single-owner "
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
                    &client, &aperture, &work, available);
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
