#define _POSIX_C_SOURCE 200809L
#include "mem_service_replay_history.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char history_magic[16] = "MSREPLAY0000001\n";
#define FRAME_HEADER 48U
#define FRAME_BYTES (FRAME_HEADER + MEM_SERVICE_IDEMPOTENCY_KEY_LEN + \
                     MEM_SERVICE_IDEMPOTENCY_RESPONSE_LEN + 8U)

static uint64_t decode(const unsigned char *p, size_t n)
{
    uint64_t value = 0;
    while (n != 0) value = (value << 8) | p[--n];
    return value;
}

static void encode(unsigned char *p, uint64_t value, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        p[i] = (unsigned char)value;
        value >>= 8;
    }
}

static uint64_t frame_checksum(const unsigned char *p, size_t n)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; ++i)
        value = (value ^ p[i]) * UINT64_C(1099511628211);
    return value;
}

/* 0 means complete; 1 is clean EOF before any byte; -1 includes torn data. */
static int read_exact(int fd, unsigned char *p, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t got = read(fd, p + done, n - done);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return -1;
        if (got == 0) return done == 0 ? 1 : -1;
        done += (size_t)got;
    }
    return 0;
}

static int write_exact(int fd, const unsigned char *p, size_t n)
{
    while (n != 0) {
        ssize_t put = write(fd, p, n);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return -1;
        p += put;
        n -= (size_t)put;
    }
    return 0;
}

static bool same_file(const struct mem_service_replay_history *h)
{
    struct stat opened, named;
    return h->fd >= 0 && !h->failed && fstat(h->fd, &opened) == 0 &&
        lstat(h->path, &named) == 0 && S_ISREG(opened.st_mode) &&
        S_ISREG(named.st_mode) && opened.st_nlink == 1 &&
        opened.st_dev == named.st_dev && opened.st_ino == named.st_ino;
}

struct batch_lookup {
    const struct mem_service_idempotency_record *const *records;
    size_t count;
    bool present[MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS];
    bool conflict;
};

static int scan(struct mem_service_replay_history *h, uint64_t prefix_count,
    uint64_t prefix_checksum, const char *key,
    struct mem_service_idempotency_record *out, bool allow_suffix,
    struct batch_lookup *batch)
{
    unsigned char frame[FRAME_BYTES];
    struct mem_service_idempotency_record found = {0};
    uint64_t count = 0, checksum = 0;
    bool prefix_seen = prefix_count == 0 && prefix_checksum == 0;
    if (!same_file(h) || lseek(h->fd, 0, SEEK_SET) != 0 ||
        read_exact(h->fd, frame, sizeof(history_magic)) != 0 ||
        memcmp(frame, history_magic, sizeof(history_magic)) != 0)
        goto fail;
    for (;;) {
        int rc = read_exact(h->fd, frame, FRAME_HEADER);
        if (rc == 1) break;
        if (rc != 0 || count == UINT64_MAX ||
            decode(frame, 8) != count + 1 || decode(frame + 8, 8) != checksum ||
            decode(frame + 16, 4) == 0 || decode(frame + 36, 4) != 0 ||
            decode(frame + 40, 8) != 0)
            goto fail;
        size_t key_len = (size_t)decode(frame + 28, 4);
        size_t response_len = (size_t)decode(frame + 32, 4);
        if (key_len == 0 || key_len >= MEM_SERVICE_IDEMPOTENCY_KEY_LEN ||
            response_len >= MEM_SERVICE_IDEMPOTENCY_RESPONSE_LEN)
            goto fail;
        size_t bytes = FRAME_HEADER + key_len + response_len;
        if (read_exact(h->fd, frame + FRAME_HEADER,
                       key_len + response_len + 8U) != 0 ||
            memchr(frame + FRAME_HEADER, 0, key_len + response_len) != NULL ||
            decode(frame + bytes, 8) != frame_checksum(frame, bytes))
            goto fail;
        checksum = decode(frame + bytes, 8);
        count++;
        if (count == prefix_count) prefix_seen = checksum == prefix_checksum;
        for (size_t i = 0; batch != NULL && i < batch->count; ++i) {
            const struct mem_service_idempotency_record *r = batch->records[i];
            if (strlen(r->key) != key_len ||
                memcmp(r->key, frame + FRAME_HEADER, key_len) != 0) continue;
            batch->present[i] = true;
            if (r->operation != decode(frame + 16, 4) ||
                r->request_checksum != decode(frame + 20, 4) ||
                r->status != decode(frame + 24, 4) ||
                r->response_len != response_len ||
                memcmp(r->response, frame + FRAME_HEADER + key_len, response_len) != 0)
                batch->conflict = true;
        }
        if (key != NULL && strlen(key) == key_len &&
            memcmp(key, frame + FRAME_HEADER, key_len) == 0) {
            memset(&found, 0, sizeof(found));
            found.in_use = true;
            memcpy(found.key, key, key_len);
            found.operation = (uint32_t)decode(frame + 16, 4);
            found.request_checksum = (uint32_t)decode(frame + 20, 4);
            found.status = (uint32_t)decode(frame + 24, 4);
            found.response_len = (uint32_t)response_len;
            memcpy(found.response, frame + FRAME_HEADER + key_len, response_len);
        }
    }
    if (!prefix_seen || (!allow_suffix && count != prefix_count) || !same_file(h))
        goto fail;
    h->count = count;
    h->checksum = checksum;
    if (found.in_use && out != NULL) *out = found;
    return found.in_use ? 1 : 0;
fail:
    h->failed = true;
    return -1;
}

static int sync_parent(const char *path)
{
    char parent[512];
    memcpy(parent, path, strlen(path) + 1);
    char *slash = strrchr(parent, '/');
    if (slash == NULL) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    return rc;
}

