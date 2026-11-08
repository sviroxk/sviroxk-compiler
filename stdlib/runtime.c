/* SC runtime: panic handler.
 *
 * Every runtime trap (array bounds, invalid shift, overflow, OOM, ...)
 * routes through __sc_panic. The message is printed to stderr and the
 * process exits with a non-zero status so callers can detect a panic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void __sc_panic(const char* message) {
    fprintf(stderr, "%s\n", message);
    fflush(stderr);
    exit(1);
}

/* Concatenate two C strings. Returns a freshly malloc'd NUL-terminated copy;
 * the caller (SC codegen) is responsible for the lifetime of the returned
 * pointer. Strings are immutable in SC, so the result is the only owner. */
char* __sc_concat(const char* a, const char* b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char* out = (char*)malloc(la + lb + 1);
    if (!out) {
        fprintf(stderr, "SC runtime error: out of memory in concat\n");
        exit(1);
    }
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

/* Deep equality for POD structs. SC structs are plain aggregates of
 * integers, floats and pointers, so a byte-wise comparison over the whole
 * allocation is correct (and fast). Returns 1 if equal, 0 otherwise. */
int __sc_struct_eq(const void* a, const void* b, size_t size) {
    return memcmp(a, b, size) == 0 ? 1 : 0;
}