// (c) 2012 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_LIB_TIMING_H
#define MARS_LIB_TIMING_H

#include "brick.h"

#include <linux/sched.h>

/* Simple infrastructure for timing of arbitrary operations and creation
 * of some simple histogram statistics.
 */

#define TIMING_MAX			24

struct timing_stats {
#ifdef CONFIG_MARS_DEBUG
	int tim_count[TIMING_MAX];
#endif
};

#define _TIME_THIS(_stamp1, _stamp2, _CODE)				\
	({								\
		(_stamp1) = cpu_clock(raw_smp_processor_id());		\
									\
		_CODE;							\
									\
		(_stamp2) = cpu_clock(raw_smp_processor_id());		\
		(_stamp2) - (_stamp1);					\
	})


#define TIME_THIS(_CODE)						\
	({								\
		unsigned long long _stamp1;				\
		unsigned long long _stamp2;				\
		_TIME_THIS(_stamp1, _stamp2, _CODE);			\
	})


#ifdef CONFIG_MARS_DEBUG

#define _TIME_STATS(_timing, _stamp1, _stamp2, _CODE)			\
	({								\
		unsigned long long _time;				\
		unsigned long _tmp;					\
		int _i; 						\
									\
		_time = _TIME_THIS(_stamp1, _stamp2, _CODE);		\
									\
		_tmp = _time / 1000;	/* convert to us */		\
		_i = 0; 						\
		while (_tmp > 0 && _i < TIMING_MAX - 1) {		\
			_tmp >>= 1;					\
			_i++;						\
		}							\
		(_timing)->tim_count[_i]++;				\
		_time;							\
	})

#define TIME_STATS(_timing, _CODE)					\
	({								\
		unsigned long long _stamp1;				\
		unsigned long long _stamp2;				\
		_TIME_STATS(_timing, _stamp1, _stamp2, _CODE);		\
	})

extern int report_timing(struct timing_stats *tim, char *str, int maxlen);

#else  // CONFIG_MARS_DEBUG

#define _TIME_STATS(_timing, _stamp1, _stamp2, _CODE)			\
	((void)_timing, (_stamp1) = (_stamp2) = cpu_clock(raw_smp_processor_id()), _CODE, 0)

#define TIME_STATS(_timing, _CODE)					\
	((void)_timing, _CODE, 0)

#define report_timing(tim,str,maxlen)	((void)tim, 0)

#endif // CONFIG_MARS_DEBUG

/* A banning represents some overloaded resource.
 *
 * Whenever overload is detected, you should call banning_hit()
 * telling that the overload is assumed / estimated to continue
 * for some duration in time.
 *
 * ATTENTION! These operations are deliberately raceful.
 * They are meant to deliver _hints_ (e.g. for IO scheduling
 * decisions etc), not hard facts!
 *
 * If you need locking, just surround these operations
 * with locking by yourself.
 */
struct banning {
	long long ban_last_hit;
	// statistical
	int ban_renew_count;
	int ban_count;
};

extern inline
bool banning_hit(struct banning *ban, long long duration)
{
	long long now = cpu_clock(raw_smp_processor_id());
	bool hit = ban->ban_last_hit >= now;
	long long new_hit = now + duration;
	ban->ban_renew_count++;
	if (!ban->ban_last_hit || ban->ban_last_hit < new_hit) {
		ban->ban_last_hit = new_hit;
		ban->ban_count++;
	}
	return hit;
}

extern inline
bool banning_is_hit(struct banning *ban)
{
	long long now = cpu_clock(raw_smp_processor_id());
	return (ban->ban_last_hit && ban->ban_last_hit >= now);
}

extern inline
void banning_reset(struct banning *ban)
{
	ban->ban_last_hit = 0;
}

/* Threshold: trigger a banning whenever some latency threshold
 * is exceeded.
 */
struct threshold {
	struct banning *thr_ban;
	// tunables
	int  thr_limit;   // in us
	int  thr_factor;  // in %
	int  thr_plus;	  // in us
	// statistical
	int thr_triggered;
	int thr_true_hit;
};

extern inline
void threshold_check(struct threshold *thr, long long latency)
{
	if (thr->thr_limit &&
	    latency > (long long)thr->thr_limit * 1000) {
		thr->thr_triggered++;
		if (!banning_hit(thr->thr_ban, latency * thr->thr_factor / 100 + thr->thr_plus * 1000))
			thr->thr_true_hit++;
	}
}

#endif
