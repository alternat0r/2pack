/*
 * crt.c - minimal CRT replacements for the no-CRT stub.
 *
 * The stub links with /NODEFAULTLIB, so nothing from the C runtime is
 * available. LzmaDec.c (and the loader) call memcpy, which must be
 * provided here. This file is compiled as C: defining a function that
 * the runtime headers already declared as extern "C" is legal in C but
 * would be an error in the C++ loader translation unit.
 */
#include <windows.h>

void* memcpy(void* dst, const void* src, SIZE_T n)
{
    BYTE* d = (BYTE*)dst;
    const BYTE* s = (const BYTE*)src;
    for (SIZE_T i = 0; i < n; ++i)
        d[i] = s[i];
    return dst;
}
