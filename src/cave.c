// this file contains the code injected into vrcompositor.exe
// do's & don'ts to whichever poor sod maintains this:
// - don't do calls, globals, or constants. this must compile into something
// with no relocations
// - if you need utility functions (e.g.: Mul), forceinline them.

#include <intrin.h>
typedef struct {
    volatile int have;
    float headset_standing[12];
    int set;
    float parent[12];
    unsigned char copy[304];
} Cave;

static __forceinline void Mul(float* out, const float* a, const float* b) {
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++)
            out[i * 4 + j] =
            a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        out[i * 4 + 3] += a[i * 4 + 3];
    }
}

void RemoteVisit(Cave* cave, float* cur, const float* remote,
    const float* raw) {
    float t[12];
    int i;
    if (!cave->have) {
        cave->set = 0;
        for (i = 0; i < 12; i++)
            cur[i] = remote[i];

        return;
    }

    Mul(t, raw, cave->headset_standing);
    for (i = 0; i < 12; i++)
        cave->parent[i] = t[i];

    cave->set = 1;
    Mul(t, cave->parent, remote);
    for (i = 0; i < 12; i++)
        cur[i] = t[i];
}

const unsigned char* LaserRecord(Cave* cave, const unsigned char* rec) {
    float t[12], * m, * g = cave->parent;
    int i, j;
    if (!cave->set)
        return rec;

    __movsb(cave->copy, rec, 296);
    m = (float*)(cave->copy + 0x18);
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            t[i * 4 + j] = g[i] * m[j] + g[4 + i] * m[4 + j] + g[8 + i] * m[8 + j];
        }

        t[i * 4 + 3] -= g[i] * g[3] + g[4 + i] * g[7] + g[8 + i] * g[11];
    }

    for (i = 0; i < 12; i++)
        m[i] = t[i];
    return cave->copy;
}
