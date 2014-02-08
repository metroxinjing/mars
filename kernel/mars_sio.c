// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/splice.h>

#include "mars.h"

///////////////////////// own type definitions ////////////////////////

#include "mars_sio.h"

////////////////// own brick / input / output operations //////////////////

static int sio_io_get(struct sio_output *output, struct aio_object *aio)
{
	struct file *file;

	if (aio->obj_initialized) {
		obj_get(aio);
		return aio->io_len;
	}

	file = output->filp;
	if (file) {
		loff_t total_size = i_size_read(file->f_mapping->host);
		aio->io_total_size = total_size;
		/* Only check reads.
		 * Writes behind EOF are always allowed (sparse files)
		 */
		if (!aio->io_may_write) {
			loff_t len = total_size - aio->io_pos;
			if (unlikely(len <= 0)) {
				/* Special case: allow reads starting _exactly_ at EOF when a timeout is specified.
				 */
				if (len < 0 || aio->io_timeout <= 0) {
					MARS_DBG("ENODATA %lld\n", len);
					return -ENODATA;
				}
			}
			// Shorten below EOF, but allow special case
			if (aio->io_len > len && len > 0) {
				aio->io_len = len;
			}
		}
	}

	/* Buffered IO.
	 */
	if (!aio->io_data) {
		struct sio_aio_aspect *aio_a = sio_aio_get_aspect(output->brick, aio);
		if (unlikely(!aio_a))
			return -EILSEQ;
		if (unlikely(aio->io_len <= 0)) {
			MARS_ERR("bad io_len = %d\n", aio->io_len);
			return -ENOMEM;
		}
		aio->io_data = brick_block_alloc(aio->io_pos, (aio_a->alloc_len = aio->io_len));
		aio_a->do_dealloc = true;
		//atomic_inc(&output->total_alloc_count);
		//atomic_inc(&output->alloc_count);
	}

	obj_get_first(aio);
	return aio->io_len;
}

static void sio_io_put(struct sio_output *output, struct aio_object *aio)
{
	struct file *file;
	struct sio_aio_aspect *aio_a;

	if (!obj_put(aio))
		return;

	file = output->filp;
	if (file) {
		aio->io_total_size = i_size_read(file->f_mapping->host);
	}

	aio_a = sio_aio_get_aspect(output->brick, aio);
	if (aio_a && aio_a->do_dealloc) {
		brick_block_free(aio->io_data, aio_a->alloc_len);
		//atomic_dec(&output->alloc_count);
	}

	obj_free(aio);
}

static
int write_aops(struct sio_output *output, struct aio_object *aio)
{
	struct file *file = output->filp;
	loff_t pos = aio->io_pos;
	void *data = aio->io_data;
	int  len = aio->io_len;
	int ret = 0;
	mm_segment_t oldfs = get_fs();
	set_fs(get_ds());
	ret = vfs_write(file, data, len, &pos);
	set_fs(oldfs);
	return ret;
}

static
int read_aops(struct sio_output *output, struct aio_object *aio)
{
	loff_t pos = aio->io_pos;
	int len = aio->io_len;
	int ret = -EIO;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());
	ret = vfs_read(output->filp, aio->io_data, len, &pos);
	set_fs(oldfs);

	if (unlikely(ret < 0)) {
		MARS_ERR("%p %p status=%d\n", output, aio, ret);
	}
	return ret;
}

static void sync_file(struct sio_output *output)
{
	struct file *file = output->filp;
	int ret;
#if defined(S_BIAS) || (defined(RHEL_MAJOR) && (RHEL_MAJOR < 7))
	ret = vfs_fsync(file, file->f_path.dentry, 1);
#else
	ret = vfs_fsync(file, 1);
#endif
	if (unlikely(ret)) {
		MARS_ERR("syncing pages failed: %d\n", ret);
	}
	return;
}

static
void _complete(struct sio_output *output, struct aio_object *aio, int err)
{
	obj_check(aio);

	if (err < 0) {
		MARS_ERR("IO error %d at pos=%lld len=%d (aio=%p io_data=%p)\n", err, aio->io_pos, aio->io_len, aio, aio->io_data);
	} else {
		aio_checksum(aio);
		aio->io_flags |= AIO_UPTODATE;
	}

	CHECKED_CALLBACK(aio, err, err_found);

done:
	sio_io_put(output, aio);

	atomic_dec(&mars_global_io_flying);
	return;

err_found:
	MARS_FAT("giving up...\n");
	goto done;
}

/* This is called by the threads
 */
static
void _sio_io_io(struct sio_threadinfo *tinfo, struct aio_object *aio)
{
	struct sio_output *output = tinfo->output;
	bool barrier = false;
	int status;

	obj_check(aio);

	atomic_inc(&tinfo->fly_count);

	if (unlikely(!output->filp)) {
		status = -EINVAL;
		goto done;
	}

	if (barrier) {
		MARS_INF("got barrier request\n");
		sync_file(output);
	}

	if (aio->io_rw == READ) {
		status = read_aops(output, aio);
	} else {
		status = write_aops(output, aio);
		if (barrier || output->brick->o_fdsync)
			sync_file(output);
	}


done:
	_complete(output, aio, status);

	atomic_dec(&tinfo->fly_count);
}

