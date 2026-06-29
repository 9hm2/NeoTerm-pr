/* uKernel Wi-Fi — real request_firmware(): load the .bin from the guest distro's
 * /lib/firmware (where `firmware-realtek` etc. install it). The driver runs in the
 * app-side daemon, so it can't see the guest's /lib/firmware at that path — the
 * bridge passes the distro rootfs firmware dir as $UK_WIFI_FW_DIR. The name is
 * used verbatim (e.g. "rtlwifi/rtl8821aufw.bin"). Replaces the compile-only stub.
 *
 * (The vendor rtl8812au reads its efuse via filp_open/kernel_read instead; that
 * path is real-file too — point its EFUSE_MAP_PATH at a readable absolute path.) */
#include <linux/firmware.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int ukw_fw_load(const struct firmware **fw, const char *name)
{
	if (fw) *fw = NULL;
	if (!fw || !name || !*name) return -2 /* -ENOENT */;
	const char *dir = getenv("UK_WIFI_FW_DIR");
	if (!dir || !*dir) dir = "/lib/firmware";
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	int fd = open(path, O_RDONLY);
	if (fd < 0) { fprintf(stderr, "ukwifi/fw: nincs firmware: %s\n", path); return -2; }
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size <= 0) { close(fd); return -2; }
	uint8_t *buf = (uint8_t *) malloc((size_t) st.st_size);
	if (!buf) { close(fd); return -12 /* -ENOMEM */; }
	off_t got = 0;
	while (got < st.st_size) { ssize_t k = read(fd, buf + got, (size_t)(st.st_size - got)); if (k <= 0) break; got += k; }
	close(fd);
	if (got != st.st_size) { free(buf); return -5 /* -EIO */; }
	struct firmware *f = (struct firmware *) malloc(sizeof *f);
	if (!f) { free(buf); return -12; }
	f->size = (size_t) st.st_size; f->data = buf;
	*fw = f;
	fprintf(stderr, "ukwifi/fw: betöltve %s (%zu bájt)\n", path, f->size);
	return 0;
}

int request_firmware(const struct firmware **fw, const char *name, struct device *dev)
{ (void) dev; return ukw_fw_load(fw, name); }
int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *dev)
{ (void) dev; return ukw_fw_load(fw, name); }
int request_firmware_direct(const struct firmware **fw, const char *name, struct device *dev)
{ (void) dev; return ukw_fw_load(fw, name); }
void release_firmware(const struct firmware *fw)
{ if (fw) { free((void *) fw->data); free((void *) fw); } }
