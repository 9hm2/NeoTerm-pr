/* uKernel Wi-Fi — minimal sysfs hooks used by nl80211_cmds.c. In NeoTerm the
 * /sys/class/net + /sys/class/ieee80211 trees are written by the daemon's wsysfs
 * writer (W2), so these per-vif hooks are thin (a refresh is enough). */
#ifndef UKWIFI_NL_SYSFS_H
#define UKWIFI_NL_SYSFS_H
void uknl_sysfs_add_iface(const char *name, int type);   /* type: 1=managed, 803=monitor */
void uknl_sysfs_del_iface(const char *name);
void uknl_sysfs_set_type(const char *name, int type);
#endif
