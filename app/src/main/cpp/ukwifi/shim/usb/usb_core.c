/* uKernel — USB core: driver-registry, URB-modell, enumeráció+probe.
 * Az I/O-t a HCD bridge-re (ukernel/hcd.h) képezzük. */
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "ukernel/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ===== aktuális HCD ===== */
static const struct ukernel_hcd_ops *g_ops;
static struct ukernel_hcd *g_hcd;
static pthread_t g_pump;
static int g_pump_run;
static int g_dev_class = -1, g_iface_class = -1;   /* az utolsó enumerált eszköz osztálya (hotplug-routinghoz) */

static void *pump_main(void *arg)
{ (void)arg; while (g_pump_run && g_ops && g_ops->pump_events) g_ops->pump_events(g_hcd, 100); return NULL; }

void ukernel_set_hcd(const struct ukernel_hcd_ops *ops) { g_ops = ops; }

/* ===== driver registry ===== */
#define MAX_DRV 16
static struct usb_driver *g_drv[MAX_DRV];
static int g_ndrv;

int usb_register_driver(struct usb_driver *drv, struct module *owner, const char *name)
{
	(void)owner; (void)name;
	if (g_ndrv < MAX_DRV) g_drv[g_ndrv++] = drv;
	printk(KERN_INFO "uKernel/usb: usb_register(%s)\n", drv->name ? drv->name : "?");
	return 0;
}
void usb_deregister(struct usb_driver *drv)
{ for (int i = 0; i < g_ndrv; i++) if (g_drv[i] == drv) g_drv[i] = NULL; }

/* ===== URB ===== */
struct urb *usb_alloc_urb(int iso, gfp_t f) { (void)iso; (void)f; return calloc(1, sizeof(struct urb)); }
void usb_free_urb(struct urb *u) { if (u) { free(u->hcpriv); free(u); } }
struct urb *usb_get_urb(struct urb *u) { return u; }

/* async completion bridge: HCD kész -> URB completion */
static void urb_async_done(struct ukernel_async *a, void *user)
{
	struct urb *urb = user;
	urb->actual_length = a->xfer.actual_length;
	urb->status = a->status;
	if (urb->complete) urb->complete(urb);
}

int usb_submit_urb(struct urb *urb, gfp_t mem_flags)
{
	(void)mem_flags;
	if (!g_ops || !g_hcd) return -ENODEV;
	struct ukernel_async *a = urb->hcpriv;
	if (!a) { a = calloc(1, sizeof(*a)); urb->hcpriv = a; }
	int type = usb_pipetype(urb->pipe);
	a->xfer.type = type;
	a->xfer.ep = usb_pipeendpoint(urb->pipe) | (usb_pipein(urb->pipe) ? 0x80 : 0);
	a->xfer.data = urb->transfer_buffer;
	a->xfer.length = urb->transfer_buffer_length;
	a->xfer.timeout_ms = 0;
	a->complete = urb_async_done;
	a->user = urb;
	if (g_ops->submit) return g_ops->submit(g_hcd, a);
	/* fallback: szinkron xfer + azonnali completion */
	int r = g_ops->xfer(g_hcd, &a->xfer);
	a->status = r; urb_async_done(a, urb);
	return 0;
}
void usb_kill_urb(struct urb *urb)
{ if (g_ops && g_ops->cancel && urb && urb->hcpriv) g_ops->cancel(g_hcd, urb->hcpriv); }
int usb_unlink_urb(struct urb *urb) { usb_kill_urb(urb); return 0; }
void usb_kill_anchored_urbs(struct usb_anchor *a) { (void)a; }

/* ===== szinkron I/O ===== */
int usb_control_msg(struct usb_device *dev, unsigned int pipe, __u8 request,
		__u8 requesttype, __u16 value, __u16 index, void *data, __u16 size, int timeout)
{
	(void)dev; (void)pipe;
	if (!g_ops || !g_hcd) return -ENODEV;
	struct ukernel_xfer x = {0};
	x.type = UK_XFER_CONTROL;
	x.bmRequestType = requesttype;
	x.bRequest = request;
	x.wValue = value; x.wIndex = index; x.wLength = size;
	x.ep = (requesttype & USB_DIR_IN) ? 0x80 : 0x00;
	x.data = data; x.length = size;
	x.timeout_ms = timeout > 0 ? timeout : 1000;
	int r = g_ops->xfer(g_hcd, &x);
	return r ? r : x.actual_length;
}

