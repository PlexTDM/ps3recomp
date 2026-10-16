/* The fork's runtime symbols the SPU files reach that a standalone SPU test
 * does not otherwise build: guest time is host time (never paused), and every
 * page is committed. Tests that compile the SPU runtime add this file. */
#include <stdint.h>
int64_t ps3_guest_ticks(int64_t host_ticks) { return host_ticks; }
int vm_commit_reserved(void* p, uint64_t n) { (void)p; (void)n; return 1; }