int mem_service_replay_history_open(struct mem_service_replay_history *h,
    const char *path, bool create, uint64_t checkpoint_count,
    uint64_t checkpoint_checksum)
{
    if (h == NULL) return -1;
    memset(h, 0, sizeof(*h));
    h->fd = -1;
    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof(h->path) ||
        (checkpoint_count == 0 && checkpoint_checksum != 0) ||
        (create && (checkpoint_count != 0 || checkpoint_checksum != 0)))
        return -1;
    memcpy(h->path, path, strlen(path) + 1);
    h->fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                 (create ? O_CREAT | O_EXCL : 0), 0600);
    if (h->fd < 0) return -1;
    if (flock(h->fd, LOCK_EX | LOCK_NB) != 0 || !same_file(h)) goto fail;
    if (create && (write_exact(h->fd, history_magic, sizeof(history_magic)) != 0 ||
                   fsync(h->fd) != 0 || sync_parent(path) != 0))
        goto fail;
    if (scan(h, checkpoint_count, checkpoint_checksum, NULL, NULL, true, NULL) < 0)
        goto fail;
    return 0;
fail:
    (void)close(h->fd);
    h->fd = -1;
    h->failed = true;
    return -1;
}

int mem_service_replay_history_close(struct mem_service_replay_history *h)
{
    if (h == NULL || h->fd < 0) return -1;
    int rc = close(h->fd);
    h->fd = -1;
    return rc;
}

int mem_service_replay_history_find(struct mem_service_replay_history *h,
    const char *key, struct mem_service_idempotency_record *record)
{
    if (h == NULL || key == NULL || record == NULL || key[0] == '\0' ||
        strnlen(key, MEM_SERVICE_IDEMPOTENCY_KEY_LEN) >= MEM_SERVICE_IDEMPOTENCY_KEY_LEN)
        return -1;
    return scan(h, h->count, h->checksum, key, record, false, NULL);
}

static bool valid_record(const struct mem_service_idempotency_record *r)
{
    return r != NULL && r->in_use && r->operation != 0 && r->key[0] != '\0' &&
        strnlen(r->key, MEM_SERVICE_IDEMPOTENCY_KEY_LEN) < MEM_SERVICE_IDEMPOTENCY_KEY_LEN &&
        r->response_len < MEM_SERVICE_IDEMPOTENCY_RESPONSE_LEN &&
        strnlen(r->response, MEM_SERVICE_IDEMPOTENCY_RESPONSE_LEN) == r->response_len;
}

static bool same_record(const struct mem_service_idempotency_record *a,
                        const struct mem_service_idempotency_record *b)
{
    return a->operation == b->operation && a->request_checksum == b->request_checksum &&
        a->status == b->status && a->response_len == b->response_len &&
        memcmp(a->response, b->response, a->response_len) == 0;
}

static int append_frame(struct mem_service_replay_history *h,
    const struct mem_service_idempotency_record *record, uint64_t *count,
    uint64_t *chain)
{
    unsigned char frame[FRAME_BYTES] = {0};
    size_t key_len = strlen(record->key);
    size_t bytes = FRAME_HEADER + key_len + record->response_len;
    encode(frame, *count + 1, 8);
    encode(frame + 8, *chain, 8);
    encode(frame + 16, record->operation, 4);
    encode(frame + 20, record->request_checksum, 4);
    encode(frame + 24, record->status, 4);
    encode(frame + 28, key_len, 4);
    encode(frame + 32, record->response_len, 4);
    memcpy(frame + FRAME_HEADER, record->key, key_len);
    memcpy(frame + FRAME_HEADER + key_len, record->response, record->response_len);
    uint64_t checksum = frame_checksum(frame, bytes);
    encode(frame + bytes, checksum, 8);
    if (write_exact(h->fd, frame, bytes + 8U) != 0) return -1;
    (*count)++;
    *chain = checksum;
    return 0;
}

int mem_service_replay_history_append_batch(struct mem_service_replay_history *h,
    const struct mem_service_idempotency_record *const *records, size_t count)
{
    struct batch_lookup batch = {.records = records, .count = count};
    uint64_t added = 0;
    if (h == NULL || records == NULL || count == 0 ||
        count > MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!valid_record(records[i])) return -1;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(records[i]->key, records[j]->key) != 0) continue;
            if (!same_record(records[i], records[j])) return -1;
            batch.present[i] = true;
        }
    }
    if (scan(h, h->count, h->checksum, NULL, NULL, false, &batch) < 0 ||
        batch.conflict) return -1;
    for (size_t i = 0; i < count; ++i) added += batch.present[i] ? 0U : 1U;
    if (added > UINT64_MAX - h->count) return -1;
    if (added == 0) return 0;
    uint64_t next_count = h->count, next_checksum = h->checksum;
    /* The scan leaves the descriptor at validated EOF. All candidate checks
     * precede writes. A failed batch may leave a complete or partial suffix;
     * neither its checkpoint nor any caller cache is committed on failure. */
    for (size_t i = 0; i < count; ++i) {
        if (!batch.present[i] &&
            append_frame(h, records[i], &next_count, &next_checksum) != 0) {
            h->failed = true;
            return -1;
        }
    }
    if (fsync(h->fd) != 0 || !same_file(h)) {
        h->failed = true;
        return -1;
    }
    h->count = next_count;
    h->checksum = next_checksum;
    return 0;
}

int mem_service_replay_history_append(struct mem_service_replay_history *h,
    const struct mem_service_idempotency_record *record)
{
    return mem_service_replay_history_append_batch(h, &record, 1);
}
