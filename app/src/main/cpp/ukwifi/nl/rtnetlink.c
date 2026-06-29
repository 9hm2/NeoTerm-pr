/* uKernel nl80211-bridge — rtnetlink (NETLINK_ROUTE) az `ip`-hez.
 *
 * Az airmon-ng `ip link show dev wlan0`-val ellenőrzi az interfészt, és
 * `ip link set wlan0mon up`-pal hozza fel a monitort. Az `ip` rtnetlinket
 * (NETLINK_ROUTE) használ, amit a host kernele a mi virtuális interfészeinkre
 * nem ismer. Ez a modul elkapja a NETLINK_ROUTE socketet, és az RTM_GETLINK-re
 * a wlan0 (+ wlan0mon, ha létezik) RTM_NEWLINK-választ adja, a SETLINK-et ACK-olja. */
#include "netlink_msg.h"
#include "uknl_common.h"
#include "userver_client.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <linux/rtnetlink.h>
#include <linux/if_addr.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <arpa/inet.h>

/* a mi interfészeink (a fake sysfs-ből) */
struct rtif { char name[16]; int ifindex; int artype; };

/* sysfs net dir the daemon writes (UsbWifiSysfsBridge), bound to /sys/class/net
 * in the guest — see wsysfs.c. Used to read iface type / detect a monitor vif. */
static const char *sysnet(void) { const char *d = getenv("UK_WIFI_SYSFS_NET"); return (d && *d) ? d : "/tmp/uksys/sys/class/net"; }
static int read_type(const char *name)
{
	char p[512]; snprintf(p, sizeof(p), "%s/%s/type", sysnet(), name);
	FILE *f = fopen(p, "r"); if (!f) return ARPHRD_ETHER;
	int t = ARPHRD_ETHER; if (fscanf(f, "%d", &t) != 1) t = ARPHRD_ETHER; fclose(f); return t;
}
static int list_ifaces(struct rtif *out, int max)
{
	int n = 0; struct stat st;
	/* lo elöl (index 1, ARPHRD_LOOPBACK) — különben az `ip link` a host lo-jára esik (EACCES) */
	if (n < max) { snprintf(out[n].name, 16, "lo"); out[n].ifindex = 1; out[n].artype = 772 /*ARPHRD_LOOPBACK*/; n++; }
	if (n < max) { snprintf(out[n].name, 16, "wlan0"); out[n].ifindex = UKNL_IFINDEX; out[n].artype = read_type("wlan0"); n++; }
	char monp[512]; snprintf(monp, sizeof(monp), "%s/wlan0mon", sysnet());
	if (stat(monp, &st) == 0 && n < max) {
		snprintf(out[n].name, 16, "wlan0mon"); out[n].ifindex = UKNL_IFINDEX + 1; out[n].artype = 803; n++;
	}
	return n;
}

