/* uKernel nl80211-bridge — netlink/genetlink üzenet-helper implementáció. */
#include "netlink_msg.h"
#include <stdlib.h>
#include <string.h>

#define ALIGN4(n) (((n) + 3) & ~3u)

void nlb_init(struct nl_buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
void nlb_free(struct nl_buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

void *nlb_reserve(struct nl_buf *b, size_t n)
{
	size_t need = b->len + n;
	if (need > b->cap) {
		size_t nc = b->cap ? b->cap : 4096;
		while (nc < need) nc *= 2;
		b->data = realloc(b->data, nc);
		b->cap = nc;
	}
	void *p = b->data + b->len;
	memset(p, 0, n);
	b->len += n;
	return p;
}

size_t nlmsg_begin(struct nl_buf *b, uint16_t type, uint16_t flags,
                   uint32_t seq, uint32_t pid, uint8_t genl_cmd, uint8_t version)
{
	size_t off = b->len;
	struct nlmsghdr *nlh = nlb_reserve(b, NLMSG_HDRLEN);
	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = flags;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = pid;
	struct genlmsghdr *g = nlb_reserve(b, ALIGN4(sizeof(*g)));
	g->cmd = genl_cmd;
	g->version = version;
	return off;
}

void nlmsg_end(struct nl_buf *b, size_t hdr_off)
{
	struct nlmsghdr *nlh = (struct nlmsghdr *)(b->data + hdr_off);
	nlh->nlmsg_len = b->len - hdr_off;
}

void nla_put(struct nl_buf *b, uint16_t type, const void *data, uint16_t len)
{
	struct nlattr *a = nlb_reserve(b, NLA_HDRLEN);
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	if (len) { void *p = nlb_reserve(b, ALIGN4(len)); memcpy(p, data, len); }
}
void nla_put_u8(struct nl_buf *b, uint16_t t, uint8_t v)  { nla_put(b, t, &v, sizeof(v)); }
void nla_put_u16(struct nl_buf *b, uint16_t t, uint16_t v){ nla_put(b, t, &v, sizeof(v)); }
void nla_put_u32(struct nl_buf *b, uint16_t t, uint32_t v){ nla_put(b, t, &v, sizeof(v)); }
void nla_put_u64(struct nl_buf *b, uint16_t t, uint64_t v){ nla_put(b, t, &v, sizeof(v)); }
void nla_put_str(struct nl_buf *b, uint16_t t, const char *s){ nla_put(b, t, s, strlen(s) + 1); }
void nla_put_flag(struct nl_buf *b, uint16_t t) { nla_put(b, t, NULL, 0); }

size_t nla_nest_begin(struct nl_buf *b, uint16_t type)
{
	size_t off = b->len;
	struct nlattr *a = nlb_reserve(b, NLA_HDRLEN);
	a->nla_type = type | NLA_F_NESTED;
	return off;
}
void nla_nest_end(struct nl_buf *b, size_t off)
{
	struct nlattr *a = (struct nlattr *)(b->data + off);
	a->nla_len = b->len - off;
}

void nlmsg_put_done(struct nl_buf *b, uint32_t seq, uint32_t pid)
{
	struct nlmsghdr *nlh = nlb_reserve(b, NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(int)));
	nlh->nlmsg_len = NLMSG_HDRLEN + sizeof(int);
	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = pid;
	*(int *)((char *)nlh + NLMSG_HDRLEN) = 0;
}

void nlmsg_put_ack(struct nl_buf *b, uint32_t seq, uint32_t pid, int err,
                   const struct nlmsghdr *orig)
{
	struct nlmsghdr *nlh = nlb_reserve(b, NLMSG_HDRLEN + NLMSG_ALIGN(sizeof(struct nlmsgerr)));
	nlh->nlmsg_len = NLMSG_HDRLEN + sizeof(struct nlmsgerr);
	nlh->nlmsg_type = NLMSG_ERROR;
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_seq = seq;
	nlh->nlmsg_pid = pid;
	struct nlmsgerr *e = (struct nlmsgerr *)((char *)nlh + NLMSG_HDRLEN);
	e->error = err;
	if (orig) memcpy(&e->msg, orig, sizeof(*orig));
}

int nl_req_parse(const uint8_t *msg, size_t len, struct nl_req *req)
{
	if (len < NLMSG_HDRLEN) return -1;
	const struct nlmsghdr *nlh = (const struct nlmsghdr *)msg;
	req->nlh = nlh;
	if (nlh->nlmsg_len < NLMSG_HDRLEN + GENL_HDRLEN) { req->gnlh = NULL; return 0; }
	req->gnlh = (const struct genlmsghdr *)((const char *)nlh + NLMSG_HDRLEN);
	req->attrs = (const struct nlattr *)((const char *)req->gnlh + GENL_HDRLEN);
	req->attrs_len = (int)nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
	if (req->attrs_len < 0) req->attrs_len = 0;
	return 0;
}

const struct nlattr *nla_find(const struct nlattr *head, int len, uint16_t type)
{
	const struct nlattr *a = head;
	int rem = len;
	while (rem >= (int)NLA_HDRLEN && rem >= a->nla_len) {
		if ((a->nla_type & NLA_TYPE_MASK) == type) return a;
		int alen = NLA_ALIGN(a->nla_len);
		rem -= alen;
		a = (const struct nlattr *)((const char *)a + alen);
	}
	return NULL;
}
