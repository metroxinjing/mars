// (c) 2012 Thomas Schoebel-Theuer / 1&1 Internet AG

#include "lib_limiter.h"

#include <linux/kernel.h>
#include <linux/module.h>

#define LIMITER_TIME_RESOLUTION 	NSEC_PER_SEC

int xio_limit(struct xio_limiter *lim, int amount)
{
	int delay = 0;
	long long now;

	now = cpu_clock(raw_smp_processor_id());

	/* Compute the maximum delay along the path
	 * down to the root of the hierarchy tree.
	 */
	while (lim != NULL) {
		long long window = now - lim->lim_stamp;
		/* Sometimes, raw CPU clocks may do weired things...
		 * Smaller windows in the denominator than 1s could fake unrealistic rates.
		 */
		if (unlikely(lim->lim_min_window <= 0))
			lim->lim_min_window = 1000;
		if (unlikely(lim->lim_max_window <= lim->lim_min_window))
			lim->lim_max_window = lim->lim_min_window + 8000;
		if (unlikely(window < (long long)lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000)))
			window = (long long)lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000);

		/* Only use incremental accumulation at repeated calls, but
		 * never after longer pauses.
		 */
		if (likely(lim->lim_stamp &&
			   window < (long long)lim->lim_max_window * (LIMITER_TIME_RESOLUTION / 1000))) {
			long long rate_raw;
			int rate;

			/* Races are possible, but taken into account.
			 * There is no real harm from rarely lost updates.
			 */
			if (likely(amount > 0)) {
				lim->lim_accu += amount;
				lim->lim_cumul += amount;
				lim->lim_count++;
			}

			rate_raw = lim->lim_accu * LIMITER_TIME_RESOLUTION / window;
			rate = rate_raw;
			if (unlikely(rate_raw > INT_MAX)) {
				rate = INT_MAX;
			}
			lim->lim_rate = rate;

			// limit exceeded?
			if (lim->lim_max_rate > 0 && rate > lim->lim_max_rate) {
				int this_delay = (window * rate / lim->lim_max_rate - window) / (LIMITER_TIME_RESOLUTION / 1000);
				// compute maximum
				if (this_delay > delay && this_delay > 0)
					delay = this_delay;
			}

			/* Try to keep the next window below min_window
			 */
			window -= lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000);
			if (window > 0) {
				long long used_up = (long long)lim->lim_rate * window / LIMITER_TIME_RESOLUTION;
				if (used_up > 0) {
					lim->lim_stamp += window;
					lim->lim_accu -= used_up;
					if (unlikely(lim->lim_accu < 0))
						lim->lim_accu = 0;
				}
			}
		} else { // reset, start over with new measurement cycle
			if (unlikely(amount < 0))
				amount = 0;
			lim->lim_accu = amount;
			lim->lim_stamp = now - lim->lim_min_window * (LIMITER_TIME_RESOLUTION / 1000);
			lim->lim_rate = 0;
		}
		lim = lim->lim_father;
	}
	return delay;
}
EXPORT_SYMBOL_GPL(xio_limit);

void xio_limit_sleep(struct xio_limiter *lim, int amount)
{
	int sleep = xio_limit(lim, amount);
	if (sleep > 0) {
		if (unlikely(lim->lim_max_delay <= 0))
			lim->lim_max_delay = 1000;
		if (sleep > lim->lim_max_delay)
			sleep = lim->lim_max_delay;
		brick_msleep(sleep);
	}
}
EXPORT_SYMBOL_GPL(xio_limit_sleep);
