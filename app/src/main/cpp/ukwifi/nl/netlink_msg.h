/* uKernel nl80211-bridge — netlink/genetlink üzenet-építő és -parszoló segédek.
 * Moduláris: ezt használja minden parancs-handler a válaszok felépítéséhez. */
#ifndef UKNL_NETLINK_MSG_H
#define UKNL_NETLINK_MSG_H

#include <stdint.h>
#include <stddef.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>

/* Növekvő válasz-puffer (egy vagy több netlink-üzenet). */
struct nl_buf {
	uint8_t *data;
	size_t   len;
	size_t   cap;
};

void nlb_init(struct nl_buf *b);
void nlb_free(struct nl_buf *b);
void *nlb_reserve(struct nl_buf *b, size_t n);   /* n bájt helyet ad, 4-igazítva */

/* Egy genetlink-üzenet kezdése; visszaadja az nlmsghdr offsetjét (a végén nlmsg_end-hez). */
size_t nlmsg_begin(struct nl_buf *b, uint16_t type, uint16_t flags,
                   uint32_t seq, uint32_t pid, uint8_t genl_cmd, uint8_t version);
void   nlmsg_end(struct nl_buf *b, size_t hdr_off);  /* beállítja az nlmsg_len-t */

/* Attribútumok (nlattr TLV). */
void nla_put(struct nl_buf *b, uint16_t type, const void *data, uint16_t len);
void nla_put_u8(struct nl_buf *b, uint16_t type, uint8_t v);
void nla_put_u16(struct nl_buf *b, uint16_t type, uint16_t v);
void nla_put_u32(struct nl_buf *b, uint16_t type, uint32_t v);
void nla_put_u64(struct nl_buf *b, uint16_t type, uint64_t v);
void nla_put_str(struct nl_buf *b, uint16_t type, const char *s);
void nla_put_flag(struct nl_buf *b, uint16_t type);
size_t nla_nest_begin(struct nl_buf *b, uint16_t type);  /* visszaadja az nlattr offsetjét */
void   nla_nest_end(struct nl_buf *b, size_t off);

/* Lezáró/hibajelző üzenetek. */
void nlmsg_put_done(struct nl_buf *b, uint32_t seq, uint32_t pid);
void nlmsg_put_ack(struct nl_buf *b, uint32_t seq, uint32_t pid, int err,
                   const struct nlmsghdr *orig);

/* Bejövő kérés parszolása. */
struct nl_req {
	const struct nlmsghdr  *nlh;
	const struct genlmsghdr *gnlh;
	const struct nlattr    *attrs;   /* a genlmsghdr utáni attribútumok */
	int                     attrs_len;
};
int nl_req_parse(const uint8_t *msg, size_t len, struct nl_req *req);
/* Egy adott típusú attribútum keresése (NULL ha nincs). */
const struct nlattr *nla_find(const struct nlattr *head, int len, uint16_t type);
static inline void *nla_data2(const struct nlattr *a) { return (void *)((const char *)a + NLA_HDRLEN); }

#endif