int usb_bulk_msg(struct usb_device *dev, unsigned int pipe, void *data, int len,
		int *actual, int timeout)
{
	(void)dev;
	if (!g_ops || !g_hcd) return -ENODEV;
	struct ukernel_xfer x = {0};
	x.type = usb_pipetype(pipe) == USB_ENDPOINT_XFER_INT ? UK_XFER_INT : UK_XFER_BULK;
	x.ep = usb_pipeendpoint(pipe) | (usb_pipein(pipe) ? 0x80 : 0);
	x.data = data; x.length = len;
	x.timeout_ms = timeout > 0 ? timeout : 1000;
	int r = g_ops->xfer(g_hcd, &x);
	if (actual) *actual = x.actual_length;
	return r;
}
int usb_interrupt_msg(struct usb_device *dev, unsigned int pipe, void *data, int len,
		int *actual, int timeout)
{ return usb_bulk_msg(dev, pipe, data, len, actual, timeout); }

int usb_set_interface(struct usb_device *dev, int iface, int alt)
{ (void)dev; return (g_ops && g_ops->set_interface) ? g_ops->set_interface(g_hcd, iface, alt) : 0; }
int usb_clear_halt(struct usb_device *dev, int pipe)
{
	(void)dev;
	struct ukernel_xfer x = {0};
	x.type = UK_XFER_CONTROL; x.bmRequestType = 0x02; x.bRequest = USB_REQ_CLEAR_FEATURE;
	x.wValue = 0; x.wIndex = usb_pipeendpoint(pipe) | (usb_pipein(pipe) ? 0x80 : 0);
	return g_ops ? g_ops->xfer(g_hcd, &x) : -ENODEV;
}
int usb_reset_device(struct usb_device *dev) { (void)dev; return 0; }

int usb_get_descriptor(struct usb_device *dev, unsigned char type, unsigned char index,
		void *buf, int size)
{ return usb_control_msg(dev, 0, USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
		(type << 8) | index, 0, buf, size, 1000); }

int usb_string(struct usb_device *dev, int index, char *buf, size_t size)
{
	unsigned char tmp[256];
	int r = usb_control_msg(dev, 0, USB_REQ_GET_DESCRIPTOR, USB_DIR_IN,
			(USB_DT_STRING << 8) | index, 0x0409, tmp, sizeof(tmp), 1000);
	if (r < 2) return r < 0 ? r : -EIO;
	size_t o = 0;
	for (int i = 2; i < r && o + 1 < size; i += 2) buf[o++] = tmp[i];
	buf[o] = '\0';
	return (int)o;
}

void *usb_alloc_coherent(struct usb_device *dev, size_t size, gfp_t f, dma_addr_t *dma)
{ (void)dev; (void)f; void *p = malloc(size); if (dma) *dma = (dma_addr_t)(uintptr_t)p; return p; }
void usb_free_coherent(struct usb_device *dev, size_t size, void *addr, dma_addr_t dma)
{ (void)dev; (void)size; (void)dma; free(addr); }

/* ===== descriptor-parszolás + enumeráció + probe ===== */
/* a TÉNYLEGESEN illeszkedő id_table-bejegyzést adja vissza (NULL ha nincs).
 * Fontos: a driver az id->driver_info-ból veszi a chip-típust (8812/8821/...),
 * ezért a pontos bejegyzést kell átadni a probe-nak, nem az id_table[0]-t. */
/* ===== A VALÓDI Linux USB-azonosítás (drivers/usb/core/driver.c) — bitre azonos =====
 * Ezek a kernel usb_match_* függvényei (a usb-serial core search_serial_device-je is ezeket hívja),
 * így az eszköz-driver illesztés UGYANÚGY működik, mint a rendes kernelben (match_flags: vendor,
 * product, bcdDevice-tartomány, eszköz-osztály/alosztály/protokoll, interfész-osztály/alosztály/
 * protokoll/szám), a VENDOR_SPEC speciális szabállyal együtt. */
