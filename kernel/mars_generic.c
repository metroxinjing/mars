// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/utsname.h>

#include "mars.h"
#include "mars_client.h"

//////////////////////////////////////////////////////////////

// infrastructure

struct banning mars_global_ban = {};
EXPORT_SYMBOL_GPL(mars_global_ban);
atomic_t mars_global_io_flying = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(mars_global_io_flying);

static char *id = NULL;

/* TODO: better use MAC addresses (or motherboard IDs where available).
 * Or, at least, some checks for MAC addresses should be recorded / added.
 * When the nodename is misconfigured, data might be scrambled.
 * MAC addresses should be more secure.
 * In ideal case, further checks should be added to prohibit accidental
 * name clashes.
 */
char *my_id(void)
{
	struct new_utsname *u;
	if (!id) {
		//down_read(&uts_sem); // FIXME: this is currenty not EXPORTed from the kernel!
		u = utsname();
		if (u) {
			id = brick_strdup(u->nodename);
		}
		//up_read(&uts_sem);
	}
	return id;
}
EXPORT_SYMBOL_GPL(my_id);

//////////////////////////////////////////////////////////////

// object stuff

const struct generic_object_type mref_type = {
        .object_type_name = "mref",
        .default_size = sizeof(struct mref_object),
	.object_type_nr = OBJ_TYPE_MREF,
};
EXPORT_SYMBOL_GPL(mref_type);

//////////////////////////////////////////////////////////////

// brick stuff

/////////////////////////////////////////////////////////////////////

// meta descriptions

const struct meta mars_info_meta[] = {
	META_INI(current_size,    struct mars_info, FIELD_INT),
	META_INI(tf_align,        struct mars_info, FIELD_INT),
	META_INI(tf_min_size,     struct mars_info, FIELD_INT),
	{}
};
EXPORT_SYMBOL_GPL(mars_info_meta);

const struct meta mars_mref_meta[] = {
	META_INI(_object_cb.cb_error, struct mref_object, FIELD_INT),
	META_INI(ref_pos,          struct mref_object, FIELD_INT),
	META_INI(ref_len,          struct mref_object, FIELD_INT),
	META_INI(ref_may_write,    struct mref_object, FIELD_INT),
	META_INI(ref_prio,         struct mref_object, FIELD_INT),
	META_INI(ref_cs_mode,      struct mref_object, FIELD_INT),
	META_INI(ref_timeout,      struct mref_object, FIELD_INT),
	META_INI(ref_total_size,   struct mref_object, FIELD_INT),
	META_INI(ref_checksum,     struct mref_object, FIELD_INT),
	META_INI(ref_flags,        struct mref_object, FIELD_INT),
	META_INI(ref_rw,           struct mref_object, FIELD_INT),
	META_INI(ref_id,           struct mref_object, FIELD_INT),
	{}
};
EXPORT_SYMBOL_GPL(mars_mref_meta);

const struct meta mars_timespec_meta[] = {
	META_INI(tv_sec, struct timespec, FIELD_INT),
	META_INI(tv_nsec, struct timespec, FIELD_INT),
	{}
};
EXPORT_SYMBOL_GPL(mars_timespec_meta);


//////////////////////////////////////////////////////////////

// crypto stuff

#include <linux/crypto.h>

static struct crypto_hash *mars_tfm = NULL;
static struct semaphore tfm_sem = __SEMAPHORE_INITIALIZER(tfm_sem, 1);
int mars_digest_size = 0;
EXPORT_SYMBOL_GPL(mars_digest_size);

void mars_digest(unsigned char *digest, void *data, int len)
{
	struct hash_desc desc = {
		.tfm = mars_tfm,
		.flags = 0,
	};
	struct scatterlist sg;

	memset(digest, 0, mars_digest_size);

	// TODO: use per-thread instance, omit locking
	down(&tfm_sem);

	crypto_hash_init(&desc);
	sg_init_table(&sg, 1);
	sg_set_buf(&sg, data, len);
	crypto_hash_update(&desc, &sg, sg.length);
	crypto_hash_final(&desc, digest);
	up(&tfm_sem);
}
EXPORT_SYMBOL_GPL(mars_digest);

void mref_checksum(struct mref_object *mref)
{
	unsigned char checksum[mars_digest_size];
	int len;

	if (mref->ref_cs_mode <= 0 || !mref->ref_data)
		return;

	mars_digest(checksum, mref->ref_data, mref->ref_len);

	len = sizeof(mref->ref_checksum);
	if (len > mars_digest_size)
		len = mars_digest_size;
	memcpy(&mref->ref_checksum, checksum, len);
}
EXPORT_SYMBOL_GPL(mref_checksum);

/////////////////////////////////////////////////////////////////////

// tracing

#ifdef MARS_TRACING

unsigned long long start_trace_clock = 0;
EXPORT_SYMBOL_GPL(start_trace_clock);

struct file *mars_log_file = NULL;
loff_t mars_log_pos = 0;

void _mars_log(char *buf, int len)
{
	static DECLARE_MUTEX(trace_lock);
	mm_segment_t oldfs;
	

	oldfs = get_fs();
	set_fs(get_ds());
	down(&trace_lock);

	vfs_write(mars_log_file, buf, len, &mars_log_pos);

	up(&trace_lock);
	set_fs(oldfs);
}
EXPORT_SYMBOL_GPL(_mars_log);

