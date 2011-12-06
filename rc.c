
#include "rc.h"

inline void iso_rc_init(struct iso_rc_state *rc) {
	rc->rfair = ISO_RFAIR_INITIAL;
	rc->alpha = 0;
	rc->num_marked = 0;

	rc->last_rfair_change_time = ktime_get();
	rc->last_rfair_decrease_time = ktime_get();
	rc->last_feedback_time = ktime_get();

	spin_lock_init(&rc->spinlock);
}

/* Right now, we don't do anything on the TX side */
inline int iso_rc_tx(struct iso_rc_state *rc, struct sk_buff *skb) {
	return 0;
}

inline int iso_rc_rx(struct iso_rc_state *rc, struct sk_buff *skb) {
	int marked = skb_has_feedback(skb);
	ktime_t now = ktime_get();
	int changed = 0;
	u64 dt;

	if(marked) {
		dt = ktime_us_delta(now, rc->last_rfair_decrease_time);
		rc->num_marked++;

		/* Reduce lock contention by being optimistic */
		if(dt > ISO_RFAIR_DECREASE_INTERVAL_US) {
			if(!spin_trylock(&rc->spinlock))
				goto end;

			/* Check again: it is required, but is it very likely? */
			dt = ktime_us_delta(now, rc->last_rfair_decrease_time);
			if(unlikely(dt < ISO_RFAIR_DECREASE_INTERVAL_US))
				goto done_decrease;

			/* Compute alpha */
			iso_rc_do_alpha(rc);
			iso_rc_do_md(rc);
			rc->last_rfair_decrease_time = now;

		done_decrease:
			spin_unlock(&rc->spinlock);
			goto changed;
		}

		goto end;
	} else {
		dt = ktime_us_delta(now, rc->last_rfair_change_time);
		if(dt > ISO_RFAIR_INCREASE_INTERVAL_US) {
			if(!spin_trylock(&rc->spinlock))
				goto end;

			dt = ktime_us_delta(now, rc->last_rfair_change_time);
			if(unlikely(dt < ISO_RFAIR_INCREASE_INTERVAL_US))
				goto done_increase;

			iso_rc_do_alpha(rc);
			iso_rc_do_ai(rc);
		done_increase:
			spin_unlock(&rc->spinlock);
			goto changed;
		}

		goto end;
	}

 changed:
	rc->last_rfair_change_time = now;
	changed = 1;

 end:
	return changed;
}

inline void iso_rc_do_ai(struct iso_rc_state *rc) {
	rc->rfair = min((u64)ISO_MAX_TX_RATE, rc->rfair + ISO_RFAIR_INCREMENT);
}

inline void iso_rc_do_md(struct iso_rc_state *rc) {
	rc->rfair = rc->rfair * (2048 * ISO_FALPHA - rc->alpha) / (2048 * ISO_FALPHA);
	rc->rfair = max((u64)ISO_MIN_RFAIR, rc->rfair);
}

inline void iso_rc_do_alpha(struct iso_rc_state *rc) {
	u64 num_marked = rc->num_marked;
	u64 frac = num_marked > 0 ? 1024 : 0;

#define MUL31(x) (((x) << 5) - (x))
#define DIV32(x) ((x) >> 5)
#define EWMA_G32(old, new)  DIV32((MUL31(old) + new))

	rc->alpha = EWMA_G32(rc->alpha, frac);
	rc->num_marked = 0;
}

void iso_rc_show(struct iso_rc_state *rc, struct seq_file *s) {
	seq_printf(s, "\trfair %llu   alpha %llu   num_marked %llu   "
			   "last_change %llx   last_decrease %llx   last_feedback %llx\n",
			   rc->rfair, rc->alpha, rc->num_marked,
			   *(u64*)&rc->last_rfair_change_time, *(u64*)&rc->last_rfair_decrease_time,
			   *(u64*)&rc->last_feedback_time);
}

/* Local Variables: */
/* indent-tabs-mode:t */
/* End: */