#ifndef USB_CLASS_VENDOR_SPEC
#define USB_CLASS_VENDOR_SPEC 0xff
#endif
int usb_match_device(struct usb_device *dev, const struct usb_device_id *id)
{
	if ((id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) && id->idVendor != dev->descriptor.idVendor) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_PRODUCT) && id->idProduct != dev->descriptor.idProduct) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_LO) && (id->bcdDevice_lo > dev->descriptor.bcdDevice)) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_HI) && (id->bcdDevice_hi < dev->descriptor.bcdDevice)) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_CLASS) && (id->bDeviceClass != dev->descriptor.bDeviceClass)) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_SUBCLASS) && (id->bDeviceSubClass != dev->descriptor.bDeviceSubClass)) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_DEV_PROTOCOL) && (id->bDeviceProtocol != dev->descriptor.bDeviceProtocol)) return 0;
	return 1;
}
int usb_match_one_id_intf(struct usb_device *dev, struct usb_host_interface *intf, const struct usb_device_id *id)
{
	/* Az interfész osztály/alosztály/protokoll/szám SOHA nem illesztendő, ha az eszköz-osztály
	 * Vendor-Specific, KIVÉVE ha a bejegyzés megadja a Vendor ID-t. */
	if (dev->descriptor.bDeviceClass == USB_CLASS_VENDOR_SPEC &&
	    !(id->match_flags & USB_DEVICE_ID_MATCH_VENDOR) &&
	    (id->match_flags & (USB_DEVICE_ID_MATCH_INT_CLASS | USB_DEVICE_ID_MATCH_INT_SUBCLASS |
				USB_DEVICE_ID_MATCH_INT_PROTOCOL | USB_DEVICE_ID_MATCH_INT_NUMBER)))
		return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_CLASS) && (id->bInterfaceClass != intf->desc.bInterfaceClass)) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_SUBCLASS) && (id->bInterfaceSubClass != intf->desc.bInterfaceSubClass)) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_PROTOCOL) && (id->bInterfaceProtocol != intf->desc.bInterfaceProtocol)) return 0;
	if ((id->match_flags & USB_DEVICE_ID_MATCH_INT_NUMBER) && (id->bInterfaceNumber != intf->desc.bInterfaceNumber)) return 0;
	return 1;
}
int usb_match_one_id(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_host_interface *intf;
	struct usb_device *dev;
	if (id == NULL) return 0;
	intf = interface->cur_altsetting;
	dev = interface_to_usbdev(interface);
	if (!usb_match_device(dev, id)) return 0;
	return usb_match_one_id_intf(dev, intf, id);
}
const struct usb_device_id *usb_match_id(struct usb_interface *interface, const struct usb_device_id *id)
{
	if (id == NULL) return NULL;
	for (; id->idVendor || id->idProduct || id->bDeviceClass || id->bInterfaceClass || id->driver_info; id++)
		if (usb_match_one_id(interface, id)) return id;
	return NULL;
}

/* config blob -> usb_host_config (interfész[0] + endpointok) */
static int parse_config(const uint8_t *blob, int len, struct usb_host_config *cfg)
{
	if (len < 9) return -EINVAL;
	memcpy(&cfg->desc, blob, 9);
	struct usb_interface *intf = calloc(1, sizeof(*intf));
	struct usb_host_interface *alt = calloc(1, sizeof(*alt));
	intf->altsetting = alt; intf->cur_altsetting = alt; intf->num_altsetting = 1;
	cfg->interface[0] = intf;

	int i = 9, ep_i = 0;
	int extra_start = -1;                      /* az interfész-desc utáni class-specifikus (pl. CDC) blokk eleje */
	struct usb_host_endpoint *eps = NULL;
	while (i + 2 <= len) {
		uint8_t blen = blob[i], btype = blob[i + 1];
		if (blen == 0) break;
		if (btype == USB_DT_INTERFACE && i + 9 <= len) {
			memcpy(&alt->desc, blob + i, 9);
			eps = calloc(alt->desc.bNumEndpoints ? alt->desc.bNumEndpoints : 1, sizeof(*eps));
			alt->endpoint = eps; ep_i = 0;
			extra_start = i + 9;               /* az EXTRA (CDC funkc. desc.) itt kezdődik */
		} else if (btype == USB_DT_ENDPOINT && eps && i + 7 <= len) {
			/* az ELSŐ endpoint lezárja az interfész EXTRA-blokkját (a cdc-acm ezt parse-olja:
			 * intf->altsetting->extra/extralen → union/call-mgmt CDC-descriptorok). */
			if (extra_start >= 0 && i > extra_start) { alt->extra = (unsigned char *)(blob + extra_start); alt->extralen = i - extra_start; extra_start = -1; }
			else extra_start = -1;
			memcpy(&eps[ep_i].desc, blob + i, blen < 7 ? blen : 7);
			ep_i++;
		}
		i += blen;
	}
	return 0;
}