void mars_log(const char *fmt, ...)
{
	char *buf = brick_string_alloc(0);
	va_list args;
	int len;
	if (!buf)
		return;

	va_start(args, fmt);
	len = vsnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);

	_mars_log(buf, len);

	brick_string_free(buf);
}
EXPORT_SYMBOL_GPL(mars_log);

void mars_trace(struct mref_object *mref, const char *info)
{
	int index = mref->ref_traces;
	if (likely(index < MAX_TRACES)) {
		mref->ref_trace_stamp[index] = cpu_clock(raw_smp_processor_id());
		mref->ref_trace_info[index] = info;
		mref->ref_traces++;
	}
}
EXPORT_SYMBOL_GPL(mars_trace);

void mars_log_trace(struct mref_object *mref)
{
	char *buf = brick_string_alloc(0);
	unsigned long long old;
	unsigned long long diff;
	int i;
	int len;

	if (!buf) {
		return;
	}
	if (!mars_log_file || !mref->ref_traces) {
		goto done;
	}
	if (!start_trace_clock) {
		start_trace_clock = mref->ref_trace_stamp[0];
	}

	diff = mref->ref_trace_stamp[mref->ref_traces-1] - mref->ref_trace_stamp[0];

	len = snprintf(buf, PAGE_SIZE, "%c ;%12lld ;%6d;%10llu", mref->ref_rw ? 'W' : 'R', mref->ref_pos, mref->ref_len, diff / 1000);

	old = start_trace_clock;
	for (i = 0; i < mref->ref_traces; i++) {
		diff = mref->ref_trace_stamp[i] - old;
		
		len += snprintf(buf + len, PAGE_SIZE - len, " ; %s ;%10llu", mref->ref_trace_info[i], diff / 1000);
		old = mref->ref_trace_stamp[i];
	}
	len +=snprintf(buf + len, PAGE_SIZE - len, "\n");

	_mars_log(buf, len);

 done:
	brick_string_free(buf);
	mref->ref_traces = 0;
}
EXPORT_SYMBOL_GPL(mars_log_trace);

#endif // MARS_TRACING

/////////////////////////////////////////////////////////////////////

// power led handling

void mars_power_led_on(struct mars_brick *brick, bool val)
{
	bool oldval = brick->power.led_on;
	if (val != oldval) {
		//MARS_DBG("brick '%s' type '%s' led_on %d -> %d\n", brick->brick_path, brick->type->type_name, oldval, val);
		set_led_on(&brick->power, val);
		mars_trigger();
	}
}
EXPORT_SYMBOL_GPL(mars_power_led_on);

void mars_power_led_off(struct mars_brick *brick, bool val)
{
	bool oldval = brick->power.led_off;
	if (val != oldval) {
		//MARS_DBG("brick '%s' type '%s' led_off %d -> %d\n", brick->brick_path, brick->type->type_name, oldval, val);
		set_led_off(&brick->power, val);
		mars_trigger();
	}
}
EXPORT_SYMBOL_GPL(mars_power_led_off);


/////////////////////////////////////////////////////////////////////

// init stuff

void (*_mars_trigger)(void) = NULL;
EXPORT_SYMBOL_GPL(_mars_trigger);

struct mm_struct *mm_fake = NULL;
EXPORT_SYMBOL_GPL(mm_fake);
struct task_struct *mm_fake_task = NULL;
atomic_t mm_fake_count = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(mm_fake_count);

int __init init_mars(void)
{
	MARS_INF("init_mars()\n");

	set_fake();

#ifdef MARS_TRACING
	{
		int flags = O_CREAT | O_TRUNC | O_RDWR | O_LARGEFILE;
		int prot = 0600;
		mm_segment_t oldfs;
		oldfs = get_fs();
		set_fs(get_ds());
		mars_log_file = filp_open("/mars/trace.csv", flags, prot);
		set_fs(oldfs);
		if (IS_ERR(mars_log_file)) {
			MARS_ERR("cannot create trace logfile, status = %ld\n", PTR_ERR(mars_log_file));
			mars_log_file = NULL;
		}
	}
#endif

	mars_tfm = crypto_alloc_hash("md5", 0, CRYPTO_ALG_ASYNC);
	if (!mars_tfm) {
		MARS_ERR("cannot alloc crypto hash\n");
		return -ENOMEM;
	}
	if (IS_ERR(mars_tfm)) {
		MARS_ERR("alloc crypto hash failed, status = %d\n", (int)PTR_ERR(mars_tfm));
		return PTR_ERR(mars_tfm);
	}
#if 0
	if (crypto_tfm_alg_type(crypto_hash_tfm(mars_tfm)) != CRYPTO_ALG_TYPE_DIGEST) {
		MARS_ERR("bad crypto hash type\n");
		return -EINVAL;
	}
#endif
	mars_digest_size = crypto_hash_digestsize(mars_tfm);
	MARS_INF("digest_size = %d\n", mars_digest_size);

	return 0;
}

void __exit exit_mars(void)
{
	MARS_INF("exit_mars()\n");

	put_fake();

	if (mars_tfm) {
		crypto_free_hash(mars_tfm);
	}

#ifdef MARS_TRACING
	if (mars_log_file) {
		filp_close(mars_log_file, NULL);
		mars_log_file = NULL;
	}
#endif
	if (id) {
		brick_string_free(id);
		id = NULL;
	}
}

#ifndef CONFIG_MARS_HAVE_BIGMODULE
MODULE_DESCRIPTION("MARS block storage");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_mars);
module_exit(exit_mars);
#endif