/* This is called from outside
 */
static
void sio_io_io(struct sio_output *output, struct aio_object *aio)
{
	int index;
	struct sio_threadinfo *tinfo;
	struct sio_aio_aspect *aio_a;

	obj_check(aio);

	aio_a = sio_aio_get_aspect(output->brick, aio);
	if (unlikely(!aio_a)) {
		MARS_FAT("cannot get aspect\n");
		SIMPLE_CALLBACK(aio, -EINVAL);
		return;
	}

	atomic_inc(&mars_global_io_flying);
	obj_get(aio);

	index = 0;
	if (aio->io_rw == READ) {
		spin_lock(&output->g_lock);
		index = output->index++;
		spin_unlock(&output->g_lock);
		index = (index % WITH_THREAD) + 1;
	}

	tinfo = &output->tinfo[index];

	atomic_inc(&tinfo->total_count);
	atomic_inc(&tinfo->queue_count);

	spin_lock(&tinfo->lock);
	list_add_tail(&aio_a->io_head, &tinfo->aio_list);
	spin_unlock(&tinfo->lock);

	wake_up_interruptible(&tinfo->event);
}

static int sio_thread(void *data)
{
	struct sio_threadinfo *tinfo = data;

	MARS_INF("sio thread has started.\n");
	//set_user_nice(current, -20);

	while (!brick_thread_should_stop()) {
		struct list_head *tmp = NULL;
		struct aio_object *aio;
		struct sio_aio_aspect *aio_a;

		wait_event_interruptible_timeout(
			tinfo->event,
			!list_empty(&tinfo->aio_list) || brick_thread_should_stop(),
			HZ);

		tinfo->last_jiffies = jiffies;

		spin_lock(&tinfo->lock);

		if (!list_empty(&tinfo->aio_list)) {
			tmp = tinfo->aio_list.next;
			list_del_init(tmp);
			atomic_dec(&tinfo->queue_count);
		}

		spin_unlock(&tinfo->lock);

		if (!tmp)
			continue;

		aio_a = container_of(tmp, struct sio_aio_aspect, io_head);
		aio = aio_a->object;
		_sio_io_io(tinfo, aio);
	}

	MARS_INF("sio thread has stopped.\n");
	return 0;
}

static int sio_get_info(struct sio_output *output, struct mars_info *info)
{
	struct file *file = output->filp;
	if (unlikely(!file || !file->f_mapping || !file->f_mapping->host))
		return -EINVAL;

	info->tf_align = 1;
	info->tf_min_size = 1;
	info->current_size = i_size_read(file->f_mapping->host);
	MARS_DBG("determined file size = %lld\n", info->current_size);
	return 0;
}

//////////////// informational / statistics ///////////////

static noinline
char *sio_statistics(struct sio_brick *brick, int verbose)
{
	struct sio_output *output = brick->outputs[0];
	char *res = brick_string_alloc(1024);
	int queue_sum = 0;
	int fly_sum   = 0;
	int total_sum = 0;
	int i;

	for (i = 1; i <= WITH_THREAD; i++) {
		struct sio_threadinfo *tinfo = &output->tinfo[i];
		queue_sum += atomic_read(&tinfo->queue_count);
		fly_sum   += atomic_read(&tinfo->fly_count);
		total_sum += atomic_read(&tinfo->total_count);
	}

	snprintf(res, 1024,
		 "queued read = %d write = %d "
		 "flying read = %d write = %d "
		 "total  read = %d write = %d "
		 "\n",
		 queue_sum, atomic_read(&output->tinfo[0].queue_count),
		 fly_sum,   atomic_read(&output->tinfo[0].fly_count),
		 total_sum, atomic_read(&output->tinfo[0].total_count)
		);
	return res;
}

static noinline
void sio_reset_statistics(struct sio_brick *brick)
{
	struct sio_output *output = brick->outputs[0];
	int i;
	for (i = 0; i <= WITH_THREAD; i++) {
		struct sio_threadinfo *tinfo = &output->tinfo[i];
		atomic_set(&tinfo->total_count, 0);
	}
}


//////////////// object / aspect constructors / destructors ///////////////

static int sio_aio_aspect_init_fn(struct generic_aspect *_ini)
{
	struct sio_aio_aspect *ini = (void*)_ini;
	INIT_LIST_HEAD(&ini->io_head);
	return 0;
}

static void sio_aio_aspect_exit_fn(struct generic_aspect *_ini)
{
	struct sio_aio_aspect *ini = (void*)_ini;
	(void)ini;
	CHECK_HEAD_EMPTY(&ini->io_head);
}

MARS_MAKE_STATICS(sio);

////////////////////// brick constructors / destructors ////////////////////

