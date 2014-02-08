// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef MARS_IF_H
#define MARS_IF_H

#include <linux/semaphore.h>

#define HT_SHIFT			6 //????
#define MARS_MAX_SEGMENT_SIZE		(1U << (9+HT_SHIFT))

#define MAX_BIO 			32

///////////////////////// global tuning ////////////////////////

extern int if_throttle_start_size; // in kb
extern struct mars_limiter if_throttle;

/////////////////////////////////////////////////

/* I don't want to enhance / intrude into struct bio for compatibility reasons
 * (support for a variety of kernel versions).
 * The following is just a silly workaround which could be removed again.
 */
struct bio_wrapper {
	struct bio *bio;
	atomic_t bi_comp_cnt;
	unsigned long start_time;
};

struct if_mref_aspect {
	GENERIC_ASPECT(mref);
	struct list_head plug_head;
	struct list_head hash_head;
	int hash_index;
	int bio_count;
	int current_len;
	int max_len;
	struct page *orig_page;
	struct bio_wrapper *orig_biow[MAX_BIO];
	struct if_input *input;
};

struct if_hash_anchor;

struct if_input {
	MARS_INPUT(if);
	// TODO: move this to if_brick (better systematics)
	struct list_head plug_anchor;
	struct request_queue *q;
	struct gendisk *disk;
	struct block_device *bdev;
	loff_t capacity;
	atomic_t plugged_count;
	atomic_t flying_count;
	// only for statistics
	atomic_t read_flying_count;
	atomic_t write_flying_count;
	atomic_t total_reada_count;
	atomic_t total_read_count;
	atomic_t total_write_count;
	atomic_t total_empty_count;
	atomic_t total_fire_count;
	atomic_t total_flush_count;
	atomic_t total_nometa_count;
	atomic_t total_nosync_count;
	atomic_t total_mref_read_count;
	atomic_t total_mref_write_count;
	spinlock_t req_lock;
	struct semaphore kick_sem;
	struct if_hash_anchor *hash_table;
};

struct if_output {
	MARS_OUTPUT(if);
};

struct if_brick {
	MARS_BRICK(if);
	// parameters
	loff_t dev_size;
	int max_plugged;
	// inspectable
	atomic_t open_count;
	// private
	struct semaphore switch_sem;
	struct say_channel *say_channel;
};

MARS_TYPES(if);

#endif
