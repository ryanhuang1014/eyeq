#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include "rl.h"
#include "tx.h"
#include "rx.h"

#ifndef DIRECT
#error "Compiling direct.c without -DDIRECT"
#endif

extern struct net_device *iso_netdev;
static netdev_tx_t (*old_ndo_start_xmit)(struct sk_buff *, struct net_device *);
netdev_tx_t iso_ndo_start_xmit(struct sk_buff *, struct net_device *);
rx_handler_result_t iso_rx_handler(struct sk_buff **);

int iso_tx_hook_init(struct iso_tx_context *);
void iso_tx_hook_exit(struct iso_tx_context *);

int iso_rx_hook_init(struct iso_rx_context *);
void iso_rx_hook_exit(struct iso_rx_context *);

enum iso_verdict iso_tx(struct sk_buff *skb, const struct net_device *out, struct iso_tx_context *);
enum iso_verdict iso_rx(struct sk_buff *skb, const struct net_device *in, struct iso_rx_context *);

/* Called with bh disabled */
inline void skb_xmit(struct sk_buff *skb) {
	struct netdev_queue *txq;
	int cpu;
	int locked = 0;

	if(likely(old_ndo_start_xmit != NULL)) {
		cpu = smp_processor_id();
		txq = netdev_get_tx_queue(iso_netdev, skb_get_queue_mapping(skb));

		if(txq->xmit_lock_owner != cpu) {
			HARD_TX_LOCK(iso_netdev, txq, cpu);
			locked = 1;
		}
		/* XXX: will the else condition happen? */

		if(!netif_tx_queue_stopped(txq)) {
			old_ndo_start_xmit(skb, iso_netdev);
		} else {
			kfree_skb(skb);
		}

		if(locked) {
			HARD_TX_UNLOCK(iso_netdev, txq);
		}
	}
}

int iso_tx_hook_init(struct iso_tx_context *txctx) {
	struct net_device_ops *ops;

	if(iso_netdev == NULL || iso_netdev->netdev_ops == NULL)
		return 1;

	ops = (struct net_device_ops *)iso_netdev->netdev_ops;

	rtnl_lock();
	old_ndo_start_xmit = txctx->xmit = ops->ndo_start_xmit;
	ops->ndo_start_xmit = iso_ndo_start_xmit;
	rtnl_unlock();

	synchronize_net();
	return 0;
}

void iso_tx_hook_exit(struct iso_tx_context *txctx) {
	struct net_device_ops *ops = (struct net_device_ops *)iso_netdev->netdev_ops;

	rtnl_lock();
	ops->ndo_start_xmit = txctx->xmit;
	rtnl_unlock();

	synchronize_net();
}

int iso_rx_hook_init(struct iso_rx_context *rxctx) {
	int ret = 0;

	if(iso_netdev == NULL)
		return 1;

	rtnl_lock();
	ret = netdev_rx_handler_register(iso_netdev, iso_rx_handler, NULL);
	rtnl_unlock();

	/* Wait till stack sees our new handler */
	synchronize_net();
	return ret;
}

void iso_rx_hook_exit(struct iso_rx_context *rxctx) {
	rtnl_lock();
	netdev_rx_handler_unregister(iso_netdev);
	rtnl_unlock();
	synchronize_net();
}

/* Called with bh disabled */
netdev_tx_t iso_ndo_start_xmit(struct sk_buff *skb, struct net_device *out) {
	enum iso_verdict verdict;
	struct netdev_queue *txq;
	int cpu = smp_processor_id();
	netdev_tx_t ret = NETDEV_TX_OK;

	txq = netdev_get_tx_queue(iso_netdev, skb_get_queue_mapping(skb));
	HARD_TX_UNLOCK(iso_netdev, txq);

	skb_reset_mac_header(skb);
	verdict = iso_tx(skb, out, &global_txcontext);

	switch(verdict) {
	case ISO_VERDICT_DROP:
		ret = NETDEV_TX_BUSY;
		break;

	case ISO_VERDICT_PASS:
		skb_xmit(skb);
		break;

	case ISO_VERDICT_SUCCESS:
	case ISO_VERDICT_ERROR:
	default:
		break;
	}

	HARD_TX_LOCK(iso_netdev, txq, cpu);
	return ret;
}

rx_handler_result_t iso_rx_handler(struct sk_buff **pskb) {
	struct sk_buff *skb = *pskb;
	enum iso_verdict verdict;

	if(unlikely(skb->pkt_type == PACKET_LOOPBACK))
		return RX_HANDLER_PASS;

	verdict = iso_rx(skb, iso_netdev, &global_rxcontext);

	switch(verdict) {
	case ISO_VERDICT_DROP:
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;

	case ISO_VERDICT_SUCCESS:
	case ISO_VERDICT_PASS:
	default:
		return RX_HANDLER_PASS;
	}

	/* Unreachable */
	return RX_HANDLER_PASS;
}

/* Local Variables: */
/* indent-tabs-mode:t */
/* End: */
