#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

int __real_mprotect(void *address, size_t length, int protection);
int __real_munmap(void *address, size_t length);

/* Third exact fixture view is the saved-descriptor probe, after old/new
 * mappings. This fixture tests diagnostic accounting, not OBMM revocation. */
int __wrap_mprotect(void *address, size_t length, int protection)
{
    static unsigned views;
    if ((uintptr_t)address == UINT64_C(0x400000000) && length == 4096 &&
        protection == PROT_READ && ++views == 3) {
#ifdef SESSION_TEST_RETIRED_MAP_ERROR
        errno = EIO;
        return -1;
#else
        return __real_mprotect(address, length, PROT_NONE);
#endif
    }
    return __real_mprotect(address, length, protection);
}

int __wrap_munmap(void *address, size_t length)
{
#ifdef SESSION_TEST_RETIRED_CLEANUP_ERROR
    static unsigned unmaps;
    if ((uintptr_t)address == UINT64_C(0x400000000) && length == 16384 &&
        ++unmaps == 3) {
        errno = EIO;
        return -1;
    }
#endif
    return __real_munmap(address, length);
}