/* egy RTM_NEWLINK üzenet egy interfészről (portid = az app port-id-je, szálanként) */
static void put_link(struct nl_buf *b, const struct rtif *ii, uint32_t seq, uint32_t portid, int multi)
{
	int is_lo = (strcmp(ii->name, "lo") == 0);
	struct uk_iface_info inf; uint8_t mac[6] = {0x24,0x2f,0xd0,0x8b,0x0c,0x9e};
	unsigned flags = IFF_BROADCAST | IFF_MULTICAST;
	uint32_t mtu = 1500;
	if (is_lo) { memset(mac, 0, 6); flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING; mtu = 65536; }
	else if (uk_get_iface(0, &inf) == 0) { memcpy(mac, inf.mac, 6); flags = inf.flags; }

	size_t off = b->len;
	struct nlmsghdr *nlh = nlb_reserve(b, NLMSG_HDRLEN);
	nlh->nlmsg_type = RTM_NEWLINK; nlh->nlmsg_flags = multi ? NLM_F_MULTI : 0;
	nlh->nlmsg_seq = seq; nlh->nlmsg_pid = portid;   /* az app port-id-je (ip ehhez hasonlít) */
	struct ifinfomsg *ifi = nlb_reserve(b, NLMSG_ALIGN(sizeof(*ifi)));
	memset(ifi, 0, sizeof(*ifi));
	ifi->ifi_family = AF_UNSPEC; ifi->ifi_type = ii->artype; ifi->ifi_index = ii->ifindex;
	ifi->ifi_flags = flags;   /* VALÓS flag-ek (IFF_UP admin + IFF_RUNNING carrier) */
	nla_put_str(b, IFLA_IFNAME, ii->name);
	nla_put_u32(b, IFLA_MTU, mtu);
	nla_put(b, IFLA_ADDRESS, mac, 6);
	uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
	nla_put(b, IFLA_BROADCAST, bcast, 6);
	/* operstate a VALÓS flag-ekből: RUNNING->UP, UP-de-nincs-carrier->DOWN, lent->DOWN */
	uint8_t oper = (flags & IFF_RUNNING) ? 6 /*IF_OPER_UP*/ : 2 /*IF_OPER_DOWN*/;
	nla_put_u8(b, IFLA_OPERSTATE, oper);
	/* az nlmsg_len beállítása (a nlb_reserve realloc-olhatott, újra lekérjük) */
	nlh = (struct nlmsghdr *)(b->data + off);
	nlh->nlmsg_len = b->len - off;
}

static int prefixlen(uint32_t mask) { int n = 0; while (mask) { n += mask & 1; mask >>= 1; } return n; }

/* RTM_NEWADDR a wlan0 VALÓS IP-jéről (ip addr) */
static void put_addr(struct nl_buf *b, uint32_t seq, uint32_t portid, int multi)
{
	struct uk_iface_info inf; if (uknl_iface_info(&inf) != 0 || inf.ip == 0) return;
	uint32_t mask = inf.netmask ? inf.netmask : 0xffffff00;
	uint32_t be_ip = htonl(inf.ip), be_bc = htonl((inf.ip & mask) | ~mask);
	size_t off = b->len;
	struct nlmsghdr *nlh = nlb_reserve(b, NLMSG_HDRLEN);
	nlh->nlmsg_type = RTM_NEWADDR; nlh->nlmsg_flags = multi ? NLM_F_MULTI : 0;
	nlh->nlmsg_seq = seq; nlh->nlmsg_pid = portid;
	struct ifaddrmsg *ifa = nlb_reserve(b, NLMSG_ALIGN(sizeof(*ifa)));
	memset(ifa, 0, sizeof(*ifa));
	ifa->ifa_family = AF_INET; ifa->ifa_prefixlen = prefixlen(mask); ifa->ifa_index = UKNL_IFINDEX;
	nla_put(b, IFA_ADDRESS, &be_ip, 4); nla_put(b, IFA_LOCAL, &be_ip, 4);
	nla_put(b, IFA_BROADCAST, &be_bc, 4); nla_put_str(b, IFA_LABEL, "wlan0");
	nlh = (struct nlmsghdr *)(b->data + off); nlh->nlmsg_len = b->len - off;
}

