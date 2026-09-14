#define _POSIX_C_SOURCE 200809L
#include "mem_service_replay_history.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Compile-time syscall substitution affects only this fixture binary. */
#undef fsync
#undef write
#undef read
extern int fsync(int);
extern ssize_t write(int, const void *, size_t);
extern ssize_t read(int, void *, size_t);
static bool fail_sync;
static int fail_write;
static int writes_until_failure = -1;
static uint64_t read_calls, sync_calls, write_calls;
int history_test_fsync(int fd)
{
    sync_calls++;
    if (fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}
ssize_t history_test_write(int fd, const void *p, size_t n)
{
    write_calls++;
    if (writes_until_failure == 0) { fail_write = 1; writes_until_failure = -1; }
    else if (writes_until_failure > 0) writes_until_failure--;
    if (fail_write == 1) { fail_write = 2; return write(fd, p, n > 7 ? 7 : n); }
    if (fail_write == 2) { errno = ENOSPC; return -1; }
    return write(fd, p, n);
}
ssize_t history_test_read(int fd, void *p, size_t n)
{
    read_calls++;
    return read(fd, p, n);
}

static struct mem_service_idempotency_record make_record(unsigned int i)
{
    struct mem_service_idempotency_record r = {0};
    r.in_use = true;
    r.operation = 0x71;
    r.request_checksum = i * 177U + 1U;
    r.status = i % 3U;
    snprintf(r.key, sizeof(r.key), "operation-%u", i);
    snprintf(r.response, sizeof(r.response), "status=%u\ngeneration=%u\nexact-tail", r.status, i);
    r.response_len = (uint32_t)strlen(r.response);
    return r;
}

static void roundtrip(const char *path)
{
    struct mem_service_replay_history h;
    uint64_t checkpoint = 0;
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    for (unsigned int i = 0; i < 300; ++i) {
        struct mem_service_idempotency_record r = make_record(i);
        assert(mem_service_replay_history_append(&h, &r) == 0);
        if (i == 63) checkpoint = h.checksum;
    }
    assert(h.count == 300);
    uint64_t final_checksum = h.checksum;
    assert(mem_service_replay_history_close(&h) == 0);
    /* Complete suffix after the snapshot checkpoint must remain replayable. */
    assert(mem_service_replay_history_open(&h, path, false, 64, checkpoint) == 0);
    assert(h.count == 300 && h.checksum == final_checksum);
    for (unsigned int i = 0; i < 300; ++i) {
        struct mem_service_idempotency_record r = make_record(i), result = {0};
        assert(mem_service_replay_history_find(&h, r.key, &result) == 1);
        assert(memcmp(&r, &result, sizeof(r)) == 0);
        assert(mem_service_replay_history_append(&h, &r) == 0);
    }
    assert(h.count == 300 && h.checksum == final_checksum);
    struct mem_service_idempotency_record r = make_record(0), unchanged = r;
    assert(mem_service_replay_history_find(&h, "missing", &r) == 0);
    assert(memcmp(&r, &unchanged, sizeof(r)) == 0);
    r.status++;
    assert(mem_service_replay_history_append(&h, &r) == -1 && !h.failed);
    assert(h.count == 300);
    assert(mem_service_replay_history_close(&h) == 0);
    assert(mem_service_replay_history_open(&h, path, false, 64, checkpoint ^ 1) == -1);
    assert(h.fd == -1);
}

static void bounds(const char *path)
{
    struct mem_service_replay_history h;
    struct mem_service_idempotency_record r = make_record(0), result;
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    memset(r.key, 'k', sizeof(r.key));
    assert(mem_service_replay_history_append(&h, &r) == -1);
    r.key[sizeof(r.key) - 1] = 0;
    memset(r.response, 'r', sizeof(r.response));
    r.response_len = sizeof(r.response) - 1;
    assert(mem_service_replay_history_append(&h, &r) == -1);
    r.response[r.response_len] = 0;
    assert(mem_service_replay_history_append(&h, &r) == 0);
    assert(mem_service_replay_history_find(&h, r.key, &result) == 1);
    assert(memcmp(&r, &result, sizeof(r)) == 0);
    r = make_record(1);
    r.response[0] = 0;
    assert(mem_service_replay_history_append(&h, &r) == -1);
    r = make_record(2);
    r.response_len = 0;
    r.response[0] = 0;
    assert(mem_service_replay_history_append(&h, &r) == 0);
    assert(mem_service_replay_history_find(&h, r.key, &result) == 1);
    assert(result.response_len == 0 && result.response[0] == 0);
    assert(mem_service_replay_history_close(&h) == 0);
}

static void damage(const char *path, const char *mode, bool batch)
{
    struct mem_service_replay_history h;
    struct mem_service_idempotency_record r = make_record(0), unchanged = r;
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    assert(mem_service_replay_history_append(&h, &r) == 0);
    off_t prefix_bytes = lseek(h.fd, 0, SEEK_END);
    r = make_record(1);
    assert(mem_service_replay_history_append(&h, &r) == 0);
    uint64_t count = h.count, checksum = h.checksum;
    if (strcmp(mode, "truncate") == 0) {
        assert(ftruncate(h.fd, prefix_bytes) == 0);
    } else if (strcmp(mode, "torn") == 0) {
        assert(write(h.fd, "partial", 7) == 7);
    } else {
        unsigned char byte;
        assert(pread(h.fd, &byte, 1, prefix_bytes + 50) == 1);
        byte ^= 1U;
        assert(pwrite(h.fd, &byte, 1, prefix_bytes + 50) == 1);
    }
    r = unchanged;
    /* Even a key in the intact first frame must not hide later damage. */
    if (batch) {
        const struct mem_service_idempotency_record *records[] = {&r};
        assert(mem_service_replay_history_append_batch(&h, records, 1) == -1 && h.failed);
    } else {
        assert(mem_service_replay_history_find(&h, r.key, &r) == -1 && h.failed);
    }
    assert(memcmp(&r, &unchanged, sizeof(r)) == 0);
    assert(mem_service_replay_history_append(&h, &r) == -1);
    assert(mem_service_replay_history_close(&h) == 0);
    assert(mem_service_replay_history_open(&h, path, false, count, checksum) == -1);
}

static void ownership(const char *path)
{
    struct mem_service_replay_history h, other;
    struct mem_service_idempotency_record r = make_record(0);
    assert(mem_service_replay_history_open(&h, path, false, 0, 0) == -1);
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    assert(mem_service_replay_history_open(&other, path, true, 0, 0) == -1);
    assert(mem_service_replay_history_open(&other, path, false, 0, 0) == -1);
    assert(mem_service_replay_history_append(&h, &r) == 0);
    char renamed[600];
    snprintf(renamed, sizeof(renamed), "%s.renamed", path);
    assert(rename(path, renamed) == 0);
    assert(symlink(renamed, path) == 0);
    assert(mem_service_replay_history_open(&other, path, false, 0, 0) == -1);
    assert(mem_service_replay_history_find(&h, r.key, &r) == -1);
    assert(mem_service_replay_history_close(&h) == 0);
}

static void io_failure(const char *path, bool sync_failure)
{
    struct mem_service_replay_history h;
    struct mem_service_idempotency_record r = make_record(0);
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    assert(mem_service_replay_history_append(&h, &r) == 0);
    uint64_t count = h.count, checksum = h.checksum;
    r = make_record(1);
    fail_sync = sync_failure;
    fail_write = sync_failure ? 0 : 1;
    assert(mem_service_replay_history_append(&h, &r) == -1 && h.failed);
    assert(h.count == count && h.checksum == checksum);
    fail_sync = false;
    fail_write = 0;
    assert(mem_service_replay_history_append(&h, &r) == -1);
    assert(mem_service_replay_history_find(&h, r.key, &r) == -1);
    assert(mem_service_replay_history_close(&h) == 0);
    /* An uncertain complete frame is validated afresh on reopen. A torn
     * frame remains rejected and is never silently truncated. */
    int rc = mem_service_replay_history_open(&h, path, false, count, checksum);
    assert(rc == (sync_failure ? 0 : -1));
    if (rc == 0) {
        assert(h.count == 2);
        assert(mem_service_replay_history_find(&h, r.key, &r) == 1);
        assert(mem_service_replay_history_close(&h) == 0);
    }
}

static void batch_roundtrip(const char *path)
{
    struct mem_service_replay_history h;
    struct mem_service_idempotency_record *records = calloc(59, sizeof(*records));
    const struct mem_service_idempotency_record *batch[59];
    assert(records != NULL);
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    for (unsigned i = 0; i < 671; ++i) {
        struct mem_service_idempotency_record r = make_record(i);
        assert(mem_service_replay_history_append(&h, &r) == 0);
    }
    uint64_t prefix_checksum = h.checksum;
    for (unsigned i = 0; i < 59; ++i) {
        records[i] = make_record(671 + i);
        batch[i] = &records[i];
    }
    read_calls = sync_calls = 0;
    assert(mem_service_replay_history_append_batch(&h, batch, 59) == 0);
    assert(read_calls == 1344 && sync_calls == 1 && h.count == 730);
    printf("batch_history start_records=671 appended=59 read_calls=%llu sync_calls=%llu\n",
           (unsigned long long)read_calls, (unsigned long long)sync_calls);
    assert(mem_service_replay_history_close(&h) == 0);
    assert(mem_service_replay_history_open(&h, path, false, 671, prefix_checksum) == 0);
    assert(h.count == 730);
    for (unsigned i = 0; i < 59; ++i) {
        struct mem_service_idempotency_record found = {0};
        assert(mem_service_replay_history_find(&h, batch[i]->key, &found) == 1);
        assert(memcmp(batch[i], &found, sizeof(found)) == 0);
    }
    uint64_t checksum = h.checksum;
    sync_calls = 0;
    assert(mem_service_replay_history_append_batch(&h, batch, 59) == 0);
    assert(h.count == 730 && h.checksum == checksum && sync_calls == 0);
    assert(mem_service_replay_history_close(&h) == 0);
    free(records);
}

static void batch_preflight(const char *path)
{
    struct mem_service_replay_history h;
    struct mem_service_idempotency_record a = make_record(0), b = make_record(1);
    const struct mem_service_idempotency_record *batch[] = {&a, &b, &b, &a};
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    assert(mem_service_replay_history_append_batch(&h, batch, 4) == 0);
    assert(h.count == 2);
    uint64_t checksum = h.checksum;
    struct mem_service_idempotency_record conflicting = b, fresh = make_record(2);
    conflicting.status++;
    const struct mem_service_idempotency_record *invalid[] = {&fresh, &conflicting};
    write_calls = 0;
    assert(mem_service_replay_history_append_batch(&h, invalid, 2) == -1);
    assert(h.count == 2 && h.checksum == checksum && !h.failed && write_calls == 0);
    conflicting = fresh;
    conflicting.operation++;
    assert(mem_service_replay_history_append_batch(&h, invalid, 2) == -1);
    assert(!h.failed && write_calls == 0);
    memset(conflicting.key, 'x', sizeof(conflicting.key));
    assert(mem_service_replay_history_append_batch(&h, invalid, 2) == -1);
    invalid[1] = NULL;
    assert(mem_service_replay_history_append_batch(&h, invalid, 2) == -1);
    assert(mem_service_replay_history_append_batch(&h, batch, 0) == -1);
    assert(mem_service_replay_history_append_batch(&h, NULL, 1) == -1);
    assert(mem_service_replay_history_append_batch(NULL, batch, 1) == -1);
    assert(mem_service_replay_history_append_batch(&h, batch,
        MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS + 1) == -1);
    assert(h.count == 2 && h.checksum == checksum && write_calls == 0);
    assert(mem_service_replay_history_close(&h) == 0);
}

static void batch_io_failure(const char *path, bool sync_failure)
{
    struct mem_service_replay_history h;
    struct mem_service_idempotency_record a = make_record(0), b = make_record(1), c = make_record(2);
    const struct mem_service_idempotency_record *batch[] = {&b, &c};
    assert(mem_service_replay_history_open(&h, path, true, 0, 0) == 0);
    assert(mem_service_replay_history_append(&h, &a) == 0);
    uint64_t checksum = h.checksum;
    fail_sync = sync_failure;
    writes_until_failure = sync_failure ? -1 : 1;
    assert(mem_service_replay_history_append_batch(&h, batch, 2) == -1);
    assert(h.failed && h.count == 1 && h.checksum == checksum);
    fail_sync = false; fail_write = 0; writes_until_failure = -1;
    assert(mem_service_replay_history_append_batch(&h, batch, 2) == -1);
    assert(mem_service_replay_history_close(&h) == 0);
    int rc = mem_service_replay_history_open(&h, path, false, 1, checksum);
    assert(rc == (sync_failure ? 0 : -1));
    if (rc == 0) {
        assert(h.count == 3);
        assert(mem_service_replay_history_append_batch(&h, batch, 2) == 0);
        assert(h.count == 3);
        assert(mem_service_replay_history_close(&h) == 0);
    }
}

int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "--case") != 0) goto usage;
    if (strcmp(argv[2], "roundtrip") == 0) roundtrip(argv[3]);
    else if (strcmp(argv[2], "bounds") == 0) bounds(argv[3]);
    else if (strcmp(argv[2], "truncate") == 0 || strcmp(argv[2], "torn") == 0 ||
             strcmp(argv[2], "corrupt") == 0) damage(argv[3], argv[2], false);
    else if (strcmp(argv[2], "ownership") == 0) ownership(argv[3]);
    else if (strcmp(argv[2], "write-failure") == 0) io_failure(argv[3], false);
    else if (strcmp(argv[2], "sync-failure") == 0) io_failure(argv[3], true);
    else if (strcmp(argv[2], "batch-roundtrip") == 0) batch_roundtrip(argv[3]);
    else if (strcmp(argv[2], "batch-preflight") == 0) batch_preflight(argv[3]);
    else if (strcmp(argv[2], "batch-write-failure") == 0) batch_io_failure(argv[3], false);
    else if (strcmp(argv[2], "batch-sync-failure") == 0) batch_io_failure(argv[3], true);
    else if (strcmp(argv[2], "batch-corrupt") == 0) damage(argv[3], "corrupt", true);
    else if (strcmp(argv[2], "batch-truncate") == 0) damage(argv[3], "truncate", true);
    else if (strcmp(argv[2], "batch-torn") == 0) damage(argv[3], "torn", true);
    else goto usage;
    printf("replay_history=pass case=%s scope=storage-boundary-fixture\n", argv[2]);
    return 0;
usage:
    fprintf(stderr, "usage: replay-history --case <name> <new-history-path>\n");
    return 2;
}
