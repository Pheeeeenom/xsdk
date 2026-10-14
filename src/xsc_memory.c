#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xsc_private.h"

enum {
	CHUNK_BYTES = 64 * 1024,
	ALIGN_BYTES = 16,
	// Python's repr gives no float more digits than these (float.__repr__, 'r').
	REPR_DIGITS = 17,
	// "%.9f" of the largest double.
	ROUND_TEXT_BYTES = 400,
};

struct XscChunk {
	XscChunk *next;
	size_t used;
	size_t size;
};

static size_t header_bytes(void)
{
	return (sizeof(XscChunk) + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES;
}

void *xsdk_xsc_alloc(Xsc *x, size_t bytes)
{
	bytes = (bytes + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES;
	XscChunk *chunk = x->chunks;
	if (!chunk || chunk->size - chunk->used < bytes) {
		const size_t size = bytes > CHUNK_BYTES ? bytes : CHUNK_BYTES;
		chunk = malloc(header_bytes() + size);
		if (!chunk) {
			xsdk_xsc_fail(x, XSDK_XSC_OUT_OF_MEMORY, "out of memory for %zu bytes", bytes);
			return NULL;
		}
		chunk->used = 0;
		chunk->size = size;
		// A large block goes behind the current chunk, which keeps its free space.
		if (x->chunks && size > CHUNK_BYTES) {
			chunk->next = x->chunks->next;
			x->chunks->next = chunk;
		} else {
			chunk->next = x->chunks;
			x->chunks = chunk;
		}
	}
	uint8_t *const memory = (uint8_t *)chunk + header_bytes() + chunk->used;
	chunk->used += bytes;
	memset(memory, 0, bytes);
	return memory;
}

bool xsdk_xsc_fail(Xsc *x, XsdkXscStatus status, const char *format, ...)
{
	if (x->status != XSDK_XSC_OK)
		return false;
	x->status = status;
	va_list args;
	va_start(args, format);
	(void)vsnprintf(x->message, sizeof x->message, format, args);
	va_end(args);
	return false;
}

bool xsdk_xsc_room(Xsc *x, void *items, size_t count, size_t *capacity, size_t item_bytes)
{
	if (count < *capacity)
		return true;
	const size_t grown = *capacity ? *capacity * 2 : 8;
	void *old;
	memcpy(&old, items, sizeof old);
	void *const bigger = xsdk_xsc_alloc(x, grown * item_bytes);
	if (!bigger)
		return false;
	if (count)
		memcpy(bigger, old, count * item_bytes);
	memcpy(items, &bigger, sizeof bigger);
	*capacity = grown;
	return true;
}

char *xsdk_xsc_copy_text(Xsc *x, const char *text, size_t length_bytes)
{
	char *const copy = xsdk_xsc_alloc(x, length_bytes + 1);
	if (copy)
		memcpy(copy, text, length_bytes);
	return copy;
}

void xsdk_xsc_release(Xsc *x)
{
	XscChunk *chunk = x->chunks;
	while (chunk) {
		XscChunk *const next = chunk->next;
		free(chunk);
		chunk = next;
	}
	x->chunks = NULL;
}

// The fewest significant digits that read back as value, and where the decimal point goes after the first.
static int shortest_digits(double value, char *digits, int *exponent)
{
	char text[REPR_DIGITS + 16];
	for (int precision = 1; precision <= REPR_DIGITS; precision++) {
		(void)snprintf(text, sizeof text, "%.*e", precision - 1, value);
		if (strtod(text, NULL) == value || precision == REPR_DIGITS)
			break;
	}
	int count = 0;
	const char *p = text;
	for (; *p && *p != 'e'; p++) {
		if (*p >= '0' && *p <= '9')
			digits[count++] = *p;
	}
	*exponent = *p == 'e' ? atoi(p + 1) : 0;
	while (count > 1 && digits[count - 1] == '0')
		count--;
	digits[count] = 0;
	return count;
}

static void append(char *text, size_t text_bytes, size_t *length, const char *part, int part_length)
{
	for (int i = 0; i < part_length && *length + 1 < text_bytes; i++)
		text[(*length)++] = part[i];
	text[*length] = 0;
}

static void append_zeros(char *text, size_t text_bytes, size_t *length, int count)
{
	for (int i = 0; i < count; i++)
		append(text, text_bytes, length, "0", 1);
}

void xsdk_xsc_float_repr(double value, char *text, size_t text_bytes)
{
	if (isnan(value) || isinf(value)) {
		(void)snprintf(text, text_bytes, "%s", isnan(value) ? "nan" : value < 0 ? "-inf" : "inf");
		return;
	}
	char digits[REPR_DIGITS + 2] = "0";
	int exponent = 0;
	int count = 1;
	if (value != 0)
		count = shortest_digits(fabs(value), digits, &exponent);
	const int point = exponent + 1;
	size_t length = 0;
	text[0] = 0;
	if (signbit(value))
		append(text, text_bytes, &length, "-", 1);
	// Python writes an exponent past these (pystrtod.c, format_float_short).
	if (point <= -4 || point > 16) {
		append(text, text_bytes, &length, digits, 1);
		if (count > 1) {
			append(text, text_bytes, &length, ".", 1);
			append(text, text_bytes, &length, digits + 1, count - 1);
		}
		char tail[8];
		const int written = snprintf(tail, sizeof tail, "e%c%02d", exponent < 0 ? '-' : '+', abs(exponent));
		append(text, text_bytes, &length, tail, written);
		return;
	}
	if (point <= 0) {
		append(text, text_bytes, &length, "0.", 2);
		append_zeros(text, text_bytes, &length, -point);
		append(text, text_bytes, &length, digits, count);
		return;
	}
	if (point < count) {
		append(text, text_bytes, &length, digits, point);
		append(text, text_bytes, &length, ".", 1);
		append(text, text_bytes, &length, digits + point, count - point);
		return;
	}
	append(text, text_bytes, &length, digits, count);
	append_zeros(text, text_bytes, &length, point - count);
	append(text, text_bytes, &length, ".0", 2);
}

double xsdk_xsc_round9(double value)
{
	if (!isfinite(value))
		return value;
	char text[ROUND_TEXT_BYTES];
	(void)snprintf(text, sizeof text, "%.9f", value);
	return strtod(text, NULL);
}

double xsdk_xsc_float_bits(uint32_t bits)
{
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

uint32_t xsdk_xsc_bits_of_float(double value)
{
	const float narrow = (float)value;
	uint32_t bits;
	memcpy(&bits, &narrow, sizeof bits);
	return bits;
}