/* RTM_NEWROUTE: default (gw-n át) vagy a subnet-útvonal a wlan0-on (ip route) */
static void put_route(struct nl_buf *b, uint32_t seq, uint32_t portid, int multi, int is_default)
{
	struct uk_iface_info inf; if (uknl_iface_info(&inf) != 0 || inf.ip == 0) return;
	if (is_default && !inf.gw) return;
	uint32_t mask = inf.netmask ? inf.netmask : 0xffffff00;
	uint32_t oif = UKNL_IFINDEX, be_src = htonl(inf.ip);
	size_t off = b->len;
	struct nlmsghdr *nlh = nlb_reserve(b, NLMSG_HDRLEN);
	nlh->nlmsg_type = RTM_NEWROUTE; nlh->nlmsg_flags = multi ? NLM_F_MULTI : 0;
	nlh->nlmsg_seq = seq; nlh->nlmsg_pid = portid;
	struct rtmsg *rtm = nlb_reserve(b, NLMSG_ALIGN(sizeof(*rtm)));
	memset(rtm, 0, sizeof(*rtm));
	rtm->rtm_family = AF_INET; rtm->rtm_table = RT_TABLE_MAIN; rtm->rtm_protocol = RTPROT_BOOT;
	rtm->rtm_scope = is_default ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK; rtm->rtm_type = RTN_UNICAST;
	if (is_default) { uint32_t be_gw = htonl(inf.gw); rtm->rtm_dst_len = 0; nla_put(b, RTA_GATEWAY, &be_gw, 4); }
	else { uint32_t be_dst = htonl(inf.ip & mask); rtm->rtm_dst_len = prefixlen(mask); nla_put(b, RTA_DST, &be_dst, 4); }
	nla_put(b, RTA_OIF, &oif, 4); nla_put(b, RTA_PREFSRC, &be_src, 4);
	nlh = (struct nlmsghdr *)(b->data + off); nlh->nlmsg_len = b->len - off;
}

/* In-daemon rtnetlink dispatch: parse the guest's request datagram and build the
 * reply into resp (one or more netlink messages). Replaces the LD_PRELOAD thread
 * loop; the proot ferry hands us the raw bytes (UK_OP_RTNL) and returns resp. */
