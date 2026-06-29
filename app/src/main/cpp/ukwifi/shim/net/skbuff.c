/* uKernel — sk_buff implementáció malloc fölött. */
#include <linux/skbuff.h>
#include <stdlib.h>
#include <string.h>

struct sk_buff *alloc_skb(unsigned int size, gfp_t gfp)
{
	(void)gfp;
	struct sk_buff *skb = calloc(1, sizeof(*skb));
	if (!skb) return NULL;
	skb->head = malloc(size ? size : 1);
	if (!skb->head) { free(skb); return NULL; }
	skb->data = skb->head;
	skb->tail = skb->head;
	skb->end  = skb->head + size;
	skb->truesize = size;
	return skb;
}
void kfree_skb(struct sk_buff *skb) { if (skb) { free(skb->head); free(skb); } }

struct sk_buff *skb_clone(struct sk_buff *skb, gfp_t gfp)
{
	if (!skb) return NULL;
	unsigned int sz = skb->end - skb->head;
	struct sk_buff *n = alloc_skb(sz, gfp);
	if (!n) return NULL;
	memcpy(n->head, skb->head, sz);
	n->data = n->head + (skb->data - skb->head);
	n->tail = n->head + (skb->tail - skb->head);
	n->len = skb->len; n->protocol = skb->protocol; n->priority = skb->priority;
	memcpy(n->cb, skb->cb, sizeof(n->cb));
	return n;
}
struct sk_buff *skb_copy(const struct sk_buff *skb, gfp_t gfp) { return skb_clone((struct sk_buff *)skb, gfp); }
struct sk_buff *netdev_alloc_skb(struct net_device *dev, unsigned int len) { (void)dev; return alloc_skb(len, GFP_ATOMIC); }

int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len)
{
	if (offset + len > (int)skb->len) return -1;
	memcpy(to, skb->data + offset, len);
	return 0;
}

void *skb_put(struct sk_buff *skb, unsigned int len)
{ unsigned char *t = skb->tail; skb->tail += len; skb->len += len; return t; }
void *skb_push(struct sk_buff *skb, unsigned int len)
{ skb->data -= len; skb->len += len; return skb->data; }
void *skb_pull(struct sk_buff *skb, unsigned int len)
{ skb->data += len; skb->len -= len; return skb->data; }
void skb_reserve(struct sk_buff *skb, int len) { skb->data += len; skb->tail += len; }
void skb_trim(struct sk_buff *skb, unsigned int len) { if (skb->len > len) { skb->len = len; skb->tail = skb->data + len; } }

void skb_queue_head_init(struct sk_buff_head *list)
{ list->next = (struct sk_buff *)list; list->prev = (struct sk_buff *)list; list->qlen = 0; }
void skb_queue_tail(struct sk_buff_head *list, struct sk_buff *newsk)
{
	struct sk_buff *prev = list->prev;
	newsk->next = (struct sk_buff *)list; newsk->prev = prev;
	prev->next = newsk; list->prev = newsk; list->qlen++;
}
struct sk_buff *skb_dequeue(struct sk_buff_head *list)
{
	struct sk_buff *skb = list->next;
	if (skb == (struct sk_buff *)list) return NULL;
	list->next = skb->next; skb->next->prev = (struct sk_buff *)list; list->qlen--;
	skb->next = skb->prev = NULL;
	return skb;
}

/* Drain + free every queued skb (drivers call this on stop/teardown). */
void skb_queue_purge(struct sk_buff_head *list)
{
	struct sk_buff *skb;
	while ((skb = skb_dequeue(list)) != NULL) kfree_skb(skb);
}
