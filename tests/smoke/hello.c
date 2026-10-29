/* hello.c - bisection helper: one tiny console program built for x86_64, aarch64 and arm64ec.
 * Prints the architecture it was compiled for and what the OS says about the process. Exit code 0.
 * Used by tools/build/macos/run-ladder.sh to tell "Wine itself" from "ARM64EC" from "FEX". */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#if defined(__arm64ec__)
#define ARCH "arm64ec"
#elif defined(__aarch64__)
#define ARCH "aarch64"
#elif defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "unknown"
#endif

int main(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("HELLO compiled-for=%s pointer=%u guest-arch=%u pagesize=%lu\n", ARCH, (unsigned)sizeof(void *),
           si.wProcessorArchitecture, si.dwPageSize);
    fflush(stdout);
    return 0;
}
