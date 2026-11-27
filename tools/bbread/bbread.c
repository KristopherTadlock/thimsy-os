/*
 * bbread - read and decode a USB device's BOS descriptor (Billboard
 * Capability, Billboard AUM / Alternate Mode capability, Container ID,
 * USB 2.0 Extension, SuperSpeed, ...), the device descriptor and its
 * string descriptors through usbfs.
 *
 * Only standard GET_DESCRIPTOR control requests on endpoint 0 are issued
 * (USBDEVFS_CONTROL, recipient = device). The tool never claims an
 * interface, never detaches a driver, never sets a configuration and
 * never resets the device.
 *
 * Usage:
 *   bbread [-q] /dev/bus/usb/BBB/DDD     decode one device
 *   bbread [-q] /sys/bus/usb/devices/X   same, bus/dev taken from sysfs
 *   bbread -b                            decode every Billboard-class
 *                                        device found in /dev/bus/usb
 *   bbread -w SECONDS                    poll /dev/bus/usb every 200 ms
 *                                        for up to SECONDS, decode each
 *                                        newly seen Billboard device once
 *   bbread -d FILE                       decode a raw descriptor file
 *                                        (sysfs 'descriptors' or
 *                                        'bos_descriptors', or a dump)
 *   bbread --selftest                    decode a built-in sample BOS
 *
 * -q suppresses raw hex dumps.
 *
 * Build (static): see build.sh next to this file.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>

#define BBREAD_VERSION "1.0"

#define DT_DEVICE 0x01
#define DT_CONFIG 0x02
#define DT_STRING 0x03
#define DT_INTERFACE 0x04
#define DT_ENDPOINT 0x05
#define DT_IAD 0x0B
#define DT_BOS 0x0F
#define DT_DEVCAP 0x10
#define DT_SS_EP_COMP 0x30

#define CLASS_BILLBOARD 0x11

static int g_quiet;
static int g_fd = -1; /* usbfs fd used for string lookups; -1 = offline */
static uint16_t g_langid;
static int g_langid_done;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static void hexdump(const char *indent, const uint8_t *p, size_t n)
{
	size_t i, j;
	if (g_quiet)
		return;
	for (i = 0; i < n; i += 16) {
		printf("%s%04zx:", indent, i);
		for (j = i; j < i + 16; j++) {
			if (j < n)
				printf(" %02x", p[j]);
			else
				printf("   ");
		}
		printf("  |");
		for (j = i; j < i + 16 && j < n; j++)
			putchar(p[j] >= 0x20 && p[j] < 0x7f ? p[j] : '.');
		printf("|\n");
	}
}