void ukw_rtnl_dispatch(const uint8_t *buf, size_t n, struct nl_buf *resp)
{
	uint32_t portid = 0;
	{
		size_t off = 0;
		while (off + NLMSG_HDRLEN <= (size_t)n) {
			struct nlmsghdr *nlh = (struct nlmsghdr *)(buf + off);
			if (nlh->nlmsg_len < NLMSG_HDRLEN || off + nlh->nlmsg_len > (size_t)n) break;
			uint16_t type = nlh->nlmsg_type;
			uint32_t seq = nlh->nlmsg_seq, pid = nlh->nlmsg_pid; portid = pid;
			int dump = (nlh->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP;

			if (type == RTM_GETLINK) {
				struct ifinfomsg *ifi = (struct ifinfomsg *)((char *)nlh + NLMSG_HDRLEN);
				int has_ifi = nlh->nlmsg_len >= NLMSG_HDRLEN + sizeof(*ifi);
				int want_idx = (!dump && has_ifi) ? ifi->ifi_index : 0;
				/* A non-dump GETLINK az interfészt NÉV szerint is kérheti
				 * (ip link set wlan0 up -> ifi_index=0 + IFLA_IFNAME=wlan0). A nevet
				 * ki kell olvasni és arra szűrni — különben az összes linket
				 * visszaadnánk, az ip csak az elsőt olvassa, a többi a stash-ben
				 * marad és deszinkronizálja a következő kérést (végtelen retry). */
				char want_name[16] = "";
				if (!dump && has_ifi) {
					struct rtattr *rta = (struct rtattr *)((char *)ifi + NLMSG_ALIGN(sizeof(*ifi)));
					int rlen = (int)nlh->nlmsg_len - NLMSG_HDRLEN - (int)NLMSG_ALIGN(sizeof(*ifi));
					for (; RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen))
						if (rta->rta_type == IFLA_IFNAME) {
							snprintf(want_name, sizeof want_name, "%s", (char *)RTA_DATA(rta)); break;
						}
				}
				struct rtif ifs[4]; int ni = list_ifaces(ifs, 4);
				int sent = 0;
				for (int i = 0; i < ni; i++) {
					if (want_idx && ifs[i].ifindex != want_idx) continue;
					if (want_name[0] && strcmp(ifs[i].name, want_name) != 0) continue;
					put_link(resp, &ifs[i], seq, portid, dump);
					sent++;
					if (!dump) break;   /* non-dump: pontosan egy link, mint a kernel */
				}
				if (dump) nlmsg_put_done(resp, seq, portid);
				else if (!sent) nlmsg_put_ack(resp, seq, portid, -19 /*ENODEV*/, nlh);
				if (uknl_debug) fprintf(stderr, "[uknl] rtnl GETLINK (idx=%d name=%s dump=%d) -> %d link\n", want_idx, want_name[0]?want_name:"-", dump, sent);
			} else if (type == RTM_GETADDR) {
				put_addr(resp, seq, portid, dump);
				if (dump) nlmsg_put_done(resp, seq, portid);
				if (uknl_debug) fprintf(stderr, "[uknl] rtnl GETADDR (dump=%d)\n", dump);
			} else if (type == RTM_GETROUTE) {
				put_route(resp, seq, portid, dump, 1);   /* default (gw) */
				put_route(resp, seq, portid, dump, 0);   /* subnet */
				if (dump) nlmsg_put_done(resp, seq, portid);
				if (uknl_debug) fprintf(stderr, "[uknl] rtnl GETROUTE (dump=%d)\n", dump);
			} else if (type == RTM_NEWLINK || type == RTM_SETLINK) {
				/* ip link set wlan0 up/down/mtu — TÉNYLEGESEN alkalmazzuk */
				struct ifinfomsg *ifi = (struct ifinfomsg *)((char *)nlh + NLMSG_HDRLEN);
				if (nlh->nlmsg_len >= NLMSG_HDRLEN + sizeof(*ifi)) {
					if (ifi->ifi_change & IFF_UP) uk_set_ifflags(0, (ifi->ifi_flags & IFF_UP) ? 1 : 0);
					struct rtattr *rta = (struct rtattr *)((char *)ifi + NLMSG_ALIGN(sizeof(*ifi)));
					int rlen = nlh->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(*ifi));
					for (; RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen))
						if (rta->rta_type == IFLA_MTU) uk_set_mtu(0, *(uint32_t *)RTA_DATA(rta));
				}
				nlmsg_put_ack(resp, seq, portid, 0, nlh);
				if (uknl_debug) fprintf(stderr, "[uknl] rtnl SETLINK -> alkalmazva\n");
			} else if (type == RTM_NEWADDR || type == RTM_DELADDR) {
				/* ip addr add/del 1.2.3.4/24 dev wlan0 */
				struct ifaddrmsg *ifa = (struct ifaddrmsg *)((char *)nlh + NLMSG_HDRLEN);
				if (type == RTM_DELADDR) uk_set_ifaddr(0, 0, 0);
				else if (nlh->nlmsg_len >= NLMSG_HDRLEN + sizeof(*ifa)) {
					int pl = ifa->ifa_prefixlen;
					uint32_t mask = pl ? (0xffffffffu << (32 - pl)) : 0xffffff00, ip = 0;
					struct rtattr *rta = (struct rtattr *)((char *)ifa + NLMSG_ALIGN(sizeof(*ifa)));
					int rlen = nlh->nlmsg_len - NLMSG_HDRLEN - NLMSG_ALIGN(sizeof(*ifa));
					for (; RTA_OK(rta, rlen); rta = RTA_NEXT(rta, rlen))
						if (rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS)
							ip = ntohl(*(uint32_t *)RTA_DATA(rta));
					if (ip) uk_set_ifaddr(0, ip, mask);
				}
				nlmsg_put_ack(resp, seq, portid, 0, nlh);
				if (uknl_debug) fprintf(stderr, "[uknl] rtnl %s -> alkalmazva\n", type == RTM_NEWADDR ? "NEWADDR" : "DELADDR");
			} else if (type == RTM_DELLINK) {
				nlmsg_put_ack(resp, seq, portid, 0, nlh);
			} else {
				if (nlh->nlmsg_flags & NLM_F_ACK) nlmsg_put_ack(resp, seq, portid, 0, nlh);
			}
			off += NLMSG_ALIGN(nlh->nlmsg_len);
		}
	}
}
