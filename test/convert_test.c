// SPDX-License-Identifier: GPL-2.0
/* Checks the 64-bit block conversion against the plain per-byte "^ 4" definition.
 * Run: cc -O2 -o /tmp/convert_test test/convert_test.c && /tmp/convert_test
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t __le64;
#define cpu_to_le64(x) (x) /* little-endian host */
static inline u64 ror64(u64 v, unsigned int s) { return v >> s | v << (64 - s); }

#include "../fl2000_convert.h"

static void reference(u8 *dst, size_t idx, const u32 *src, unsigned int pixels, unsigned int bpp)
{
	for (unsigned int x = 0; x < pixels; x++) {
		u32 p = bpp == 2 ? fl2000_rgb565(src[x]) : src[x];

		for (unsigned int i = 0; i < bpp; i++)
			dst[((idx + x) * bpp + i) ^ 4] = p >> (8 * i);
	}
}

int main(void)
{
	enum { MAX = 64 };
	u32 src[MAX];
	u8 a[(MAX + 16) * 3 + 8] __attribute__((aligned(8)));
	u8 b[sizeof(a)] __attribute__((aligned(8)));

	srand(1);
	for (int iter = 0; iter < 20000; iter++) {
		unsigned int bpp = 2 + (iter & 1);
		size_t idx = rand() % 16;
		unsigned int pixels = rand() % MAX;

		for (int i = 0; i < MAX; i++)
			src[i] = (u32)rand() << 1 ^ (u32)rand();
		memset(a, 0x5A, sizeof(a));
		memset(b, 0x5A, sizeof(b));
		fl2000_convert_line(a, idx, src, pixels, bpp);
		reference(b, idx, src, pixels, bpp);
		assert(!memcmp(a, b, sizeof(a)));
	}
	puts("convert_test: OK");
	return 0;
}
