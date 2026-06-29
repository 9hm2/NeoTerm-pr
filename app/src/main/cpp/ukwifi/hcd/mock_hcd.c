/* uKernel — mock HCD backend.
 * A VALÓS RTL8811AU-ról (2357:011e) kiolvasott descriptorokat tükrözi, így
 * eszköz nélkül (CI/fejlesztés) is hűen enumerálható + probe-olható. */
#include "ukernel/hcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* valós device descriptor (18 bájt) */
static const uint8_t mock_dev[18] = {
	0x12,0x01,0x10,0x02,0x00,0x00,0x00,0x40,
	0x57,0x23,0x1e,0x01,0x00,0x02,0x01,0x02,0x03,0x01
};
/* valós config descriptor (60 bájt): 1 interfész, 6 endpoint */
static const uint8_t mock_cfg[60] = {
	0x09,0x02,0x3c,0x00,0x01,0x01,0x00,0xe0,0xfa,
	0x09,0x04,0x00,0x00,0x06,0xff,0xff,0xff,0x02,
	0x07,0x05,0x84,0x02,0x00,0x02,0x00,
	0x07,0x05,0x05,0x02,0x00,0x02,0x00,
	0x07,0x05,0x06,0x02,0x00,0x02,0x00,
	0x07,0x05,0x87,0x03,0x40,0x00,0x03,
	0x07,0x05,0x08,0x02,0x00,0x02,0x00,
	0x07,0x05,0x09,0x02,0x00,0x02,0x00
};

struct ukernel_hcd { int dummy; };
static struct ukernel_hcd mock_inst;

static struct ukernel_hcd *m_open(uint16_t vid, uint16_t pid)
{ fprintf(stderr, "uKernel/mock: nyit %04x:%04x (RTL8811AU emuláció)\n", vid, pid); return &mock_inst; }
static int m_dev(struct ukernel_hcd *d, const uint8_t **b, int *l) { (void)d; *b = mock_dev; *l = sizeof(mock_dev); return 0; }
static int m_cfg(struct ukernel_hcd *d, int i, const uint8_t **b, int *l) { (void)d; (void)i; *b = mock_cfg; *l = sizeof(mock_cfg); return 0; }
static int m_setcfg(struct ukernel_hcd *d, int c) { (void)d; (void)c; return 0; }
static int m_claim(struct ukernel_hcd *d, int i) { (void)d; (void)i; return 0; }
static int m_setif(struct ukernel_hcd *d, int i, int a) { (void)d; (void)i; (void)a; return 0; }

static int m_xfer(struct ukernel_hcd *d, struct ukernel_xfer *x)
{
	(void)d;
	if (x->type == UK_XFER_CONTROL && (x->bmRequestType & 0x80)) {
		/* standard GET_DESCRIPTOR válasz a saját tábláinkból */
		if (x->bRequest == 6 /*GET_DESCRIPTOR*/ && x->data) {
			int dt = (x->wValue >> 8) & 0xff;
			const uint8_t *src = NULL; int slen = 0;
			if (dt == 1) { src = mock_dev; slen = sizeof(mock_dev); }
			else if (dt == 2) { src = mock_cfg; slen = sizeof(mock_cfg); }
			if (src) {
				int n = x->wLength < slen ? x->wLength : slen;
				memcpy(x->data, src, n);
				x->actual_length = n;
				return 0;
			}
		}
		if (x->data && x->length) memset(x->data, 0, x->length);
		x->actual_length = x->wLength;
		return 0;
	}
	/* bulk/int IN: nullázott puffer; OUT: elnyelve */
	if ((x->ep & 0x80) && x->data && x->length) memset(x->data, 0, x->length);
	x->actual_length = x->length;
	return 0;
}
static void m_close(struct ukernel_hcd *d) { (void)d; }

static const struct ukernel_hcd_ops mock_ops = {
	.name = "mock",
	.open = m_open,
	.get_device_descriptor = m_dev,
	.get_config_descriptor = m_cfg,
	.set_configuration = m_setcfg,
	.claim_interface = m_claim,
	.set_interface = m_setif,
	.xfer = m_xfer,
	.submit = NULL,   /* szinkron fallback a usb_core-ban */
	.cancel = NULL,
	.pump_events = NULL,
	.close = m_close,
};

const struct ukernel_hcd_ops *ukernel_hcd_mock(void) { return &mock_ops; }
