/* test-nal.c — sanity tests for the AU assembler (host-buildable, no OBS deps). */
#include "dji-nal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct cap {
	int count;
	int keyframes;
	size_t last_size;
};

static void on_au(void *opaque, const uint8_t *au, size_t size, bool key)
{
	struct cap *c = opaque;
	c->count++;
	if (key)
		c->keyframes++;
	c->last_size = size;
	assert(size >= 4);
	assert(au[0] == 0 && au[1] == 0); /* starts with a start code */
}

/* Build a fake NAL: start code + header + payload (first payload byte MSB
 * controls first_mb_in_slice==0). */
static size_t put_nal(uint8_t *dst, uint8_t hdr, uint8_t first_payload,
		      size_t payload_len)
{
	size_t n = 0;
	dst[n++] = 0; dst[n++] = 0; dst[n++] = 0; dst[n++] = 1;
	dst[n++] = hdr;
	dst[n++] = first_payload;
	for (size_t i = 1; i < payload_len; i++)
		dst[n++] = (uint8_t)(i & 0x7f);
	return n;
}

int main(void)
{
	uint8_t stream[4096];
	size_t len = 0;

	/* AU1: SPS PPS IDR ; AU2: non-IDR ; AU3: non-IDR */
	len += put_nal(stream + len, 0x67, 0x80, 8);  /* SPS  */
	len += put_nal(stream + len, 0x68, 0x80, 4);  /* PPS  */
	len += put_nal(stream + len, 0x65, 0x88, 32); /* IDR, first_mb=0 */
	len += put_nal(stream + len, 0x41, 0x9a, 32); /* P, first_mb=0   */
	len += put_nal(stream + len, 0x41, 0x9a, 32); /* P, first_mb=0   */

	/* Test 1: whole stream in one push + flush */
	{
		struct dji_nal_asm a;
		struct cap c = {0};
		dji_nal_init(&a, DJI_NAL_H264);
		dji_nal_push(&a, stream, len, on_au, &c);
		dji_nal_flush(&a, on_au, &c);
		printf("one-push: %d AUs, %d key\n", c.count, c.keyframes);
		assert(c.count == 3);
		assert(c.keyframes == 1);
		assert(a.have_sps && a.have_pps);
		dji_nal_free(&a);
	}

	/* Test 2: byte-at-a-time (worst-case splitting) */
	{
		struct dji_nal_asm a;
		struct cap c = {0};
		dji_nal_init(&a, DJI_NAL_H264);
		for (size_t i = 0; i < len; i++)
			dji_nal_push(&a, stream + i, 1, on_au, &c);
		dji_nal_flush(&a, on_au, &c);
		printf("byte-wise: %d AUs, %d key\n", c.count, c.keyframes);
		assert(c.count == 3);
		assert(c.keyframes == 1);
		dji_nal_free(&a);
	}

	/* Test 3: multi-slice AU must not split (second slice first_mb!=0) */
	{
		uint8_t s2[4096];
		size_t l2 = 0;
		l2 += put_nal(s2 + l2, 0x65, 0x88, 16); /* IDR slice 1, first_mb=0 */
		l2 += put_nal(s2 + l2, 0x65, 0x22, 16); /* IDR slice 2, first_mb!=0 */
		l2 += put_nal(s2 + l2, 0x41, 0x9a, 16); /* next picture */

		struct dji_nal_asm a;
		struct cap c = {0};
		dji_nal_init(&a, DJI_NAL_H264);
		dji_nal_push(&a, s2, l2, on_au, &c);
		dji_nal_flush(&a, on_au, &c);
		printf("multi-slice: %d AUs, %d key\n", c.count, c.keyframes);
		assert(c.count == 2);
		assert(c.keyframes == 1);
		dji_nal_free(&a);
	}

	printf("all NAL tests passed\n");
	return 0;
}