static void sleep_ms(long ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* ------------------------------------------------------------------ */
/* usbfs control transfers (endpoint 0, standard GET_DESCRIPTOR only)  */

static int get_descriptor(int fd, uint8_t type, uint8_t index, uint16_t langid,
			  uint8_t *buf, uint16_t len)
{
	struct usbdevfs_ctrltransfer ct;
	int r = -1, tries;

	for (tries = 0; tries < 3; tries++) {
		memset(&ct, 0, sizeof(ct));
		ct.bRequestType = 0x80; /* IN | standard | device */
		ct.bRequest = 0x06;     /* GET_DESCRIPTOR */
		ct.wValue = (uint16_t)((type << 8) | index);
		ct.wIndex = langid;
		ct.wLength = len;
		ct.timeout = 1000;
		ct.data = buf;
		r = ioctl(fd, USBDEVFS_CONTROL, &ct);
		if (r >= 0)
			return r;
		if (errno != ETIMEDOUT && errno != EPROTO && errno != EILSEQ &&
		    errno != EPIPE && errno != EAGAIN && errno != EOVERFLOW)
			break;
		sleep_ms(50);
	}
	return -1;
}

/* Fetch string descriptor `idx` as UTF-8 into out. Returns 0 on success. */
static int get_string(int fd, uint8_t idx, char *out, size_t outsz)
{
	uint8_t buf[256];
	int n, i;
	size_t o = 0;

	out[0] = 0;
	if (idx == 0 || fd < 0)
		return -1;
	if (!g_langid_done) {
		g_langid_done = 1;
		n = get_descriptor(fd, DT_STRING, 0, 0, buf, 255);
		if (n >= 4 && buf[1] == DT_STRING)
			g_langid = le16(buf + 2);
		else
			g_langid = 0x0409;
	}
	n = get_descriptor(fd, DT_STRING, idx, g_langid, buf, 255);
	if (n < 2) {
		snprintf(out, outsz, "<error: %s>", n < 0 ? strerror(errno) : "short");
		return -1;
	}
	if (buf[1] != DT_STRING) {
		snprintf(out, outsz, "<bad type 0x%02x>", buf[1]);
		return -1;
	}
	if (buf[0] < n)
		n = buf[0];
	for (i = 2; i + 1 < n; i += 2) {
		unsigned c = le16(buf + i);
		if (c < 0x80) {
			if (o + 2 > outsz)
				break;
			out[o++] = (c >= 0x20 && c != 0x7f) ? (char)c : '?';
		} else if (c < 0x800) {
			if (o + 3 > outsz)
				break;
			out[o++] = (char)(0xc0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3f));
		} else {
			if (o + 4 > outsz)
				break;
			out[o++] = (char)(0xe0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
			out[o++] = (char)(0x80 | (c & 0x3f));
		}
	}
	out[o] = 0;
	return 0;
}

static void print_string_idx(const char *label, uint8_t idx)
{
	char s[512];
	printf("  %-24s %3u", label, idx);
	if (idx == 0) {
		printf("  (none)\n");
		return;
	}
	if (g_fd < 0) {
		printf("  (offline: string not fetched)\n");
		return;
	}
	get_string(g_fd, idx, s, sizeof(s));
	printf("  \"%s\"\n", s);
}

/* ------------------------------------------------------------------ */
/* decoders                                                            */

static const char *class_name(uint8_t c)
{
	switch (c) {
	case 0x00: return "(per interface)";
	case 0x01: return "Audio";
	case 0x02: return "CDC";
	case 0x03: return "HID";
	case 0x08: return "Mass Storage";
	case 0x09: return "Hub";
	case 0x0a: return "CDC Data";
	case 0x0e: return "Video";
	case 0x10: return "Audio/Video";
	case 0x11: return "Billboard";
	case 0x12: return "USB Type-C Bridge";
	case 0xdc: return "Diagnostic";
	case 0xe0: return "Wireless";
	case 0xef: return "Miscellaneous";
	case 0xfe: return "Application Specific";
	case 0xff: return "Vendor Specific";
	default: return "?";
	}
}

static void decode_device(const uint8_t *d, size_t n)
{
	printf("Device descriptor (%zu bytes)\n", n);
	hexdump("  ", d, n);
	if (n < 18 || d[1] != DT_DEVICE) {
		printf("  !! not a valid device descriptor\n");
		return;
	}
	printf("  bcdUSB                   %x.%02x\n", d[3], d[2]);
	printf("  bDeviceClass             0x%02x %s\n", d[4], class_name(d[4]));
	printf("  bDeviceSubClass          0x%02x\n", d[5]);
	printf("  bDeviceProtocol          0x%02x\n", d[6]);
	printf("  bMaxPacketSize0          %u\n", d[7]);
	printf("  idVendor:idProduct       %04x:%04x\n", le16(d + 8), le16(d + 10));
	printf("  bcdDevice                %x.%02x\n", d[13], d[12]);
	print_string_idx("iManufacturer", d[14]);
	print_string_idx("iProduct", d[15]);
	print_string_idx("iSerialNumber", d[16]);
	printf("  bNumConfigurations       %u\n", d[17]);
}

/* Configuration stream: config + interfaces + endpoints + class descs. */
static void decode_config_stream(const uint8_t *p, size_t n)
{
	size_t off = 0;
	while (off + 2 <= n) {
		uint8_t len = p[off], type = p[off + 1];
		const uint8_t *d = p + off;
		if (len < 2 || off + len > n) {
			printf("  !! truncated/invalid descriptor at offset %zu (bLength %u)\n",
			       off, len);
			hexdump("    ", d, n - off);
			return;
		}
		switch (type) {
		case DT_CONFIG:
			if (len >= 9) {
				printf("Configuration descriptor: wTotalLength %u, "
				       "bNumInterfaces %u, bConfigurationValue %u, "
				       "bmAttributes 0x%02x%s%s, bMaxPower %u (x2 mA USB2 / x8 mA USB3)\n",
				       le16(d + 2), d[4], d[5], d[7],
				       (d[7] & 0x40) ? " self-powered" : "",
				       (d[7] & 0x20) ? " remote-wakeup" : "", d[8]);
				print_string_idx("iConfiguration", d[6]);
			}
			break;
		case DT_INTERFACE:
			if (len >= 9) {
				printf(" Interface %u alt %u: class 0x%02x %s, sub 0x%02x, "
				       "proto 0x%02x, %u endpoint(s)\n",
				       d[2], d[3], d[5], class_name(d[5]), d[6], d[7], d[4]);
				print_string_idx(" iInterface", d[8]);
			}
			break;
		case DT_ENDPOINT:
			if (len >= 7)
				printf("  Endpoint 0x%02x: %s, wMaxPacketSize %u, bInterval %u\n",
				       d[2],
				       (const char *[]){"control", "isoc", "bulk", "interrupt"}[d[3] & 3],
				       le16(d + 4) & 0x7ff, d[6]);
			break;
		case DT_IAD:
			if (len >= 8)
				printf(" Interface association: first %u, count %u, class 0x%02x\n",
				       d[2], d[3], d[4]);
			break;
		case DT_SS_EP_COMP:
			printf("   SuperSpeed endpoint companion (%u bytes)\n", len);
			break;
		default:
			printf("  descriptor type 0x%02x, %u bytes\n", type, len);
			hexdump("    ", d, len);
			break;
		}
		off += len;
	}
}

static const char *bb_mode_state(unsigned v)
{
	switch (v & 3) {
	case 0: return "00 Unspecified Error";
	case 1: return "01 Configuration not attempted or exited";
	case 2: return "10 Configuration attempted but unsuccessful";
	default: return "11 Configuration successful";
	}
}

static const char *svid_name(uint16_t s)
{
	switch (s) {
	case 0xff00: return "USB PD SID";
	case 0xff01: return "DisplayPort (VESA)";
	case 0x8087: return "Intel (Thunderbolt)";
	case 0x2109: return "VIA Labs";
	case 0x05ac: return "Apple";
	case 0x0bda: return "Realtek";
	default: return "";
	}
}

static void pins(uint8_t m, char *out)
{
	int i, o = 0;
	for (i = 0; i < 6; i++)
		if (m & (1 << i))
			out[o++] = (char)('A' + i);
	if (!o)
		out[o++] = '-';
	out[o] = 0;
}

static void decode_dp_vdo(uint32_t v)
{
	char dfp[8], ufp[8];
	unsigned cap = v & 3;
	pins((v >> 8) & 0xff, dfp);
	pins((v >> 16) & 0xff, ufp);
	printf("      DP caps: port %s, signaling 0x%x%s, %s, USB2 signaling %s\n",
	       (const char *[]){"reserved", "UFP_D", "DFP_D", "UFP_D+DFP_D"}[cap],
	       (v >> 2) & 0xf, ((v >> 2) & 1) ? " (DP1.3)" : "",
	       (v & (1 << 6)) ? "receptacle" : "plug/captive",
	       (v & (1 << 7)) ? "not used" : "may be used");
	printf("      pin assignments: field[15:8] %s, field[23:16] %s%s\n", dfp, ufp,
	       (v & (1 << 6)) ? "" : "  (plug: UFP_D pins are in field[15:8])");
}

static void decode_billboard(const uint8_t *d, size_t len)
{
	unsigned nmodes, i, vconn, bcd;
	char s[512];

	if (len < 44) {
		printf("    !! Billboard capability too short (%zu < 44)\n", len);
		return;
	}
	nmodes = d[4];
	vconn = le16(d + 6);
	bcd = le16(d + 40);
	print_string_idx("  iAdditionalInfoURL", d[3]);
	printf("    bNumberOfAlternateModes  %u\n", nmodes);
	printf("    bPreferredAlternateMode  %u\n", d[5]);
	printf("    VCONN Power              0x%04x: ", vconn);
	if (vconn & 0x8000)
		printf("VCONN power not required\n");
	else
		printf("%s\n", (const char *[]){"1W", "1.5W", "2W", "3W", "4W", "5W",
						 "6W", "reserved"}[vconn & 7]);
	printf("    bmConfigured            ");
	for (i = 0; i < 32; i++)
		printf("%s%02x", (i && !(i % 16)) ? "\n                            " : " ",
		       d[8 + i]);
	printf("\n");
	printf("    bcdVersion               %x.%02x\n", bcd >> 8, bcd & 0xff);
	printf("    bAdditionalFailureInfo   0x%02x", d[42]);
	if (bcd < 0x0110)
		printf(" (reserved before Billboard 1.1)");
	if (d[42] & 1)
		printf(" [bit0: alt mode failed - insufficient power]");
	if (d[42] & 2)
		printf(" [bit1: USB PD communication failure]");
	if (d[42] & 0xfc)
		printf(" [reserved bits set]");
	printf("\n");
	printf("    bReserved                0x%02x\n", d[43]);
	if (nmodes > 0x34) {
		printf("    !! bNumberOfAlternateModes %u > 52 (MAX_NUM_ALT_OR_USB4_MODE)\n",
		       nmodes);
		nmodes = 0x34;
	}
	if (44 + 4 * nmodes > len) {
		printf("    !! bLength %zu too short for %u modes (need %u)\n", len, nmodes,
		       44 + 4 * nmodes);
		nmodes = (unsigned)((len - 44) / 4);
	}
	for (i = 0; i < nmodes; i++) {
		const uint8_t *m = d + 44 + 4 * i;
		unsigned st = (d[8 + (i >> 2)] >> ((i & 3) * 2)) & 3;
		printf("    Alternate mode %u%s\n", i,
		       i == d[5] ? " (preferred)" : "");
		printf("      wSVID                0x%04x %s\n", le16(m), svid_name(le16(m)));
		printf("      bAlternateMode       %u\n", m[2]);
		printf("      iAlternateModeString %u", m[3]);
		if (!m[3])
			printf("  (none)\n");
		else if (g_fd < 0)
			printf("  (offline: string not fetched)\n");
		else {
			get_string(g_fd, m[3], s, sizeof(s));
			printf("  \"%s\"\n", s);
		}
		printf("      bmConfigured state   %s\n", bb_mode_state(st));
	}
}

static void decode_devcap(const uint8_t *d, size_t len)
{
	uint8_t type = d[2];
	char s[512];

	switch (type) {
	case 0x02:
		printf("  USB 2.0 Extension capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		if (len >= 7)
			printf("    bmAttributes 0x%08x%s\n", le32(d + 3),
			       (le32(d + 3) & 2) ? " LPM" : "");
		break;
	case 0x03:
		printf("  SuperSpeed USB capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		if (len >= 10)
			printf("    bmAttributes 0x%02x, wSpeedsSupported 0x%04x, "
			       "bFunctionalitySupport %u, bU1DevExitLat %u us, wU2DevExitLat %u us\n",
			       d[3], le16(d + 4), d[6], d[7], le16(d + 8));
		break;
	case 0x04:
		printf("  Container ID capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		if (len >= 20) {
			const uint8_t *u = d + 4;
			printf("    ContainerID %02x%02x%02x%02x-%02x%02x-%02x%02x-"
			       "%02x%02x-%02x%02x%02x%02x%02x%02x\n",
			       u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9],
			       u[10], u[11], u[12], u[13], u[14], u[15]);
		}
		break;
	case 0x05:
		printf("  Platform capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		break;
	case 0x06:
		printf("  Power Delivery capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		break;
	case 0x07:
		printf("  Battery Info capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		break;
	case 0x08:
		printf("  PD Consumer Port capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		break;
	case 0x09:
		printf("  PD Provider Port capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		break;
	case 0x0a:
		printf("  SuperSpeedPlus capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		break;
	case 0x0d:
		printf("  Billboard capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		decode_billboard(d, len);
		break;
	case 0x0f:
		printf("  Billboard AUM (Alternate Mode) capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		if (len >= 8) {
			uint32_t vdo = le32(d + 4);
			printf("    bIndex               %u\n", d[3]);
			printf("    dwAlternateModeVdo   0x%08x\n", vdo);
			decode_dp_vdo(vdo);
			printf("      (VDO decoded as DP Capabilities; valid only if mode %u is SVID 0xff01)\n",
			       d[3]);
		}
		break;
	case 0x10:
		printf("  Configuration Summary capability (%zu bytes)\n", len);
		hexdump("    ", d, len);
		break;
	default:
		printf("  Device capability type 0x%02x (%zu bytes)\n", type, len);
		hexdump("    ", d, len);
		break;
	}
	(void)s;
}

/* Decode a full BOS (header + capabilities). Returns number of Billboard caps. */
static int decode_bos(const uint8_t *p, size_t n)
{
	size_t off;
	unsigned total, ncaps, seen = 0;
	int bb = 0;

	printf("BOS descriptor (%zu bytes)\n", n);
	hexdump("  ", p, n);
	if (n < 5 || p[1] != DT_BOS) {
		printf("  !! not a BOS descriptor\n");
		return 0;
	}
	total = le16(p + 2);
	ncaps = p[4];
	printf("  wTotalLength %u, bNumDeviceCaps %u\n", total, ncaps);
	if (total != n)
		printf("  !! wTotalLength %u != bytes available %zu\n", total, n);
	off = p[0] >= 5 ? p[0] : 5;
	while (off + 3 <= n) {
		uint8_t len = p[off];
		if (len < 3 || off + len > n) {
			printf("  !! invalid capability at offset %zu (bLength %u)\n", off, len);
			break;
		}
		if (p[off + 1] != DT_DEVCAP)
			printf("  !! descriptor type 0x%02x where 0x10 expected\n", p[off + 1]);
		if (p[off + 2] == 0x0d)
			bb++;
		decode_devcap(p + off, len);
		off += len;
		seen++;
	}
	if (seen != ncaps)
		printf("  !! decoded %u capabilities, header says %u\n", seen, ncaps);
	return bb;
}

/* Raw descriptor file: device desc + configs (sysfs 'descriptors', usbfs
 * read), or a BOS ('bos_descriptors'). */
static void decode_raw(const uint8_t *p, size_t n)
{
	if (n >= 2 && p[1] == DT_BOS) {
		decode_bos(p, n);
		return;
	}
	if (n >= 18 && p[0] == 18 && p[1] == DT_DEVICE) {
		decode_device(p, 18);
		if (n > 18) {
			printf("Configuration data (%zu bytes)\n", n - 18);
			hexdump("  ", p + 18, n - 18);
			decode_config_stream(p + 18, n - 18);
		}
		return;
	}
	printf("Unrecognized descriptor data (%zu bytes)\n", n);
	hexdump("  ", p, n);
	decode_config_stream(p, n);
}

/* ------------------------------------------------------------------ */
/* device access                                                       */

static int has_billboard_class(const uint8_t *p, size_t n)
{
	size_t off;
	if (n < 18 || p[1] != DT_DEVICE)
		return 0;
	if (p[4] == CLASS_BILLBOARD)
		return 1;
	for (off = 18; off + 2 <= n && p[off] >= 2; off += p[off])
		if (p[off + 1] == DT_INTERFACE && p[off] >= 9 && off + 9 <= n &&
		    p[off + 5] == CLASS_BILLBOARD)
			return 1;
	return 0;
}

static int do_device(const char *path)
{
	uint8_t raw[65536], bos[65536];
	ssize_t rn;
	int fd, n, ret = 0;
	unsigned total;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		int e = errno;
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "bbread: open %s: %s\n", path, strerror(errno));
			return 1;
		}
		fprintf(stderr, "bbread: %s opened read-only (%s); control transfers "
				"(BOS, strings) will likely fail\n",
			path, strerror(e));
	}
	g_fd = fd;
	g_langid_done = 0;

	printf("=== %s ===\n", path);

	/* usbfs read(): cached device descriptor + all configuration descriptors
	 * (no bus traffic). */
	rn = read(fd, raw, sizeof(raw));
	if (rn < 0) {
		printf("read() of cached descriptors failed: %s\n", strerror(errno));
		rn = 0;
	}

	/* Live device descriptor from the device itself. */
	{
		uint8_t dd[18];
		n = get_descriptor(fd, DT_DEVICE, 0, 0, dd, 18);
		if (n == 18)
			decode_device(dd, 18);
		else {
			printf("GET_DESCRIPTOR(DEVICE) failed (%d: %s); using cached copy\n", n,
			       n < 0 ? strerror(errno) : "short");
			if (rn >= 18)
				decode_device(raw, 18);
		}
	}
	if (rn > 18) {
		printf("Cached configuration data (%zd bytes)\n", rn - 18);
		hexdump("  ", raw + 18, (size_t)rn - 18);
		decode_config_stream(raw + 18, (size_t)rn - 18);
	}
	if (rn >= 18 && has_billboard_class(raw, (size_t)rn))
		printf("(device has Billboard class 0x11)\n");

	/* BOS: header first for wTotalLength, then the whole thing. */
	n = get_descriptor(fd, DT_BOS, 0, 0, bos, 5);
	if (n < 5) {
		printf("GET_DESCRIPTOR(BOS, 5) failed: %s (bcdUSB < 2.01 devices may STALL)\n",
		       n < 0 ? strerror(errno) : "short reply");
		ret = 2;
		goto out;
	}
	total = le16(bos + 2);
	if (bos[1] != DT_BOS || total < 5) {
		printf("BOS header looks wrong:");
		hexdump("  ", bos, 5);
		ret = 2;
		goto out;
	}
	n = get_descriptor(fd, DT_BOS, 0, 0, bos, (uint16_t)total);
	if (n < 0) {
		printf("GET_DESCRIPTOR(BOS, %u) failed: %s\n", total, strerror(errno));
		ret = 2;
		goto out;
	}
	if ((unsigned)n < total)
		printf("!! BOS short read: %d of %u bytes\n", n, total);
	if (!decode_bos(bos, (size_t)n))
		printf("(no Billboard capability in BOS)\n");
out:
	printf("\n");
	close(fd);
	g_fd = -1;
	return ret;
}

static int sysfs_to_dev(const char *dir, char *out, size_t outsz)
{
	char p[512];
	int bus = -1, dev = -1;
	FILE *f;

	snprintf(p, sizeof(p), "%s/busnum", dir);
	if ((f = fopen(p, "r"))) {
		if (fscanf(f, "%d", &bus) != 1)
			bus = -1;
		fclose(f);
	}
	snprintf(p, sizeof(p), "%s/devnum", dir);
	if ((f = fopen(p, "r"))) {
		if (fscanf(f, "%d", &dev) != 1)
			dev = -1;
		fclose(f);
	}
	if (bus < 0 || dev < 0)
		return -1;
	snprintf(out, outsz, "/dev/bus/usb/%03d/%03d", bus, dev);
	return 0;
}

/* Scan /dev/bus/usb for Billboard devices. seen[] tracks bus*1000+dev. */
static int scan(int *seen, int *nseen, int maxseen, int only_new)
{
	DIR *bd, *dd;
	struct dirent *be, *de;
	char path[600];
	uint8_t raw[8192];
	int found = 0;

	bd = opendir("/dev/bus/usb");
	if (!bd) {
		fprintf(stderr, "bbread: /dev/bus/usb: %s\n", strerror(errno));
		return -1;
	}
	while ((be = readdir(bd))) {
		char bdir[300];
		if (be->d_name[0] == '.')
			continue;
		snprintf(bdir, sizeof(bdir), "/dev/bus/usb/%s", be->d_name);
		dd = opendir(bdir);
		if (!dd)
			continue;
		while ((de = readdir(dd))) {
			int fd, key, i, dup = 0;
			ssize_t rn;
			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s", bdir, de->d_name);
			fd = open(path, O_RDONLY | O_CLOEXEC);
			if (fd < 0)
				continue;
			rn = read(fd, raw, sizeof(raw));
			close(fd);
			if (rn < 18 || !has_billboard_class(raw, (size_t)rn))
				continue;
			key = atoi(be->d_name) * 1000 + atoi(de->d_name);
			for (i = 0; i < *nseen; i++)
				if (seen[i] == key)
					dup = 1;
			if (only_new && dup)
				continue;
			if (!dup && *nseen < maxseen)
				seen[(*nseen)++] = key;
			found++;
			do_device(path);
			fflush(stdout);
		}
		closedir(dd);
	}
	closedir(bd);
	return found;
}

/* ------------------------------------------------------------------ */
/* self test                                                           */

static const uint8_t sample_bos[] = {
	/* BOS header: bLength 5, type 0x0F, wTotalLength 88, 4 caps */
	0x05, 0x0f, 0x58, 0x00, 0x04,
	/* USB 2.0 Extension: LPM */
	0x07, 0x10, 0x02, 0x02, 0x00, 0x00, 0x00,
	/* Container ID */
	0x14, 0x10, 0x04, 0x00,
	0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	/* Billboard capability, 1 mode: bLength 48 */
	0x30, 0x10, 0x0d,
	0x01,             /* iAdditionalInfoURL */
	0x01,             /* bNumberOfAlternateModes */
	0x00,             /* bPreferredAlternateMode */
	0x00, 0x80,       /* VCONN Power: not required */
	0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* bmConfigured */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0x21, 0x01,       /* bcdVersion 1.21 */
	0x01,             /* bAdditionalFailureInfo: insufficient power */
	0x00,             /* bReserved */
	0x01, 0xff, 0x00, 0x02, /* wSVID 0xff01, bAlternateMode 0, iString 2 */
	/* Billboard AUM capability: bIndex 0, VDO 0x00000c05 */
	0x08, 0x10, 0x0f, 0x00, 0x05, 0x0c, 0x00, 0x00,
};

static int selftest(void)
{
	const uint8_t *bb;
	int fails = 0, nbb;
	unsigned st;

	printf("bbread %s self-test: decoding built-in sample BOS\n\n", BBREAD_VERSION);
	if (sizeof(sample_bos) != 88) {
		printf("FAIL: sample size %zu != 88\n", sizeof(sample_bos));
		return 1;
	}
	nbb = decode_bos(sample_bos, sizeof(sample_bos));
	bb = sample_bos + 5 + 7 + 20;
#define CHECK(c, msg)                                   \
	do {                                            \
		if (!(c)) {                             \
			printf("FAIL: %s\n", msg);      \
			fails++;                        \
		} else                                  \
			printf("ok:   %s\n", msg);      \
	} while (0)
	printf("\nchecks:\n");
	CHECK(nbb == 1, "one Billboard capability found");
	CHECK(bb[2] == 0x0d && bb[0] == 48, "Billboard cap located, bLength 48");
	CHECK(bb[4] == 1, "bNumberOfAlternateModes == 1");
	CHECK(le16(bb + 6) & 0x8000, "VCONN power not required");
	st = bb[8] & 3;
	CHECK(st == 2, "mode 0 bmConfigured == 10 (attempted but unsuccessful)");
	CHECK(le16(bb + 40) == 0x0121, "bcdVersion 1.21");
	CHECK(bb[42] == 1, "bAdditionalFailureInfo bit0");
	CHECK(le16(bb + 44) == 0xff01, "mode 0 SVID 0xff01");
	CHECK(le32(sample_bos + 80 + 4) == 0x0c05, "AUM VDO 0x00000c05");
	{
		/* bit-pair extraction for modes 0..7 */
		uint8_t m[32] = {0xe4, 0x1b};
		unsigned want[8] = {0, 1, 2, 3, 3, 2, 1, 0}, i, ok = 1;
		for (i = 0; i < 8; i++)
			if (((m[i >> 2] >> ((i & 3) * 2)) & 3) != want[i])
				ok = 0;
		CHECK(ok, "bmConfigured bit-pair order (mode i = bits 2i..2i+1, LSB first)");
	}
	printf("\n%s (%d failure%s)\n", fails ? "SELFTEST FAILED" : "SELFTEST PASSED",
	       fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
	fprintf(stderr,
		"bbread %s - decode USB BOS / Billboard descriptors via usbfs\n"
		"usage: bbread [-q] /dev/bus/usb/BBB/DDD | /sys/bus/usb/devices/X\n"
		"       bbread [-q] -b              all Billboard-class devices\n"
		"       bbread [-q] -w SECONDS      wait for / watch Billboard devices\n"
		"       bbread [-q] -d FILE         decode raw descriptor file\n"
		"       bbread --selftest\n"
		"Only GET_DESCRIPTOR requests on ep0; never claims/configures/resets.\n",
		BBREAD_VERSION);
}

int main(int argc, char **argv)
{
	int i = 1;

	setvbuf(stdout, NULL, _IOLBF, 0);
	while (i < argc && !strcmp(argv[i], "-q")) {
		g_quiet = 1;
		i++;
	}
	if (i >= argc) {
		usage();
		return 64;
	}
	if (!strcmp(argv[i], "--selftest"))
		return selftest();
	if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
		usage();
		return 0;
	}
	if (!strcmp(argv[i], "-d")) {
		static uint8_t buf[1 << 20];
		size_t n = 0;
		ssize_t r;
		int fd;
		if (i + 1 >= argc) {
			usage();
			return 64;
		}
		fd = open(argv[i + 1], O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "bbread: %s: %s\n", argv[i + 1], strerror(errno));
			return 1;
		}
		while (n < sizeof(buf) && (r = read(fd, buf + n, sizeof(buf) - n)) > 0)
			n += (size_t)r;
		close(fd);
		printf("=== %s (offline decode; strings not fetched) ===\n", argv[i + 1]);
		decode_raw(buf, n);
		return 0;
	}
	if (!strcmp(argv[i], "-b")) {
		int seen[256], ns = 0, r = scan(seen, &ns, 256, 0);
		if (r == 0)
			printf("no Billboard-class device found in /dev/bus/usb\n");
		return r > 0 ? 0 : 3;
	}
	if (!strcmp(argv[i], "-w")) {
		int seen[256], ns = 0, total = 0, secs;
		struct timespec t0, t;
		if (i + 1 >= argc) {
			usage();
			return 64;
		}
		secs = atoi(argv[i + 1]);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		printf("watching /dev/bus/usb for Billboard devices for %d s\n", secs);
		for (;;) {
			int r = scan(seen, &ns, 256, 1);
			if (r > 0)
				total += r;
			clock_gettime(CLOCK_MONOTONIC, &t);
			if (t.tv_sec - t0.tv_sec >= secs)
				break;
			sleep_ms(200);
		}
		printf("watch done: %d Billboard device instance(s) decoded\n", total);
		return total ? 0 : 3;
	}
	{
		struct stat stt;
		char dev[64];
		const char *path = argv[i];
		if (stat(path, &stt) == 0 && S_ISDIR(stt.st_mode)) {
			if (sysfs_to_dev(path, dev, sizeof(dev))) {
				fprintf(stderr, "bbread: %s: no busnum/devnum\n", path);
				return 1;
			}
			path = dev;
		}
		return do_device(path);
	}
}
