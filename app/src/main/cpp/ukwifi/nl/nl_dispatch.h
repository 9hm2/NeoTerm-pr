/* uKernel nl80211-bridge — moduláris dispatch.
 * Minden család (CTRL, nl80211, ...) regisztrál egy parancs-táblát; a dispatcher
 * az nlmsg_type (család-id) és a genl cmd alapján a megfelelő handlerre route-ol.
 * Új parancs hozzáadása = egy sor a megfelelő modul tábláján. */
#ifndef UKNL_DISPATCH_H
#define UKNL_DISPATCH_H

#include "netlink_msg.h"

/* Egy parancs-handler: a req-ből építi a resp-et. 0 = ok. */
typedef int (*nl_handler_fn)(const struct nl_req *req, struct nl_buf *resp);

struct nl_cmd {
	uint8_t        cmd;     /* genl cmd (pl. NL80211_CMD_GET_INTERFACE) */
	nl_handler_fn  fn;
	const char    *name;
};

struct nl_family {
	uint16_t            id;     /* netlink message type = család-id */
	const char         *name;
	const struct nl_cmd *cmds;
	int                 ncmds;
};

/* Család regisztrálása (a modulok init-jükben hívják). */
void nl_register_family(const struct nl_family *fam);
const struct nl_family *nl_find_family(uint16_t id);
const struct nl_family *nl_find_family_by_name(const char *name);

/* Egy bejövő netlink-üzenet feldolgozása -> válasz a resp-be. 0 = kezelve. */
int nl_dispatch(const uint8_t *msg, size_t len, struct nl_buf *resp);

/* A modulok regisztráló belépési pontjai (külön fájlokban definiálva). */
void genl_ctrl_register(void);
void nl80211_register(void);

#endif
