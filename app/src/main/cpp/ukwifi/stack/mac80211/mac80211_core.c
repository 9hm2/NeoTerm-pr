/* uKernel — mac80211 VÉKONY STUB (.so). Soft-MAC driverek betöltéséhez.
 * A cfg80211.so wiphy-jére épül. A valódi mac80211 TX/RX/rate-control path
 * későbbi bővítés (a rtl8812au full-MAC, ezt nem igényli). */
#include <net/mac80211.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <stdlib.h>

struct ieee80211_hw *ieee80211_alloc_hw(size_t priv_len, const struct ieee80211_ops *ops)
{
	(void)ops;
	struct ieee80211_hw *hw = calloc(1, sizeof(*hw));
	if (!hw) return NULL;
	if (priv_len) hw->priv = calloc(1, priv_len);
	hw->wiphy = wiphy_new(NULL, 0);   /* a cfg80211.so-ból */
	hw->queues = 4;
	printk(KERN_INFO "uKernel/mac80211: alloc_hw (stub, priv=%zu)\n", priv_len);
	return hw;
}
int ieee80211_register_hw(struct ieee80211_hw *hw)
{ printk(KERN_INFO "uKernel/mac80211: register_hw (stub)\n"); return hw->wiphy ? wiphy_register(hw->wiphy) : 0; }
void ieee80211_unregister_hw(struct ieee80211_hw *hw) { if (hw->wiphy) wiphy_unregister(hw->wiphy); }
void ieee80211_free_hw(struct ieee80211_hw *hw) { if (hw) { free(hw->priv); free(hw); } }
void ieee80211_rx(struct ieee80211_hw *hw, struct sk_buff *skb) { (void)hw; kfree_skb(skb); }
void ieee80211_tx_status(struct ieee80211_hw *hw, struct sk_buff *skb) { (void)hw; kfree_skb(skb); }
