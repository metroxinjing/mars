// (c) 2010 Thomas Schoebel-Theuer / 1&1 Internet AG
#ifndef BRICK_H
#define BRICK_H

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/kthread.h>

#include <asm/atomic.h>

#ifndef CONFIG_MARS_MODULE
// when unsure, include faked config file
#include "mars_config.h"
#endif

#include "brick_say.h"
#include "meta.h"

#define MAX_BRICK_TYPES 64

#define brick_msleep(msecs) _brick_msleep(msecs, false)
extern int _brick_msleep(int msecs, bool shorten);
#define brick_yield() brick_msleep(0)

/////////////////////////////////////////////////////////////////////////

// printk() replacements

#define SAFE_STR(str) ((str) ? (str) : "NULL")

#define _BRICK_MSG(_class, _dump, _fmt, _args...)		\
	brick_say(_class, _dump, "BRICK", __BASE_FILE__, __LINE__, __FUNCTION__, _fmt, ##_args)

#define BRICK_FAT(_fmt, _args...) _BRICK_MSG(SAY_FATAL, true,  _fmt, ##_args)
#define BRICK_ERR(_fmt, _args...) _BRICK_MSG(SAY_ERROR, true,  _fmt, ##_args)
#define BRICK_DMP(_fmt, _args...) _BRICK_MSG(SAY_ERROR, false, _fmt, ##_args)
#define BRICK_WRN(_fmt, _args...) _BRICK_MSG(SAY_WARN,  false, _fmt, ##_args)
#define BRICK_INF(_fmt, _args...) _BRICK_MSG(SAY_INFO,  false, _fmt, ##_args)

#ifdef BRICK_DEBUGGING
#define BRICK_DBG(_fmt, _args...) _BRICK_MSG(SAY_DEBUG, false, _fmt, ##_args)
#else
#define BRICK_DBG(_args...) /**/
#endif

#include "brick_checking.h"

/////////////////////////////////////////////////////////////////////////

// number management helpers

extern int get_nr(void);
extern void put_nr(int nr);

/////////////////////////////////////////////////////////////////////////

// definitions for generic objects with aspects

struct generic_object;
struct generic_aspect;

#define GENERIC_ASPECT_TYPE(OBJTYPE)					\
	/* readonly from outside */					\
	const char *aspect_type_name;					\
	const struct generic_object_type *object_type;			\
	/* private */							\
	int  aspect_size;						\
        int  (*init_fn)(struct OBJTYPE##_aspect *ini);			\
        void (*exit_fn)(struct OBJTYPE##_aspect *ini);			\

struct generic_aspect_type {
	GENERIC_ASPECT_TYPE(generic);
};

#define GENERIC_OBJECT_TYPE(OBJTYPE)					\
	/* readonly from outside */					\
	const char *object_type_name;					\
	/* private */							\
	int default_size;						\
	int object_type_nr;						\
        int  (*init_fn)(struct OBJTYPE##_object *ini);			\
        void (*exit_fn)(struct OBJTYPE##_object *ini);			\

struct generic_object_type {
	GENERIC_OBJECT_TYPE(generic);
};

#define GENERIC_OBJECT_LAYOUT(OBJTYPE)					\
	/* private */							\
	int size_hint;							\
	atomic_t alloc_count;						\
	atomic_t aspect_count;						\
	atomic_t total_alloc_count;					\
	atomic_t total_aspect_count;					\

struct generic_object_layout {
	GENERIC_OBJECT_LAYOUT(generic);
};

#define GENERIC_OBJECT(OBJTYPE)						\
	/* maintenance, access by macros */				\
	atomic_t ref_count;       /* reference counter */		\
	bool     ref_initialized; /* internally used for checking */	\
	/* readonly from outside */					\
	const struct generic_object_type *object_type;			\
	/* private */							\
	struct generic_object_layout *object_layout;			\
	struct OBJTYPE##_aspect **aspects;				\
	int aspect_nr_max;						\
	int free_offset;						\
	int max_offset;							\
	

struct generic_object {
	GENERIC_OBJECT(generic);
};

#define GENERIC_ASPECT(OBJTYPE)						\
	/* readonly from outside */					\
	struct OBJTYPE##_object *object;				\
	const struct generic_aspect_type *aspect_type;			\
	/* private */							\
	bool shortcut;							\

struct generic_aspect {
	GENERIC_ASPECT(generic);
};

#define _mref_check(mref)						\
	({								\
		if (unlikely(BRICK_CHECKING && !(mref)->ref_initialized)) { \
			MARS_ERR("mref %p is not initialized\n", (mref)); \
		}							\
		CHECK_ATOMIC(&(mref)->ref_count, 1);			\
	})

#define _mref_get_first(mref)						\
	({								\
		if (unlikely(BRICK_CHECKING && (mref)->ref_initialized)) { \
			MARS_ERR("mref %p is already initialized\n", (mref)); \
		}							\
		_CHECK_ATOMIC(&(mref)->ref_count, !=, 0);		\
		(mref)->ref_initialized = true;				\
		atomic_inc(&(mref)->ref_count);				\
	})

#define _mref_get(mref)							\
	({								\
		_mref_check(mref);					\
		atomic_inc(&(mref)->ref_count);				\
	})

#define _mref_put(mref)							\
	({								\
		_mref_check(mref);					\
		atomic_dec_and_test(&(mref)->ref_count);		\
	})

#define _mref_free(mref)						\
	({								\
		if (likely(mref)) {					\
			generic_free((struct generic_object*)(mref));	\
		}							\
	})

/////////////////////////////////////////////////////////////////////////

// definitions for asynchronous callback objects

#define GENERIC_CALLBACK(OBJTYPE)					\
	/* set by macros, afterwards readonly from outside */		\
	void (*cb_fn)(struct OBJTYPE##_callback *cb);			\
	void  *cb_private;						\
	int    cb_error;						\
	/* private */							\
	struct generic_callback *cb_next;				\

struct generic_callback {
	GENERIC_CALLBACK(generic);
};

#define CALLBACK_OBJECT(OBJTYPE)					\
	GENERIC_OBJECT(OBJTYPE);					\
	/* private, access by macros */					\
	struct generic_callback *object_cb;				\
	struct generic_callback _object_cb;				\

struct callback_object {
	CALLBACK_OBJECT(generic);
};

/* Initial setup of the callback chain
 */
#define SETUP_CALLBACK(obj,fn,priv)					\
	(obj)->_object_cb.cb_fn = (fn);					\
	(obj)->_object_cb.cb_private = (priv);				\
	(obj)->_object_cb.cb_error = 0;					\
	(obj)->_object_cb.cb_next = NULL;				\
	(obj)->object_cb = &(obj)->_object_cb;				\

/* Insert a new member into the callback chain
 */
#define INSERT_CALLBACK(obj,new,fn,priv)				\
	if (!(new)->cb_fn) {						\
		(new)->cb_fn = (fn);					\
		(new)->cb_private = (priv);				\
		(new)->cb_error = 0;					\
		(new)->cb_next = (obj)->object_cb;			\
		(obj)->object_cb = (new);				\
	}

/* Call the first callback in the chain.
 */
#define SIMPLE_CALLBACK(obj,err)					\
	if (obj) {							\
		struct generic_callback *__cb = (obj)->object_cb;	\
		if (__cb) {						\
			__cb->cb_error = (err);				\
			__cb->cb_fn(__cb);				\
		}							\
	}

#define CHECKED_CALLBACK(obj,err,done)					\
	{								\
		struct generic_callback *__cb;				\
		CHECK_PTR(obj, done);					\
		__cb = (obj)->object_cb;				\
		CHECK_PTR_NULL(__cb, done);				\
		__cb->cb_error = (err);					\
		__cb->cb_fn(__cb);					\
	}

/* An intermediate callback handler must call this
 * to continue the callback chain.
 */
#define NEXT_CHECKED_CALLBACK(cb,done)					\
	{								\
		struct generic_callback *__next_cb = (cb)->cb_next;	\
		CHECK_PTR_NULL(__next_cb, done);			\
		__next_cb->cb_error = (cb)->cb_error;			\
		__next_cb->cb_fn(__next_cb);				\
	}

/* Query the callback status.
 * This uses always the first member of the chain!
 */
#define CALLBACK_ERROR(obj)						\
	((obj)->object_cb ? (obj)->object_cb->cb_error : -EINVAL)

/////////////////////////////////////////////////////////////////////////

// definitions for generic bricks

struct generic_input;
struct generic_output;
struct generic_brick_ops;
struct generic_output_ops;
struct generic_brick_type;

struct generic_switch {
	/* set by strategy layer, readonly from worker layer */
	bool button;
	/* set by worker layer, readonly from strategy layer */
	bool led_on;
	bool led_off;
	/* private (for any layer) */
	bool force_off;
	int  percent_done;
	wait_queue_head_t event;
};

#define GENERIC_BRICK(BRITYPE)						\
	/* accessible */						\
	struct generic_switch power;					\
	/* set by strategy layer, readonly from worker layer */		\
	const struct BRITYPE##_brick_type *type;			\
	int nr_inputs;							\
	int nr_outputs;							\
	struct BRITYPE##_input **inputs;				\
	struct BRITYPE##_output **outputs;				\
	/* private (for any layer) */					\
	struct BRITYPE##_brick_ops *ops;				\
	int brick_index; /* globally unique */                          \
	int (*free)(struct BRITYPE##_brick *del);			\
	struct list_head tmp_head;					\

struct generic_brick {
	GENERIC_BRICK(generic);
};

#define GENERIC_INPUT(BRITYPE)						\
	/* set by strategy layer, readonly from worker layer */		\
	struct BRITYPE##_brick *brick;					\
	const struct BRITYPE##_input_type *type;			\
	/* private (for any layer) */					\
	struct BRITYPE##_output *connect;				\
	struct list_head input_head;					\
	
struct generic_input {
	GENERIC_INPUT(generic);
};

#define GENERIC_OUTPUT(BRITYPE)						\
	/* set by strategy layer, readonly from worker layer */		\
	struct BRITYPE##_brick *brick;					\
	const struct BRITYPE##_output_type *type;			\
	/* private (for any layer) */					\
	struct BRITYPE##_output_ops *ops;				\
	struct list_head output_head;					\
	int nr_connected;						\
	
struct generic_output {
	GENERIC_OUTPUT(generic);
};

#define GENERIC_OUTPUT_CALL(OUTPUT,OP,ARGS...)				\
	(								\
		(OUTPUT) && (OUTPUT)->ops->OP ?				\
		(OUTPUT)->ops->OP(OUTPUT, ##ARGS) :			\
		-ENOSYS							\
	)
		
#define GENERIC_INPUT_CALL(INPUT,OP,ARGS...)				\
	(							        \
		(INPUT) && (INPUT)->connect ?				\
		GENERIC_OUTPUT_CALL((INPUT)->connect, OP, ##ARGS) :	\
		-ENOTCONN						\
	)

#define GENERIC_BRICK_OPS(BRITYPE)					\
	int (*brick_switch)(struct BRITYPE##_brick *brick);		\
	
struct generic_brick_ops {
	GENERIC_BRICK_OPS(generic);
};

#define GENERIC_OUTPUT_OPS(BRITYPE)					\
	/*int (*output_start)(struct BRITYPE##_output *output);*/	\
	/*int (*output_stop)(struct BRITYPE##_output *output);*/		\
	
struct generic_output_ops {
	GENERIC_OUTPUT_OPS(generic)
};

// although possible, *_type should never be extended
#define GENERIC_BRICK_TYPE(BRITYPE)					\
	/* set by strategy layer, readonly from worker layer */		\
	const char *type_name;						\
	int max_inputs;							\
	int max_outputs;						\
	const struct BRITYPE##_input_type **default_input_types;	\
	const char **default_input_names;				\
	const struct BRITYPE##_output_type **default_output_types;	\
	const char **default_output_names;				\
	/* private (for any layer) */					\
	int brick_size;							\
	struct BRITYPE##_brick_ops *master_ops;				\
	const struct generic_aspect_type **aspect_types;		\
	const struct BRITYPE##_input_types **default_type;		\
	int (*brick_construct)(struct BRITYPE##_brick *brick);		\
	int (*brick_destruct)(struct BRITYPE##_brick *brick);		\

struct generic_brick_type {
	GENERIC_BRICK_TYPE(generic);
};

#define GENERIC_INPUT_TYPE(BRITYPE)					\
	/* set by strategy layer, readonly from worker layer */		\
	char *type_name;						\
	/* private (for any layer) */					\
	int input_size;							\
	int (*input_construct)(struct BRITYPE##_input *input);		\
	int (*input_destruct)(struct BRITYPE##_input *input);		\

struct generic_input_type {
	GENERIC_INPUT_TYPE(generic);
};

#define GENERIC_OUTPUT_TYPE(BRITYPE)					\
	/* set by strategy layer, readonly from worker layer */		\
	char *type_name;						\
	/* private (for any layer) */					\
	int output_size;						\
	struct BRITYPE##_output_ops *master_ops;			\
	int (*output_construct)(struct BRITYPE##_output *output);	\
	int (*output_destruct)(struct BRITYPE##_output *output);	\

struct generic_output_type {
	GENERIC_OUTPUT_TYPE(generic);
};

int generic_register_brick_type(const struct generic_brick_type *new_type);
int generic_unregister_brick_type(const struct generic_brick_type *old_type);

extern void _generic_output_init(struct generic_brick *brick, const struct generic_output_type *type, struct generic_output *output);

extern void _generic_output_exit(struct generic_output *output);

#ifdef _STRATEGY // call this only in strategy bricks, never in ordinary bricks

// you need this only if you circumvent generic_brick_init_full()
extern int generic_brick_init(const struct generic_brick_type *type, struct generic_brick *brick);

extern void generic_brick_exit(struct generic_brick *brick);

extern int generic_input_init(struct generic_brick *brick, int index, const struct generic_input_type *type, struct generic_input *input);

extern void generic_input_exit(struct generic_input *input);

extern int generic_output_init(struct generic_brick *brick, int index, const struct generic_output_type *type, struct generic_output *output);

extern int generic_size(const struct generic_brick_type *brick_type);

extern int generic_connect(struct generic_input *input, struct generic_output *output);

extern int generic_disconnect(struct generic_input *input);

/* If possible, use this instead of generic_*_init().
 * input_types and output_types may be NULL => use default_*_types
 */
int generic_brick_init_full(
	void *data, 
	int size, 
	const struct generic_brick_type *brick_type,
	const struct generic_input_type **input_types,
	const struct generic_output_type **output_types);

int generic_brick_exit_full(
	struct generic_brick *brick);

#endif // _STRATEGY

// simple wrappers for type safety

#define DECLARE_BRICK_FUNCTIONS(BRITYPE)				\
extern inline int BRITYPE##_register_brick_type(void)		        \
{									\
	extern const struct BRITYPE##_brick_type BRITYPE##_brick_type;	\
	extern int BRITYPE##_brick_nr;					\
	if (unlikely(BRITYPE##_brick_nr >= 0)) {			\
		BRICK_ERR("brick type " #BRITYPE " is already registered.\n"); \
		return -EEXIST;						\
	}								\
	BRITYPE##_brick_nr = generic_register_brick_type((const struct generic_brick_type*)&BRITYPE##_brick_type); \
	return BRITYPE##_brick_nr < 0 ? BRITYPE##_brick_nr : 0;		\
}									\
									\
extern inline int BRITYPE##_unregister_brick_type(void)		        \
{									\
	extern const struct BRITYPE##_brick_type BRITYPE##_brick_type;	\
	return generic_unregister_brick_type((const struct generic_brick_type*)&BRITYPE##_brick_type); \
}									\
									\
extern const struct BRITYPE##_brick_type BRITYPE##_brick_type;	        \
extern const struct BRITYPE##_input_type BRITYPE##_input_type;	        \
extern const struct BRITYPE##_output_type BRITYPE##_output_type;        \

///////////////////////////////////////////////////////////////////////

// default operations on objects / aspects

extern struct generic_object *generic_alloc(struct generic_object_layout *object_layout, const struct generic_object_type *object_type);
extern void generic_free(struct generic_object *object);
extern struct generic_aspect *generic_get_aspect(struct generic_brick *brick, struct generic_object *obj);

#define DECLARE_OBJECT_FUNCTIONS(OBJTYPE)				\
extern inline struct OBJTYPE##_object *alloc_##OBJTYPE(struct generic_object_layout *layout) \
{									\
        return (void*)generic_alloc(layout, &OBJTYPE##_type);		\
}

#define DECLARE_ASPECT_FUNCTIONS(BRITYPE,OBJTYPE)			\
									\
extern inline struct OBJTYPE##_object *BRITYPE##_alloc_##OBJTYPE(struct BRITYPE##_brick *brick) \
{									\
        return alloc_##OBJTYPE(&brick->OBJTYPE##_object_layout);	\
}									\
									\
extern inline struct BRITYPE##_##OBJTYPE##_aspect *BRITYPE##_##OBJTYPE##_get_aspect(struct BRITYPE##_brick *brick, struct OBJTYPE##_object *obj) \
{									\
        return (void*)generic_get_aspect((struct generic_brick*)brick, (struct generic_object*)obj); \
}									\
									\


///////////////////////////////////////////////////////////////////////

// some general helpers

#ifdef _STRATEGY // call this only from the strategy implementation

/* Generic interface to simple brick status changes.
 */
extern void set_button(struct generic_switch *sw, bool val, bool force);
extern void set_led_on(struct generic_switch *sw, bool val);
extern void set_led_off(struct generic_switch *sw, bool val);
/*
 * "Forced switch off" means that it cannot be switched on again.
 */
extern void set_button_wait(struct generic_brick *brick, bool val, bool force, int timeout);

#endif

/////////////////////////////////////////////////////////////////////////

// threads

/* Please do not directly use kthreads any more in future.
 * Use these thin abstractions instead.
 */

#define brick_thread_t struct task_struct

#define brick_thread_create(_thread_fn, _data, _fmt, _args...)		\
	({								\
		brick_thread_t *_thr = kthread_create(_thread_fn, _data, _fmt, ##_args);	\
		if (unlikely(IS_ERR(_thr))) {				\
			int _err = PTR_ERR(_thr);			\
			BRICK_ERR("cannot create thread '%s', status = %d\n", _fmt, _err); \
			_thr = NULL;					\
		} else {						\
			struct say_channel *ch = get_binding(current);	\
			if (ch)						\
				bind_to_channel(ch, _thr);		\
			get_task_struct(_thr);				\
			wake_up_process(_thr);				\
		}							\
		_thr;							\
	})

extern void brick_thread_stop_nowait(struct task_struct *k);

#define brick_thread_stop(_thread)					\
	do {								\
		if (likely(_thread)) {					\
			BRICK_INF("stopping thread '%s'\n", (_thread)->comm); \
			kthread_stop(_thread);				\
			BRICK_INF("thread '%s' finished.\n", (_thread)->comm); \
			remove_binding(_thread);			\
			put_task_struct(_thread);			\
			_thread = NULL;					\
		}							\
	} while (0)

#define brick_thread_should_stop()		\
	({					\
		brick_yield();			\
		kthread_should_stop();		\
	})

/////////////////////////////////////////////////////////////////////////

// init

extern int init_brick(void);
extern void exit_brick(void);

#endif
