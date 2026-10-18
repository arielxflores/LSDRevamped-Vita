/* LZ4 HC block compression for tools/mkpak.py (loaded with ctypes): out must hold LZ4_compressBound(n). */
#include "../../src/lz4.h"
#include "lz4hc.h"
int lz4pack(const char *src, int n, char *dst, int cap) { return LZ4_compress_HC(src, dst, n, cap, LZ4HC_CLEVEL_MAX); }
int lz4bound(int n) { return LZ4_compressBound(n); }
int lz4unpack(const char *src, int n, char *dst, int size) { return LZ4_decompress_safe(src, dst, n, size); }
