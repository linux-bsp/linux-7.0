/* SPDX-License-Identifier: GPL-2.0 */
/* AM625 warm-reset log ABI. Keep the U-Boot and Linux copies identical. */
#ifndef BSP_BOOTLOG_LAYOUT_H
#define BSP_BOOTLOG_LAYOUT_H
#include <linux/types.h>
#define BL_BASE 0x9c900000UL
#define BL_BANK_SIZE 0x80000U
#define BL_TOTAL (2 * BL_BANK_SIZE)
#define BL_MAGIC 0x32474c42U
#define BL_RECORD_MAGIC 0x31474f4cU
#define BL_STAGES 4
struct bl_bank {
	u32 magic, version;
	u64 generation;
	u8 id[16];
	u32 crc, reserved[7];
};
struct bl_record {
	u32 magic, version, stage, length, dropped, crc, reserved[2];
	u8 data[];
};
static const unsigned int bl_offset[BL_STAGES] = {0x1000, 0x9000, 0x11000, 0x21000};
static const unsigned int bl_size[BL_STAGES] = {0x8000, 0x8000, 0x10000, 0x50000};
static inline u32 bl_crc(u32 crc, const void *data, unsigned int len)
{
	const u8 *p = data;
	crc = ~crc;
	while (len--) {
		int i;
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((0U - (crc & 1U)) & 0xedb88320U);
	}
	return ~crc;
}
static inline int bl_valid(const struct bl_bank *b)
{
	return b->magic == BL_MAGIC && b->version == 2 && b->generation &&
		b->crc == bl_crc(0, b, 32);
}
static inline int bl_record_valid(const struct bl_record *r, unsigned int stage)
{
	return r->magic == BL_RECORD_MAGIC && r->version == 2 &&
		r->stage == stage + 1 &&
		r->length <= bl_size[stage] - sizeof(*r) &&
		r->crc == bl_crc(0, r->data, r->length);
}
/* The previous committed prefix survives reset during the final append. */
static inline int bl_prefix_valid(const struct bl_record *r, unsigned int stage)
{
	return r->magic == BL_RECORD_MAGIC && r->version == 2 &&
		r->stage == stage + 1 && r->reserved[0] &&
		r->reserved[0] <= bl_size[stage] - sizeof(*r) &&
		r->reserved[1] == bl_crc(0, r->data, r->reserved[0]);
}
static inline int bl_latest(const struct bl_bank *a, const struct bl_bank *b)
{
	if (!bl_valid(a))
		return bl_valid(b) ? 1 : -1;
	return bl_valid(b) && b->generation > a->generation ? 1 : 0;
}
#endif