int ukernel_usb_enumerate_and_probe(uint16_t vid, uint16_t pid)
{
	if (!g_ops) { printk(KERN_ERR "uKernel/usb: nincs HCD backend\n"); return -ENODEV; }
	g_hcd = g_ops->open(vid, pid);
	if (!g_hcd) { printk(KERN_ERR "uKernel/usb: eszköz megnyitása sikertelen (%04x:%04x)\n", vid, pid); return -ENODEV; }

	/* pump szál async I/O-hoz */
	if (g_ops->pump_events) { g_pump_run = 1; pthread_create(&g_pump, NULL, pump_main, NULL); }

	struct usb_device *dev = calloc(1, sizeof(*dev));
	dev->hcd_priv = g_hcd; dev->speed = USB_SPEED_HIGH; dev->devnum = 1;
	dev->state = USB_STATE_CONFIGURED;   /* különben a usb-serial serial_write -ENODEV-et ad (NOTATTACHED) */

	const uint8_t *db; int dl;
	if (g_ops->get_device_descriptor(g_hcd, &db, &dl) || dl < 18) {
		printk(KERN_ERR "uKernel/usb: device descriptor hiba\n"); return -EIO; }
	memcpy(&dev->descriptor, db, 18);
	printk(KERN_INFO "uKernel/usb: eszköz %04x:%04x, %d konfiguráció\n",
	       dev->descriptor.idVendor, dev->descriptor.idProduct, dev->descriptor.bNumConfigurations);

	const uint8_t *cb; int cl;
	struct usb_host_config *cfg = calloc(1, sizeof(*cfg));
	if (!g_ops->get_config_descriptor(g_hcd, 0, &cb, &cl) && cl >= 9) {
		parse_config(cb, cl, cfg);
		dev->config = cfg; dev->actconfig = cfg;
		g_dev_class = dev->descriptor.bDeviceClass;
		g_iface_class = cfg->interface[0]->cur_altsetting->desc.bInterfaceClass;
		if (g_ops->set_configuration) g_ops->set_configuration(g_hcd, cfg->desc.bConfigurationValue);
		if (g_ops->claim_interface) g_ops->claim_interface(g_hcd, 0);
		printk(KERN_INFO "uKernel/usb: config #%d, %d interfész, %d endpoint\n",
		       cfg->desc.bConfigurationValue, cfg->desc.bNumInterfaces,
		       cfg->interface[0]->cur_altsetting->desc.bNumEndpoints);
	}

	/* product string (best effort) */
	if (dev->descriptor.iProduct)
		usb_string(dev, dev->descriptor.iProduct, dev->product, sizeof(dev->product));

	/* probe a regisztrált driverekkel — a VALÓDI kernel usb_match_id-vel (interfész-alapú azonosítás).
	 * EGY interfész EGY drivernek: az első sikeres bind után megállunk. */
	int bound = 0;
	struct usb_interface *intf = cfg->interface[0];
	if (intf) intf->usb_dev = dev;   /* interface_to_usbdev a usb_match_one_id-ben */
	for (int i = 0; i < g_ndrv && intf; i++) {
		struct usb_driver *drv = g_drv[i];
		if (!drv || !drv->probe) continue;
		const struct usb_device_id *mid = usb_match_id(intf, drv->id_table);
		if (!mid) continue;
		intf->dev.driver = &drv->driver;   /* a usb-serial core search_serial_device-je ezt nézi
						    * (to_usb_driver(iface->dev.driver) == a usb-serial usb_driver) */
		printk(KERN_INFO "uKernel/usb: %s -> probe() (driver_info=%lu)\n", drv->name, mid->driver_info);
		int r = drv->probe(intf, mid);
		printk(KERN_INFO "uKernel/usb: %s probe -> %d\n", drv->name, r);
		if (r == 0) { bound++; break; }   /* az interfész le van foglalva -> nincs több driver */
	}
	return bound;
}

/* Tiszta leállás (a bridge destruktora hívja kilépéskor): CSAK a pump-szálat állítja le (join), hogy
 * az ne fusson a process-teardown alatt. Az usbfs-fd-ket NEM zárjuk explicit — azokat a KERNEL zárja
 * le a process-kilépéskor (függő URB-ek cancel + interfész-felszabadítás), tisztán hagyva az eszközt a
 * következő futáshoz. */
void ukernel_usb_shutdown(void)
{
	if (g_pump_run) { g_pump_run = 0; pthread_join(g_pump, NULL); }
}

/* Az utolsó enumerált eszköz USB-osztálya (hotplug class-routinghoz): a daemon ebből dönti el,
 * serial / mass-storage(0x08) / wireless(0xe0) / net stb. Az interfész-osztály a megbízhatóbb
 * (sok eszköz bDeviceClass=0 "per-interface"). */
int ukernel_usb_iface_class(void) { return g_iface_class; }
int ukernel_usb_dev_class(void)   { return g_dev_class; }

/* A jelenlegi HCD-eszköz ELENGEDÉSE (interfész-release + close) — a hotplug-daemon ezt hívja egy
 * NEM-soros eszközre, hogy szabadon hagyja a megfelelő osztály-shimnek (FS-bridge / net-bridge),
 * ami közvetlenül (usbfs-en) megnyitja az eszközt. */
void ukernel_usb_close(void)
{
	if (g_pump_run) { g_pump_run = 0; pthread_join(g_pump, NULL); }
	if (g_ops && g_ops->close && g_hcd) g_ops->close(g_hcd);
	g_hcd = NULL;
}

/* Board power hooks the vendor driver calls around USB init. They live in the
 * driver's platform/*.c, which we don't build (no specific board); the chip is
 * already powered as a plain USB dongle, so these are no-ops. Stubbed here so the
 * dlopen'd driver resolves them. */
int  platform_wifi_power_on(void)  { return 0; }
void platform_wifi_power_off(void) { }