static int sio_brick_construct(struct sio_brick *brick)
{
	return 0;
}

static int sio_switch(struct sio_brick *brick)
{
	static int sio_nr = 0;
	struct sio_output *output = brick->outputs[0];
	const char *path = output->brick->brick_path;
	int prot = 0600;
	mm_segment_t oldfs;
	int status = 0;

	if (brick->power.button) {
		int flags = O_CREAT | O_RDWR | O_LARGEFILE;
		struct address_space *mapping;
		int index;

		if (brick->power.led_on)
			goto done;

		if (brick->o_direct) {
			flags |= O_DIRECT;
			MARS_INF("using O_DIRECT on %s\n", path);
		}

		mars_power_led_off((void*)brick, false);

		// TODO: convert to mapfree infrastructure

		oldfs = get_fs();
		set_fs(get_ds());
		output->filp = filp_open(path, flags, prot);
		set_fs(oldfs);

		if (unlikely(IS_ERR(output->filp))) {
			status = PTR_ERR(output->filp);
			MARS_ERR("can't open file '%s' status=%d\n", path, status);
			output->filp = NULL;
			goto done;
		}

		if ((mapping = output->filp->f_mapping)) {
			mapping_set_gfp_mask(mapping, mapping_gfp_mask(mapping) & ~(__GFP_IO | __GFP_FS));
		}

		MARS_INF("opened file '%s' as %p\n", path, output->filp);

		output->index = 0;
		for (index = 0; index <= WITH_THREAD; index++) {
			struct sio_threadinfo *tinfo = &output->tinfo[index];

			tinfo->last_jiffies = jiffies;
			tinfo->thread = brick_thread_create(sio_thread, tinfo, "mars_sio%d", sio_nr++);
			if (unlikely(!tinfo->thread)) {
				MARS_ERR("cannot create thread\n");
				status = -ENOENT;
				goto done;
			}
		}
		mars_power_led_on((void*)brick, true);
	}
done:
	if (unlikely(status < 0) || !brick->power.button) {
		int index;
		mars_power_led_on((void*)brick, false);
		for (index = 0; index <= WITH_THREAD; index++) {
			struct sio_threadinfo *tinfo = &output->tinfo[index];
			if (!tinfo->thread)
				continue;
			MARS_DBG("stopping thread %d\n", index);
			brick_thread_stop(tinfo->thread);
			tinfo->thread = NULL;
		}
		if (output->filp) {
			MARS_DBG("closing file\n");
			filp_close(output->filp, NULL);
			output->filp = NULL;
		}
		mars_power_led_off((void*)brick, true);
	}
	return status;
}

static int sio_output_construct(struct sio_output *output)
{
	int index;

	spin_lock_init(&output->g_lock);
	for (index = 0; index <= WITH_THREAD; index++) {
		struct sio_threadinfo *tinfo = &output->tinfo[index];
		tinfo->output = output;
		spin_lock_init(&tinfo->lock);
		init_waitqueue_head(&tinfo->event);
		INIT_LIST_HEAD(&tinfo->aio_list);
	}

	return 0;
}

static int sio_output_destruct(struct sio_output *output)
{
	return 0;
}

///////////////////////// static structs ////////////////////////

static struct sio_brick_ops sio_brick_ops = {
	.brick_switch = sio_switch,
	.brick_statistics = sio_statistics,
	.reset_statistics = sio_reset_statistics,
};

static struct sio_output_ops sio_output_ops = {
	.aio_get = sio_io_get,
	.aio_put = sio_io_put,
	.aio_io = sio_io_io,
	.mars_get_info = sio_get_info,
};

const struct sio_input_type sio_input_type = {
	.type_name = "sio_input",
	.input_size = sizeof(struct sio_input),
};

static const struct sio_input_type *sio_input_types[] = {
	&sio_input_type,
};

const struct sio_output_type sio_output_type = {
	.type_name = "sio_output",
	.output_size = sizeof(struct sio_output),
	.master_ops = &sio_output_ops,
	.output_construct = &sio_output_construct,
	.output_destruct = &sio_output_destruct,
};

static const struct sio_output_type *sio_output_types[] = {
	&sio_output_type,
};

const struct sio_brick_type sio_brick_type = {
	.type_name = "sio_brick",
	.brick_size = sizeof(struct sio_brick),
	.max_inputs = 0,
	.max_outputs = 1,
	.master_ops = &sio_brick_ops,
	.aspect_types = sio_aspect_types,
	.default_input_types = sio_input_types,
	.default_output_types = sio_output_types,
	.brick_construct = &sio_brick_construct,
};
EXPORT_SYMBOL_GPL(sio_brick_type);

////////////////// module init stuff /////////////////////////

int __init init_mars_sio(void)
{
	MARS_INF("init_sio()\n");
	_sio_brick_type = (void*)&sio_brick_type;
	return sio_register_brick_type();
}

void __exit exit_mars_sio(void)
{
	MARS_INF("exit_sio()\n");
	sio_unregister_brick_type();
}
