/* uKernel nl80211-bridge — dispatch implementáció. */
#include "nl_dispatch.h"
#include "uknl_common.h"
#include <string.h>
#include <stdio.h>

#define MAX_FAM 8
static const struct nl_family *g_fam[MAX_FAM];
static int g_nfam;

void nl_register_family(const struct nl_family *fam)
{ if (g_nfam < MAX_FAM) g_fam[g_nfam++] = fam; }

const struct nl_family *nl_find_family(uint16_t id)
{ for (int i = 0; i < g_nfam; i++) if (g_fam[i]->id == id) return g_fam[i]; return NULL; }

const struct nl_family *nl_find_family_by_name(const char *name)
{ for (int i = 0; i < g_nfam; i++) if (strcmp(g_fam[i]->name, name) == 0) return g_fam[i]; return NULL; }

int nl_dispatch(const uint8_t *msg, size_t len, struct nl_buf *resp)
{
	struct nl_req req;
	if (nl_req_parse(msg, len, &req) || !req.gnlh) return -1;

	const struct nl_family *fam = nl_find_family(req.nlh->nlmsg_type);
	if (!fam) {
		if (uknl_debug) {
			fprintf(stderr, "[uknl] ISMERETLEN család type=0x%x len=%zu hex:", req.nlh->nlmsg_type, len);
			for (size_t i = 0; i < len && i < 48; i++) fprintf(stderr, " %02x", msg[i]);
			fprintf(stderr, "\n");
		}
		/* ismeretlen család -> jóindulatú ACK (0), hogy a kliens ne hasaljon el
		 * (pl. iw más genl-családokat is megpróbál feloldani/lekérdezni) */
		nlmsg_put_ack(resp, req.nlh->nlmsg_seq, req.nlh->nlmsg_pid, 0, req.nlh);
		return 0;
	}
	for (int i = 0; i < fam->ncmds; i++) {
		if (fam->cmds[i].cmd == req.gnlh->cmd) {
			if (uknl_debug) fprintf(stderr, "[uknl] dispatch: %s/%s\n", fam->name, fam->cmds[i].name);
			return fam->cmds[i].fn(&req, resp);
		}
	}
	if (uknl_debug) fprintf(stderr, "[uknl] dispatch: %s cmd=%u NINCS handler -> ACK0\n", fam->name, req.gnlh->cmd);
	/* a családban nincs ilyen parancs -> ACK 0 (no-op), hogy ne akadjon el */
	nlmsg_put_ack(resp, req.nlh->nlmsg_seq, req.nlh->nlmsg_pid, 0, req.nlh);
	return 0;
}
