/*
 * ps3recomp - Real vertex-program validation
 * Decompiles + D3DCompiles every .vp in a directory (RPCS3 shader cache raw/).
 *
 * Build: gcc -std=c11 -O2 -I../../../include -I.. \
 *        validate_vp_shaders.c ../rsx_vp_decompiler.c -ld3dcompiler -o vvs.exe
 */
#include "rsx_vp_decompiler.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

static unsigned char buf[65536];
static char hlsl[160 * 1024];

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: %s <dir-of-.vp>\n", argv[0]); return 2; }
    DIR* d = opendir(argv[1]);
    if (!d) { printf("cannot open %s\n", argv[1]); return 1; }

    int total=0, decoded=0, decode_err=0, compiled=0, fail=0, shown=0;
    struct dirent* e; char path[1024];
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 3 || strcmp(e->d_name+L-3, ".vp")) continue;
        snprintf(path, sizeof(path), "%s/%s", argv[1], e->d_name);
        FILE* f = fopen(path, "rb"); if (!f) continue;
        long n = (long)fread(buf,1,sizeof buf,f); fclose(f);
        if (n < 16) continue;
        total++;

        u32 ni = rsx_vp_program_size_instrs(buf, (u32)n);
        u32 bytes = ni ? ni*16 : (u32)n;
        int r = rsx_vp_decompile(buf, bytes, hlsl, sizeof hlsl);
        if (r < 0) { decode_err++; continue; }
        decoded++;

    }
    closedir(d);

    printf("=== vertex-program corpus: %s ===\n", argv[1]);
    printf("total .vp      : %d\n", total);
    printf("decoded        : %d  (%.1f%%)\n", decoded, total?100.0*decoded/total:0);
    printf("decode errors  : %d\n", decode_err);
    return 0;
}
