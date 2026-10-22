/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XRGB8888 to FL2000 stream format conversion. Kernel-independent so that
 * test/convert_test.c can check it in userspace.
 */

#ifndef __FL2000_CONVERT_H__
#define __FL2000_CONVERT_H__

static inline u32 fl2000_rgb565(u32 p)
{
	return ((p & 0x00F80000) >> 8) | ((p & 0x0000FC00) >> 5) | ((p & 0x000000F8) >> 3);
}

/* HW expects 32-bit halves of every 64-bit word of the stream swapped, hence "^ 4" */
static void fl2000_put_pixel(u8 *dst, size_t off, u32 p, unsigned int bytes_pix)
{
	if (bytes_pix == 2)
		p = fl2000_rgb565(p);

	for (unsigned int i = 0; i < bytes_pix; i++, off++, p >>= 8)
		dst[off ^ 4] = p;
}

/* Convert @pixels starting at pixel index @idx of the frame. Whole 64-bit words (8 pixels of
 * RGB888 or 4 of RGB565) are packed in registers and stored with halves swapped (ror64 by 32)
 */
static void fl2000_convert_line(u8 *dst, size_t idx, const u32 *src, unsigned int pixels,
				unsigned int bytes_pix)
{
	unsigned int block = bytes_pix == 3 ? 8 : 4;
	unsigned int x = 0;

	for (; x < pixels && (idx + x) % block; x++)
		fl2000_put_pixel(dst, (idx + x) * bytes_pix, src[x], bytes_pix);

	for (; x + block <= pixels; x += block) {
		const u32 *p = src + x;
		__le64 *q = (__le64 *)(dst + (idx + x) * bytes_pix);

		if (bytes_pix == 3) {
			u64 c0 = p[0] & 0xFFFFFF, c1 = p[1] & 0xFFFFFF, c2 = p[2] & 0xFFFFFF;
			u64 c3 = p[3] & 0xFFFFFF, c4 = p[4] & 0xFFFFFF, c5 = p[5] & 0xFFFFFF;
			u64 c6 = p[6] & 0xFFFFFF, c7 = p[7] & 0xFFFFFF;

			q[0] = cpu_to_le64(ror64(c0 | c1 << 24 | c2 << 48, 32));
			q[1] = cpu_to_le64(ror64(c2 >> 16 | c3 << 8 | c4 << 32 | c5 << 56, 32));
			q[2] = cpu_to_le64(ror64(c5 >> 8 | c6 << 16 | c7 << 40, 32));
		} else {
			q[0] = cpu_to_le64(ror64((u64)fl2000_rgb565(p[0]) |
						 (u64)fl2000_rgb565(p[1]) << 16 |
						 (u64)fl2000_rgb565(p[2]) << 32 |
						 (u64)fl2000_rgb565(p[3]) << 48, 32));
		}
	}

	for (; x < pixels; x++)
		fl2000_put_pixel(dst, (idx + x) * bytes_pix, src[x], bytes_pix);
}

#endif /* __FL2000_CONVERT_H__ */
