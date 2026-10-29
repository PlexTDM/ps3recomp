# Platform abstraction

This tree targets Linux. The runtime was originally written against the Win32
API and keeps those call sites; `runtime/platform/win32_compat.h` supplies the
same names over POSIX primitives so the shared code does not need two copies of
every function. There are no `#ifdef _WIN32` branches left in the runtime and
HLE modules -- the shim is the only platform layer.

| Win32 name | POSIX implementation |
|---|---|
| `CreateThread`, `ExitThread`, `WaitForSingleObject` | `pthread_create`, `pthread_exit`, `pthread_join` |
| `CRITICAL_SECTION`, `SRWLOCK`, `CONDITION_VARIABLE` | `pthread_mutex_t`, `pthread_rwlock_t`, `pthread_cond_t` |
| `Sleep`, `GetTickCount64`, `QueryPerformanceCounter` | `nanosleep`, `CLOCK_MONOTONIC` |
| `CreateEventA`, `SetEvent`, `WaitForSingleObject` | a mutex + condition variable + signalled flag |
| `VirtualAlloc`, `VirtualFree` | `mmap`, `munmap` |
| `InterlockedIncrement`, `InterlockedExchange` | C11 atomics |
| `__declspec(thread)` | `__thread` |
| `AddVectoredExceptionHandler` | a `sigaction` wrapper |
| `_byteswap_*`, `__int64` | compiler builtins and the POSIX scalar types |
| `CreateDirectoryA`, `FindFirstFileA` | `mkdir`, `opendir`/`readdir` (`win32_dirent.h`) |
| `RtlCaptureStackBackTrace`, `GetModuleHandleA` | `backtrace(3)` and `dladdr` (`win32_backtrace.h`) |
| `GetLastError`, `SetLastError` | thread-local `errno` wrapper |

The Win32-compatible scalar types (`DWORD`, `LONG`, `HANDLE`, `SRWLOCK`, ...)
come from the same header. `LONG` is 32 bits, as on Windows.

## Graphics

There is no platform switch in the graphics path. `rsx_null_backend.c` is a
headless CPU rasteriser used as the fallback and by CI, and
`rsx_vulkan_backend.c` is the real backend (`-DPS3RECOMP_RSX_VULKAN=ON`). The
Vulkan backend opens an optional SDL2 window; without it, rendering and
readback stay offscreen.

## Input and audio

`cellPad` and `cellAudio` use SDL2 directly. There is no XInput or WASAPI path
to select.

## Filesystem

Guest paths use `/`. The VFS layer maps them under the title's root; symlinks
are relative so an installed tree can be moved.
