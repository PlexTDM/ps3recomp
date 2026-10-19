/* The vector forms of ppu_vmx.h's helpers (x86-64-v3, arm64 NEON) against
 * their scalar twins (name_ref): bit-identical results and return values,
 * VSCR[NJ] off and on, over random words and the special ones -- zeros,
 * denormals, infinities, NaNs quiet and signalling, the extremes. On x86-64
 * build with -march=x86-64-v3 (or -mavx2 -mfma); tools/test_ppu_vmx_vec.py
 * does. */
#include "../ppu_vmx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !(VMX_X86 || VMX_NEON)
int main(void) { printf("skip: no vector forms in this build\n"); return 0; }
#else
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}
static const uint32_t SPECIAL[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
    0x00800000u, 0x80800000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFF800001u,
    0x7FBFFFFFu, 0x7FC00001u, 0x4B000000u, 0xCF000000u, 0x4F000000u, 0x5F800000u,
    0x33800000u, 0x34000000u, 0x00000020u, 0x0000001Fu, 0xFFFFFFFFu, 0x7FFFFFFFu,
};
#define NSPECIAL (sizeof SPECIAL / sizeof SPECIAL[0])
static uint32_t word(void)
{
    const uint32_t r = rnd();
    switch (r & 7) {
    case 0: case 1: return SPECIAL[rnd() % NSPECIAL];
    case 2: {   /* a normal single near another: cancellation, ties */
        const uint32_t e = 0x30000000u + (rnd() % 0x1C000000u);
        return (rnd() & 0x80000000u) | (e & 0x7F800000u) | (rnd() & 0x7FFFFFu); }
    case 3: return rnd() & 0x80FFFFFFu;   /* denormals and tiny normals */
    default: return rnd() ^ (rnd() << 16);
    }
}
static void vec(uint8_t v[16])
{
    for (int i = 0; i < 4; i++) {
        const uint32_t w = word();
        v[4 * i] = (uint8_t)(w >> 24); v[4 * i + 1] = (uint8_t)(w >> 16);
        v[4 * i + 2] = (uint8_t)(w >> 8); v[4 * i + 3] = (uint8_t)w;
    }
}

static int g_fail, g_runs;
static void report(const char* name, const uint8_t* a, const uint8_t* b, const uint8_t* c,
                   const uint8_t* got, const uint8_t* want, uint32_t vscr)
{
    if (g_fail++ >= 12) return;
    printf("FAIL %s vscr=%08X\n  a", name, vscr);
    for (int i = 0; i < 4; i++) printf(" %08X", vmx_w(a, i));
    printf("\n  b");
    for (int i = 0; i < 4; i++) printf(" %08X", vmx_w(b, i));
    printf("\n  c");
    for (int i = 0; i < 4; i++) printf(" %08X", vmx_w(c, i));
    printf("\n  got ");
    for (int i = 0; i < 4; i++) printf(" %08X", vmx_w(got, i));
    printf("\n  want");
    for (int i = 0; i < 4; i++) printf(" %08X", vmx_w(want, i));
    printf("\n");
}

#define CHECK2(fn) do { uint8_t g[16], w[16]; \
    fn(g, a, b); fn##_ref(w, a, b); g_runs++; \
    if (memcmp(g, w, 16)) report(#fn, a, b, c, g, w, 0); } while (0)
#define CHECK2F(fn) do { uint8_t g[16], w[16]; \
    fn(g, a, b, vscr); fn##_ref(w, a, b, vscr); g_runs++; \
    if (memcmp(g, w, 16)) report(#fn, a, b, c, g, w, vscr); } while (0)
#define CHECK3F(fn) do { uint8_t g[16], w[16]; \
    fn(g, a, b, c, vscr); fn##_ref(w, a, b, c, vscr); g_runs++; \
    if (memcmp(g, w, 16)) report(#fn, a, b, c, g, w, vscr); } while (0)
#define CHECK1F(fn) do { uint8_t g[16], w[16]; \
    fn(g, b, vscr); fn##_ref(w, b, vscr); g_runs++; \
    if (memcmp(g, w, 16)) report(#fn, a, b, c, g, w, vscr); } while (0)
#define CHECKCMP(fn) do { uint8_t g[16], w[16]; \
    const uint32_t rg = fn(g, a, b), rw = fn##_ref(w, a, b); g_runs++; \
    if (memcmp(g, w, 16) || rg != rw) report(#fn, a, b, c, g, w, 0); } while (0)

int main(void)
{
    for (int n = 0; n < 400000; n++) {
        uint8_t a[16], b[16], c[16];
        vec(a); vec(b); vec(c);
        if (n & 1) memcpy(b, a, 16);           /* equal operands: max/min of zeros, compares */
        if ((n & 3) == 2) for (int i = 0; i < 16; i++) b[i] ^= (uint8_t)(i % 4 == 0 ? 0x80 : 0);
        for (int nj = 0; nj < 2; nj++) {
            const uint32_t vscr = nj ? VMX_NJ : 0;
            CHECK2F(vmx_vaddfp); CHECK2F(vmx_vsubfp);
            CHECK2F(vmx_vmaxfp); CHECK2F(vmx_vminfp);
            CHECK3F(vmx_vmaddfp); CHECK3F(vmx_vnmsubfp);
            CHECK1F(vmx_vrefp); CHECK1F(vmx_vrsqrtefp);
        }
        CHECK2(vmx_vslw); CHECK2(vmx_vsrw); CHECK2(vmx_vsraw);
        CHECKCMP(vmx_vcmpeqfp); CHECKCMP(vmx_vcmpgefp); CHECKCMP(vmx_vcmpgtfp);
        for (unsigned u = 0; u < 32; u += 1 + (n & 7)) {
            uint8_t g[16], w[16];
            vmx_vcfsx(g, b, u); vmx_vcfsx_ref(w, b, u); g_runs++;
            if (memcmp(g, w, 16)) report("vmx_vcfsx", a, b, c, g, w, u);
        }
    }
    printf("ppu_vmx %s forms: %d checks, %d failed\n", VMX_X86 ? "x86-64-v3" : "NEON", g_runs, g_fail);
    return g_fail ? 1 : 0;
}
#endif
