#include <linux/types.h>
#include <linux/string.h>
#include <linux/ktime.h>
#include <linux/printk.h>

#include "ice_ticket.h"

int ice_ed25519_verify(const u8 *sig, const u8 *pk, const u8 *msg,
		       unsigned long long msglen);

static const u8 ICE_PUBKEY[32] = {
	0xfb, 0x4d, 0x7d, 0xd1, 0xe0, 0x0a, 0xc3, 0xcf,
	0x3c, 0xaa, 0xd5, 0x27, 0x4e, 0x0c, 0x79, 0x2a,
	0x2a, 0xef, 0x13, 0x36, 0x4b, 0xef, 0xb9, 0xb8,
	0x41, 0xcf, 0xd1, 0xe8, 0x35, 0x92, 0x07, 0x92
};

#define TICKET_LEN 151

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int hex_to_bytes(const char *s, u8 *out, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		int hi = hex_val(s[i * 2]);
		int lo = hex_val(s[i * 2 + 1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (u8)((hi << 4) | lo);
	}
	return 0;
}

int ice_ticket_verify(const char *ticket, int len)
{
	u8 sig[64];
	char dev_hex[9], exp_hex[9];
	char msg[32];
	u32 exp;
	int i, n;

	if (!ticket || len < TICKET_LEN)
		return -1;
	if (memcmp(ticket, "ICE2.", 5) != 0)
		return -1;
	if (ticket[13] != '.' || ticket[22] != '.')
		return -1;

	for (i = 5; i < TICKET_LEN; i++) {
		char ch = ticket[i];
		if (ch != '.' && hex_val(ch) < 0)
			return -1;
	}

	memcpy(dev_hex, ticket + 5, 8);
	dev_hex[8] = '\0';
	memcpy(exp_hex, ticket + 14, 8);
	exp_hex[8] = '\0';

	exp = 0;
	for (i = 0; i < 8; i++) {
		int v = hex_val(exp_hex[i]);
		if (v < 0)
			return -1;
		exp = (exp << 4) | (u32)v;
	}
	if (exp <= (u32)ktime_get_real_seconds())
		return -1;

	n = snprintf(msg, sizeof(msg), "ICE2.%s.%s", dev_hex, exp_hex);
	if (n != 22)
		return -1;

	if (hex_to_bytes(ticket + 23, sig, 64) != 0)
		return -1;

	if (ice_ed25519_verify(sig, ICE_PUBKEY, (const u8 *)msg, 22) != 0) {
		pr_warn("[IceDriver] bad ticket signature\n");
		return -1;
	}

	pr_info("[IceDriver] ticket ok, dev=%s exp=%u\n", dev_hex, exp);
	return 0;
}
