// packtest.cpp - a small self-contained program used to verify 2pack.
// It writes a known result to a file so the test does not depend on a console
// (the unpacker stub is a GUI-subsystem host process).
#include <windows.h>

static char g_buf[64];
static const char* g_msg = "hello-from-packed-exe";

// Exercise a few kernel32 imports + relocations (g_buf/g_msg live in .data).
static DWORD checksum(const char* s) {
    DWORD h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

int main(void) {
    // Copy into the global buffer (relocation on g_buf).
    DWORD i = 0;
    while (g_msg[i]) { g_buf[i] = g_msg[i]; ++i; }
    g_buf[i] = 0;

    // Build the output text.
    char out[128];
    int n = 0;
    const char* pre = "packed-ok:";
    for (const char* p = pre; *p; ++p) out[n++] = *p;
    for (const char* p = g_buf; *p; ++p) out[n++] = *p;
    out[n++] = '|';
    // append hex checksum
    char hex[16];
    DWORD h = checksum(g_buf);
    int k = 0;
    for (int b = 28; b >= 0; b -= 4) {
        int v = (h >> b) & 0xF;
        hex[k++] = (char)(v < 10 ? '0' + v : 'a' + v - 10);
    }
    for (const char* p = hex; *p; ++p) out[n++] = *p;
    out[n++] = '\n';

    // Write to a file in the current directory.
    HANDLE f = CreateFileA("packtest_out.txt", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, out, (DWORD)n, &wr, NULL);
    CloseHandle(f);
    if (!ok || wr != (DWORD)n) return 2;
    return 0;
}
