#include <stddef.h>
#include <stdint.h>
#include <errno.h>

int __real_munmap(void *address, size_t length);

/* Simulate a provider reporting success while the exact test VMA remains. */
int __wrap_munmap(void *address, size_t length)
{
    if ((uintptr_t)address == UINT64_C(0x400000000) && length == 16384) {
#ifdef SESSION_TEST_UNMAP_FAIL_ONCE
        static int failed;
        if (!failed++) {
            errno = EIO;
            return -1;
        }
#else
        return 0;
#endif
    }
    return __real_munmap(address, length);
}
