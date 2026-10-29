/*
 * ps3recomp - <dirent.h> for Windows
 *
 * runtime/ppu/ppu_fs.cpp walks host directories with the POSIX API --
 * opendir/readdir/closedir -- to answer the guest's cellFsOpendir. MSVC ships
 * no <dirent.h>, so that file did not compile on Windows without help, and
 * every game port quietly carried its own copy of a Win32 shim on the include
 * path to get past it. Putting one here deletes that copy from all of them.
 *
 * On POSIX this is just the system header, so call sites include this and stop
 * thinking about it.
 *
 * Scope is deliberately the four names ppu_fs.cpp actually uses, with d_name
 * as the only dirent field: no d_type, no rewinddir/seekdir/telldir, no
 * wide-character variant. FindFirstFileA reports "." and ".." exactly as
 * readdir does, so the caller's skip logic needs no special case.
 */
#if !defined(PS3RECOMP_WIN32_DIRENT_H)
#define PS3RECOMP_WIN32_DIRENT_H


#include <dirent.h>

#endif
