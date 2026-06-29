/* uKernel nl80211-bridge — genetlink CTRL család (família-feloldás).
 * Az iw/libnl ezzel kéri le az "nl80211" família-id-jét + multicast csoportjait. */
#include "nl_dispatch.h"
#include "uknl_common.h"
#include <linux/genetlink.h>
#include <string.h>
#include <stdio.h>

/* egy multicast csoport beágyazása a CTRL_ATTR_MCAST_GROUPS-ba */
static void put_mcgrp(struct nl_buf *b, int idx, const char *name, uint32_t id)
{
	size_t g = nla_nest_begin(b, idx);
	nla_put_u32(b, CTRL_ATTR_MCAST_GRP_ID, id);
	nla_put_str(b, CTRL_ATTR_MCAST_GRP_NAME, name);
	nla_nest_end(b, g);
}

static int ctrl_getfamily(const struct nl_req *req, struct nl_buf *resp)
{
	/* a kért família neve (alapból nl80211-et adunk) */
	const struct nlattr *fn = nla_find(req->attrs, req->attrs_len, CTRL_ATTR_FAMILY_NAME);
	const char *name = "nl80211";
	uint16_t fid = UKNL_NL80211_FAMILY_ID;
	if (fn) {
		const char *req_name = (const char *)nla_data2(fn);
		if (strcmp(req_name, "nlctrl") == 0) { name = "nlctrl"; fid = GENL_ID_CTRL; }
		else { name = "nl80211"; fid = UKNL_NL80211_FAMILY_ID; }
	}

	extern int uknl_debug;
	if (uknl_debug) fprintf(stderr, "[uknl] GETFAMILY kért='%s' -> id=0x%x\n", fn ? (const char *)nla_data2(fn) : "(nincs)", fid);
	size_t h = nlmsg_begin(resp, GENL_ID_CTRL, 0, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid,
	                       CTRL_CMD_NEWFAMILY, 2);
	nla_put_u16(resp, CTRL_ATTR_FAMILY_ID, fid);
	nla_put_str(resp, CTRL_ATTR_FAMILY_NAME, name);
	nla_put_u32(resp, CTRL_ATTR_VERSION, 1);
	nla_put_u32(resp, CTRL_ATTR_HDRSIZE, 0);
	nla_put_u32(resp, CTRL_ATTR_MAXATTR, 256);
	if (fid == UKNL_NL80211_FAMILY_ID) {
		size_t mg = nla_nest_begin(resp, CTRL_ATTR_MCAST_GROUPS);
		put_mcgrp(resp, 1, "config",     UKNL_MCGRP_CONFIG);
		put_mcgrp(resp, 2, "scan",       UKNL_MCGRP_SCAN);
		put_mcgrp(resp, 3, "regulatory", UKNL_MCGRP_REG);
		put_mcgrp(resp, 4, "mlme",       UKNL_MCGRP_MLME);
		put_mcgrp(resp, 5, "vendor",     UKNL_MCGRP_VENDOR);
		nla_nest_end(resp, mg);
	}
	nlmsg_end(resp, h);
	/* a libnl nl_send_sync a válasz után ACK-ot vár (NLMSG_ERROR, err=0) */
	if (req->nlh->nlmsg_flags & NLM_F_ACK)
		nlmsg_put_ack(resp, req->nlh->nlmsg_seq, req->nlh->nlmsg_pid, 0, req->nlh);
	return 0;
}

static const struct nl_cmd ctrl_cmds[] = {
	{ CTRL_CMD_GETFAMILY, ctrl_getfamily, "GETFAMILY" },
};
static const struct nl_family ctrl_family = {
	.id = GENL_ID_CTRL, .name = "nlctrl", .cmds = ctrl_cmds, .ncmds = 1,
};

void genl_ctrl_register(void) { nl_register_family(&ctrl_family); }
