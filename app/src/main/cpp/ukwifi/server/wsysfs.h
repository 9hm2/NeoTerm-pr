/* uKernel Wi-Fi — write the fake /sys/class/net + /sys/class/ieee80211 trees from
 * the live netdev/wiphy state, into host dirs bound into the guest by
 * UsbWifiBridge. Target dirs come from $UK_WIFI_SYSFS_NET / $UK_WIFI_SYSFS_PHY.
 * Called after a driver probes (so `ip link`, `if_nametoindex`, readdir of
 * /sys/class/net see the interface). No-op if the env paths are unset. */
#ifndef UKWIFI_WSYSFS_H
#define UKWIFI_WSYSFS_H
void ukw_wsysfs_refresh(void);
#endif
