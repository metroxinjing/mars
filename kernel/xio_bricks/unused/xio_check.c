// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG

/* Check brick
 * checks various semantic properties, uses watchdog to find lost callbacks.
 */

/* FIXME: this code has been unused for a long time, it is unlikly
 * to work at all.
 */

/* FIXME: improve this a lot!
 * Check really _anything_ in the interface which _could_ go wrong,
 * even by the silliest type of accident!
 */

//#define BRICK_DEBUGGING
//#define XIO_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "../xio.h"

///////////////////////// own type definitions ////////////////////////

#include "xio_check.h"

///////////////////////// own helper functions ////////////////////////

#define CHECK_ERR(output,fmt,args...)					\
	do {								\
		struct check_input *input = (output)->brick->inputs[0]; \
		struct generic_output *other = (void*)input->connect;	\
		if (other) {						\
			XIO_ERR("instance %d/%s: " fmt, 		\
				 (output)->instance_nr, 		\
				 other->type->type_name,		\
				 ##args);				\
		} else {						\
			XIO_ERR("instance %d: " fmt,			\
				 (output)->instance_nr, 		\
				 ##args);				\
		}							\
	} while (0)

static void check_endio(struct generic_callback *cb)
{
	struct check_aio_aspect *aio_a;
	struct aio_object *aio;
	struct check_output *output;
	struct check_input *input;

	aio_a = cb->cb_private;
	CHECK_PTR(aio_a, fatal);
	_CHECK(&aio_a->cb == cb, fatal);

	aio = aio_a->object;
	CHECK_PTR(aio, fatal);

	output = aio_a->output;
	CHECK_PTR(output, fatal);

	input = output->brick->inputs[0];
	CHECK_PTR(input, fatal);

	if (atomic_dec_and_test(&aio_a->callback_count)) {
		atomic_set(&aio_a->callback_count, 1);
		CHECK_ERR(output, "too many callbacks on %p\n", aio);
	}

#ifdef CHECK_LOCK
	spin_lock(&output->check_lock);

	if (list_empty(&aio_a->aio_head)) {
		CHECK_ERR(output, "list entry missing on %p\n", aio);
	}
	list_del_init(&aio_a->aio_head);

	spin_unlock(&output->check_lock);
#else
	(void)flags;
#endif

	aio_a->last_jiffies = jiffies;

	NEXT_CHECKED_CALLBACK(cb, fatal);

	return;
fatal:
	brick_msleep(60000);
	return;
}

#ifdef CHECK_LOCK
static void dump_mem(void *data, int len)
{
	int i;
	char *tmp;
	char *buf = brick_string_alloc(0);

	for (i = 0, tmp = buf; i < len; i++) {
		unsigned char byte = ((unsigned char*)data)[i];
		if (!(i % 8)) {
			if (tmp != buf) {
				say(-1, "%4d: %s\n", i, buf);
			}
			tmp = buf;
		}
		tmp += snprintf(tmp, 1024 - i * 3, " %02x", byte);
	}
	if (tmp != buf) {
		say(-1, "%4d: %s\n", i, buf);
	}
	brick_string_free(buf);
}

static int check_watchdog(void *data)
{
	struct check_output *output = data;
	XIO_INF("watchdog has started.\n");
	while (!brick_thread_should_stop()) {
		struct list_head *h;
		unsigned long now;

		brick_msleep(5000);

		spin_lock(&output->check_lock);

		now = jiffies;
		for (h = output->aio_anchor.next; h != &output->aio_anchor; h = h->next) {
			static int limit = 1;
			const int timeout = 30;
			struct check_aio_aspect *aio_a;
			struct aio_object *aio;
			unsigned long elapsed;

			aio_a = container_of(h, struct check_aio_aspect, aio_head);
			aio = aio_a->object;
			elapsed = now - aio_a->last_jiffies;
			if (elapsed > timeout * HZ && limit-- > 0) {
				struct generic_object_layout *object_layout;
				aio_a->last_jiffies = now + 600 * HZ;
				XIO_INF("================================\n");
				CHECK_ERR(output, "aio %p callback is missing for more than %d seconds.\n", aio, timeout);
				object_layout = aio->object_layout;
				dump_mem(aio, object_layout->size_hint);
				XIO_INF("================================\n");
			}
		}

		spin_unlock(&output->check_lock);
	}
	return 0;
}
#endif

////////////////// own brick / input / output operations //////////////////

static int check_get_info(struct check_output *output, struct xio_info *info)
{
	struct check_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, xio_get_info, info);
}

static int check_io_get(struct check_output *output, struct aio_object *aio)
{
	struct check_input *input = output->brick->inputs[0];
	return GENERIC_INPUT_CALL(input, aio_get, aio);
}

static void check_io_put(struct check_output *output, struct aio_object *aio)
{
	struct check_input *input = output->brick->inputs[0];
	GENERIC_INPUT_CALL(input, aio_put, aio);
}

static void check_io_io(struct check_output *output, struct aio_object *aio)
{
	struct check_input *input = output->brick->inputs[0];
	struct check_aio_aspect *aio_a = check_aio_get_aspect(output->brick, aio);

	CHECK_PTR(aio_a, fatal);

	if (atomic_dec_and_test(&aio_a->call_count)) {
		atomic_set(&aio_a->call_count, 1);
		CHECK_ERR(output, "multiple parallel calls on %p\n", aio);
	}
	atomic_set(&aio_a->callback_count, 2);

#ifdef CHECK_LOCK
	spin_lock(&output->check_lock);

	if (!list_empty(&aio_a->aio_head)) {
		CHECK_ERR(output, "list head not empty on %p\n", aio);
		list_del(&aio_a->aio_head);
	}
	list_add_tail(&aio_a->aio_head, &output->aio_anchor);

	spin_unlock(&output->check_lock);
#else
	(void)flags;
#endif

	aio_a->last_jiffies = jiffies;
	if (!aio_a->installed) {
		aio_a->installed = true;
		aio_a->output = output;
		INSERT_CALLBACK(aio, &aio_a->cb, check_endio, aio_a);
	}

	GENERIC_INPUT_CALL(input, aio_io, aio);

	atomic_inc(&aio_a->call_count);
fatal: ;
}

//////////////// object / aspect constructors / destructors ///////////////

static int check_aio_aspect_init_fn(struct generic_aspect *_ini)
{
	struct check_aio_aspect *ini = (void*)_ini;
#ifdef CHECK_LOCK
	INIT_LIST_HEAD(&ini->aio_head);
#endif
	ini->last_jiffies = jiffies;
	atomic_set(&ini->call_count, 2);
	atomic_set(&ini->callback_count, 1);
	ini->installed = false;
	return 0;
}

static void check_aio_aspect_exit_fn(struct generic_aspect *_ini)
{
	struct check_aio_aspect *ini = (void*)_ini;
	(void)ini;
#ifdef CHECK_LOCK
	if (!list_empty(&ini->aio_head)) {
		struct check_output *output = ini->output;
		if (output) {
			CHECK_ERR(output, "list head not empty on %p\n", ini->object);
			INIT_LIST_HEAD(&ini->aio_head);
		} else {
			CHECK_HEAD_EMPTY(&ini->aio_head);
		}
	}
#endif
}

XIO_MAKE_STATICS(check);

////////////////////// brick constructors / destructors ////////////////////

static int check_brick_construct(struct check_brick *brick)
{
	return 0;
}

static int check_output_construct(struct check_output *output)
{
	static int count = 0;
#ifdef CHECK_LOCK

	spin_lock_init(&output->check_lock);
	INIT_LIST_HEAD(&output->aio_anchor);
	output->watchdog = brick_thread_create(check_watchdog, output, "check_watchdog%d", output->instance_nr);
#endif
	output->instance_nr = ++count;
	return 0;
}

///////////////////////// static structs ////////////////////////

static struct check_brick_ops check_brick_ops = {
};

static struct check_output_ops check_output_ops = {
	.xio_get_info = check_get_info,
	.aio_get = check_io_get,
	.aio_put = check_io_put,
	.aio_io = check_io_io,
};

const struct check_input_type check_input_type = {
	.type_name = "check_input",
	.input_size = sizeof(struct check_input),
};

static const struct check_input_type *check_input_types[] = {
	&check_input_type,
};

const struct check_output_type check_output_type = {
	.type_name = "check_output",
	.output_size = sizeof(struct check_output),
	.master_ops = &check_output_ops,
	.output_construct = &check_output_construct,
};

static const struct check_output_type *check_output_types[] = {
	&check_output_type,
};

const struct check_brick_type check_brick_type = {
	.type_name = "check_brick",
	.brick_size = sizeof(struct check_brick),
	.max_inputs = 1,
	.max_outputs = 1,
	.master_ops = &check_brick_ops,
	.aspect_types = check_aspect_types,
	.default_input_types = check_input_types,
	.default_output_types = check_output_types,
	.brick_construct = &check_brick_construct,
};
EXPORT_SYMBOL_GPL(check_brick_type);

////////////////// module init stuff /////////////////////////

int __init init_xio_check(void)
{
	XIO_INF("init_check()\n");
	return check_register_brick_type();
}

void __exit exit_xio_check(void)
{
	XIO_INF("exit_check()\n");
	check_unregister_brick_type();
}
