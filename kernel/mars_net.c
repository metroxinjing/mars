// (c) 2011 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/moduleparam.h>

#include "mars.h"
#include "mars_net.h"

#define USE_BUFFERING

#define SEND_PROTO_VERSION		1

////////////////////////////////////////////////////////////////////

/* Internal data structures for low-level transfer of C structures
 * described by struct meta.
 * Only these low-level fields need to have a fixed size like s64.
 * The size and bytesex of the higher-level C structures is converted
 * automatically; therefore classical "int" or "long long" etc is viable.
 */

#define MAX_FIELD_LEN			(32 + 16)

struct mars_desc_cache {
	u64   cache_sender_cookie;
	u64   cache_recver_cookie;
	u16   cache_sender_proto;
	u16   cache_recver_proto;
	s16   cache_items;
	s8    cache_is_bigendian;
	s8    cache_spare1;
	s32   cache_spare2;
	s32   cache_spare3;
	u64   cache_spare4[4];
};

struct mars_desc_item {
	char  field_name[MAX_FIELD_LEN];
	s8    field_type;
	s8    field_spare0;
	s16   field_data_size;
	s16   field_sender_size;
	s16   field_sender_offset;
	s16   field_recver_size;
	s16   field_recver_offset;
	s32   field_spare;
};

/* This must not be mirror symmetric between big and little endian
 */
#define MARS_DESC_MAGIC 		0x73D0A2EC6148F48Ell

struct mars_desc_header {
	u64 h_magic;
	u64 h_cookie;
	s16 h_meta_len;
	s16 h_index;
	u32 h_spare1;
	u64 h_spare2;
};

#define MAX_INT_TRANSFER		16

////////////////////////////////////////////////////////////////////

/* Bytesex conversion / sign extension
 */

#ifdef __LITTLE_ENDIAN
static const bool myself_is_bigendian = false;
#endif
#ifdef __BIG_ENDIAN
static const bool myself_is_bigendian = true;
#endif

extern inline
void swap_bytes(void *data, int len)
{
	char *a = data;
	char *b = data + len - 1;

	while (a < b) {
		char tmp = *a;
		*a = *b;
		*b = tmp;
		a++;
		b--;
	}
}

#define SWAP_FIELD(x) swap_bytes(&(x), sizeof(x))

extern inline
void swap_mc(struct mars_desc_cache *mc, int len)
{
	struct mars_desc_item *mi;

	SWAP_FIELD(mc->cache_sender_cookie);
	SWAP_FIELD(mc->cache_recver_cookie);
	SWAP_FIELD(mc->cache_items);

	len -= sizeof(*mc);

	for (mi = (void*)(mc + 1); len > 0; mi++, len -= sizeof(*mi)) {
		SWAP_FIELD(mi->field_type);
		SWAP_FIELD(mi->field_data_size);
		SWAP_FIELD(mi->field_sender_size);
		SWAP_FIELD(mi->field_sender_offset);
		SWAP_FIELD(mi->field_recver_size);
		SWAP_FIELD(mi->field_recver_offset);
	}
}

extern inline
char get_sign(const void *data, int len, bool is_bigendian, bool is_signed)
{
	if (is_signed) {
		char x = is_bigendian ?
			((const char*)data)[0] :
			((const char*)data)[len - 1];
		if (x < 0)
			return -1;
	}
	return 0;
}

////////////////////////////////////////////////////////////////////

/* Low-level network traffic
 */

int mars_net_default_port = CONFIG_MARS_DEFAULT_PORT;
EXPORT_SYMBOL_GPL(mars_net_default_port);
module_param_named(mars_port, mars_net_default_port, int, 0);

/* TODO: allow binding to specific source addresses instead of catch-all.
 * TODO: make all the socket options configurable.
 * TODO: implement signal handling.
 * TODO: add authentication.
 * TODO: add compression / encryption.
 */

struct mars_tcp_params default_tcp_params = {
	.ip_tos = IPTOS_LOWDELAY,
	.tcp_window_size = 8 * 1024 * 1024, // for long distance replications
	.tcp_nodelay = 0,
	.tcp_timeout = 2,
	.tcp_keepcnt = 3,
	.tcp_keepintvl = 3, // keepalive ping time
	.tcp_keepidle = 4,
};
EXPORT_SYMBOL(default_tcp_params);

static
void __setsockopt(struct socket *sock, int level, int optname, char *optval, int optsize)
{
	int status = kernel_setsockopt(sock, level, optname, optval, optsize);
	if (status < 0) {
		MARS_WRN("cannot set %d socket option %d to value %d, status = %d\n",
			 level, optname, *(int*)optval, status);
	}
}

#define _setsockopt(sock,level,optname,val) __setsockopt(sock, level, optname, (char*)&(val), sizeof(val))

int mars_create_sockaddr(struct sockaddr_storage *addr, const char *spec)
{
	struct sockaddr_in *sockaddr = (void*)addr;
	const char *new_spec;
	const char *tmp_spec;
	int status = 0;

	memset(addr, 0, sizeof(*addr));
	sockaddr->sin_family = AF_INET;
	sockaddr->sin_port = htons(mars_net_default_port);

	/* Try to translate hostnames to IPs if possible.
	 */
	if (mars_translate_hostname) {
		new_spec = mars_translate_hostname(spec);
	} else {
		new_spec = brick_strdup(spec);
	}
	tmp_spec = new_spec;

	/* This is PROVISIONARY!
	 * TODO: add IPV6 syntax and many more features :)
	 */
	if (!*tmp_spec)
		goto done;
	if (*tmp_spec != ':') {
		unsigned char u0 = 0, u1 = 0, u2 = 0, u3 = 0;
		status = sscanf(tmp_spec, "%hhu.%hhu.%hhu.%hhu", &u0, &u1, &u2, &u3);
		if (status != 4) {
			MARS_ERR("invalid sockaddr IP syntax '%s', status = %d\n", tmp_spec, status);
			status = -EINVAL;
			goto done;
		}
		MARS_DBG("decoded IP = %u.%u.%u.%u\n", u0, u1, u2, u3);
		sockaddr->sin_addr.s_addr = (__be32)u0 | (__be32)u1 << 8 | (__be32)u2 << 16 | (__be32)u3 << 24;
	}
	// deocde port number (when present)
	tmp_spec = spec;
	while (*tmp_spec && *tmp_spec++ != ':')
		/*empty*/;
	if (*tmp_spec) {
		int port = 0;
		status = sscanf(tmp_spec, "%d", &port);
		if (status != 1) {
			MARS_ERR("invalid sockaddr PORT syntax '%s', status = %d\n", tmp_spec, status);
			status = -EINVAL;
			goto done;
		}
		MARS_DBG("decoded PORT = %d\n", port);
		sockaddr->sin_port = htons(port);
	}
	status = 0;
 done:
	brick_string_free(new_spec);
	return status;
}
EXPORT_SYMBOL_GPL(mars_create_sockaddr);

static int current_debug_nr = 0; // no locking, just for debugging

static
void _set_socketopts(struct socket *sock)
{
	struct timeval t = {
		.tv_sec = default_tcp_params.tcp_timeout,
	};
	int x_true = 1;
	/* TODO: improve this by a table-driven approach
	 */
	sock->sk->sk_rcvtimeo = sock->sk->sk_sndtimeo = default_tcp_params.tcp_timeout * HZ;
	sock->sk->sk_reuse = 1;
	_setsockopt(sock, SOL_SOCKET, SO_SNDBUFFORCE, default_tcp_params.tcp_window_size);
	_setsockopt(sock, SOL_SOCKET, SO_RCVBUFFORCE, default_tcp_params.tcp_window_size);
	_setsockopt(sock, SOL_IP, SO_PRIORITY, default_tcp_params.ip_tos);
	_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, default_tcp_params.tcp_nodelay);
	_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, x_true);
	_setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, default_tcp_params.tcp_keepcnt);
	_setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, default_tcp_params.tcp_keepintvl);
	_setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, default_tcp_params.tcp_keepidle);
	_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, t);
	_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, t);

	if (sock->file) { // switch back to blocking mode
		sock->file->f_flags &= ~O_NONBLOCK;
	}
}

int mars_proto_exchange(struct mars_socket *msock, const char *msg)
{
	int status;

	msock->s_send_proto = SEND_PROTO_VERSION;
	status = mars_send_raw(msock, &msock->s_send_proto, 1, false);
	if (unlikely(status < 0)) {
		MARS_DBG("#%d protocol exchange on %s failed at sending, status = %d\n",
			 msock->s_debug_nr,
			 msg,
			 status);
		goto done;
	}
	status = mars_recv_raw(msock, &msock->s_recv_proto, 1, 1);
	if (unlikely(status < 0)) {
		MARS_DBG("#%d protocol exchange on %s failed at receiving, status = %d\n",
			 msock->s_debug_nr,
			 msg,
			 status);
		goto done;
	}
	// take the the minimum of both protocol versions
	if (msock->s_send_proto > msock->s_recv_proto)
		msock->s_send_proto = msock->s_recv_proto;
done:
	return status;
}

int mars_create_socket(struct mars_socket *msock, struct sockaddr_storage *addr, bool is_server)
{
	struct socket *sock;
	struct sockaddr *sockaddr = (void*)addr;
	int status = -EEXIST;

	if (unlikely(atomic_read(&msock->s_count))) {
		MARS_ERR("#%d socket already in use\n", msock->s_debug_nr);
		goto final;
	}
	if (unlikely(msock->s_socket)) {
		MARS_ERR("#%d socket already open\n", msock->s_debug_nr);
		goto final;
	}
	atomic_set(&msock->s_count, 1);

	status = sock_create_kern(AF_INET, SOCK_STREAM, IPPROTO_TCP, &msock->s_socket);
	if (unlikely(status < 0 || !msock->s_socket)) {
		msock->s_socket = NULL;
		MARS_WRN("cannot create socket, status = %d\n", status);
		goto final;
	}
	msock->s_debug_nr = ++current_debug_nr;
	sock = msock->s_socket;
	CHECK_PTR(sock, done);
	msock->s_alive = true;

	_set_socketopts(sock);

	if (is_server) {
		status = kernel_bind(sock, sockaddr, sizeof(*sockaddr));
		if (unlikely(status < 0)) {
			MARS_WRN("#%d bind failed, status = %d\n", msock->s_debug_nr, status);
			goto done;
		}
		status = kernel_listen(sock, 16);
		if (status < 0) {
			MARS_WRN("#%d listen failed, status = %d\n", msock->s_debug_nr, status);
		}
	} else {
		status = kernel_connect(sock, sockaddr, sizeof(*sockaddr), 0);
		if (unlikely(status < 0)) {
			MARS_DBG("#%d connect failed, status = %d\n", msock->s_debug_nr, status);
			goto done;
		}
		status = mars_proto_exchange(msock, "connect");
	}

done:
	if (status < 0) {
		mars_put_socket(msock);
	} else {
		MARS_DBG("successfully created socket #%d\n", msock->s_debug_nr);
	}
final:
	return status;
}
EXPORT_SYMBOL_GPL(mars_create_socket);

int mars_accept_socket(struct mars_socket *new_msock, struct mars_socket *old_msock)
{
	int status = -ENOENT;
	struct socket *new_socket = NULL;
	bool ok;

	ok = mars_get_socket(old_msock);
	if (likely(ok)) {
		struct socket *sock = old_msock->s_socket;
		if (unlikely(!sock)) {
			goto err;
		}

		status = kernel_accept(sock, &new_socket, O_NONBLOCK);
		if (unlikely(status < 0)) {
			goto err;
		}
		if (unlikely(!new_socket)) {
			status = -EBADF;
			goto err;
		}

		_set_socketopts(new_socket);

		memset(new_msock, 0, sizeof(struct mars_socket));
		new_msock->s_socket = new_socket;
		atomic_set(&new_msock->s_count, 1);
		new_msock->s_alive = true;
		new_msock->s_debug_nr = ++current_debug_nr;
		MARS_DBG("#%d successfully accepted socket #%d\n", old_msock->s_debug_nr, new_msock->s_debug_nr);

		status = mars_proto_exchange(new_msock, "accept");
err:
		mars_put_socket(old_msock);
	}
	return status;
}
EXPORT_SYMBOL_GPL(mars_accept_socket);

bool mars_get_socket(struct mars_socket *msock)
{
	if (unlikely(atomic_read(&msock->s_count) <= 0)) {
		MARS_ERR("#%d bad nesting on msock = %p\n", msock->s_debug_nr, msock);
		return false;
	}

	atomic_inc(&msock->s_count);

	if (unlikely(!msock->s_socket || !msock->s_alive)) {
		mars_put_socket(msock);
		return false;
	}
	return true;
}
EXPORT_SYMBOL_GPL(mars_get_socket);

void mars_put_socket(struct mars_socket *msock)
{
	if (unlikely(atomic_read(&msock->s_count) <= 0)) {
		MARS_ERR("#%d bad nesting on msock = %p sock = %p\n", msock->s_debug_nr, msock, msock->s_socket);
	} else if (atomic_dec_and_test(&msock->s_count)) {
		struct socket *sock = msock->s_socket;
		int i;

		MARS_DBG("#%d closing socket %p\n", msock->s_debug_nr, sock);
		if (likely(sock && cmpxchg(&msock->s_alive, true, false))) {
			kernel_sock_shutdown(sock, SHUT_WR);
		}
		if (likely(sock && !msock->s_alive)) {
			MARS_DBG("#%d releasing socket %p\n", msock->s_debug_nr, sock);
			sock_release(sock);
		}
		for (i = 0; i < MAX_DESC_CACHE; i++) {
			if (msock->s_desc_send[i])
				brick_block_free(msock->s_desc_send[i], PAGE_SIZE);
			if (msock->s_desc_recv[i])
				brick_block_free(msock->s_desc_recv[i], PAGE_SIZE);
		}
		brick_block_free(msock->s_buffer, PAGE_SIZE);
		memset(msock, 0, sizeof(struct mars_socket));
	}
}
EXPORT_SYMBOL_GPL(mars_put_socket);

void mars_shutdown_socket(struct mars_socket *msock)
{
	if (msock->s_socket) {
		bool ok = mars_get_socket(msock);
		if (likely(ok)) {
			struct socket *sock = msock->s_socket;
			if (likely(sock && cmpxchg(&msock->s_alive, true, false))) {
				MARS_DBG("#%d shutdown socket %p\n", msock->s_debug_nr, sock);
				kernel_sock_shutdown(sock, SHUT_WR);
			}
			mars_put_socket(msock);
		}
	}
}
EXPORT_SYMBOL_GPL(mars_shutdown_socket);

bool mars_socket_is_alive(struct mars_socket *msock)
{
	bool res = false;
	if (!msock->s_socket || !msock->s_alive)
		goto done;
	if (unlikely(atomic_read(&msock->s_count) <= 0)) {
		MARS_ERR("#%d bad nesting on msock = %p sock = %p\n", msock->s_debug_nr, msock, msock->s_socket);
		goto done;
	}
	res = true;
done:
	return res;
}
EXPORT_SYMBOL_GPL(mars_socket_is_alive);

static
int _mars_send_raw(struct mars_socket *msock, const void *buf, int len)
{
	int sleeptime = 1000 / HZ;
	int sent = 0;
	int status = 0;

	msock->s_send_cnt = 0;
	while (len > 0) {
		int this_len = len;
		struct socket *sock = msock->s_socket;

		if (unlikely(!sock || !mars_net_is_alive || brick_thread_should_stop())) {
			MARS_WRN("interrupting, sent = %d\n", sent);
			status = -EIDRM;
			break;
		}

		{
			struct kvec iov = {
				.iov_base = (void*)buf,
				.iov_len  = this_len,
			};
			struct msghdr msg = {
				.msg_iov = (struct iovec*)&iov,
				.msg_flags = 0 | MSG_NOSIGNAL,
			};
			status = kernel_sendmsg(sock, &msg, &iov, 1, this_len);
		}

		if (status == -EAGAIN) {
			if (msock->s_send_abort > 0 && ++msock->s_send_cnt > msock->s_send_abort) {
				MARS_WRN("#%d reached send abort %d\n", msock->s_debug_nr, msock->s_send_abort);
				status = -EINTR;
				break;
			}
			brick_msleep(sleeptime);
			// linearly increasing backoff
			if (sleeptime < 100) {
				sleeptime += 1000 / HZ;
			}
			continue;
		}
		msock->s_send_cnt = 0;
		if (unlikely(status == -EINTR)) { // ignore it
			flush_signals(current);
			brick_msleep(50);
			continue;
		}
		if (unlikely(!status)) {
			MARS_WRN("#%d EOF from socket upon send_page()\n", msock->s_debug_nr);
			brick_msleep(50);
			status = -ECOMM;
			break;
		}
		if (unlikely(status < 0)) {
			MARS_WRN("#%d bad socket sendmsg, len=%d, this_len=%d, sent=%d, status = %d\n", msock->s_debug_nr, len, this_len, sent, status);
			break;
		}

		len -= status;
		buf += status;
		sent += status;
		sleeptime = 1000 / HZ;
	}

	if (status >= 0)
		status = sent;

	return status;
}

int mars_send_raw(struct mars_socket *msock, const void *buf, int len, bool cork)
{
#ifdef USE_BUFFERING
	int sent = 0;
	int rest = len;
#endif
	int status = -EINVAL;

	if (!mars_get_socket(msock))
		goto final;

#ifdef USE_BUFFERING
restart:
	if (!msock->s_buffer) {
		msock->s_pos = 0;
		msock->s_buffer = brick_block_alloc(0, PAGE_SIZE);
	}

	if (msock->s_pos + rest < PAGE_SIZE) {
		memcpy(msock->s_buffer + msock->s_pos, buf, rest);
		msock->s_pos += rest;
		sent += rest;
		rest = 0;
		status = sent;
		if (cork)
			goto done;
	}

	if (msock->s_pos > 0) {
		status = _mars_send_raw(msock, msock->s_buffer, msock->s_pos);
		if (status < 0)
			goto done;

		brick_block_free(msock->s_buffer, PAGE_SIZE);
		msock->s_buffer = NULL;
		msock->s_pos = 0;
	}

	if (rest >= PAGE_SIZE) {
		status = _mars_send_raw(msock, buf, rest);
		goto done;
	} else if (rest > 0) {
		goto restart;
	}
	status = sent;

done:
#else
	status = _mars_send_raw(msock, buf, len);
#endif
	if (status < 0 && msock->s_shutdown_on_err)
		mars_shutdown_socket(msock);

	mars_put_socket(msock);

final:
	return status;
}
EXPORT_SYMBOL_GPL(mars_send_raw);

/* Note: buf may be NULL. In this case, the data is simply consumed,
 * like /dev/null
 */
int mars_recv_raw(struct mars_socket *msock, void *buf, int minlen, int maxlen)
{
	void *dummy = NULL;
	int sleeptime = 1000 / HZ;
	int status = -EIDRM;
	int done = 0;

	if (!buf) {
		buf = dummy = brick_block_alloc(0, maxlen);
	}

	if (!mars_get_socket(msock))
		goto final;

	msock->s_recv_cnt = 0;

	while (done < minlen) {
		struct kvec iov = {
			.iov_base = buf + done,
			.iov_len = maxlen - done,
		};
		struct msghdr msg = {
			.msg_iovlen = 1,
			.msg_iov = (struct iovec*)&iov,
			.msg_flags = MSG_NOSIGNAL,
		};
		struct socket *sock = msock->s_socket;

		if (unlikely(!sock)) {
			MARS_WRN("#%d socket has disappeared\n", msock->s_debug_nr);
			status = -EIDRM;
			goto err;
		}

		if (!mars_net_is_alive || brick_thread_should_stop()) {
			MARS_WRN("#%d interrupting, done = %d\n", msock->s_debug_nr, done);
			if (done > 0)
				status = -EIDRM;
			goto err;
		}

		status = kernel_recvmsg(sock, &msg, &iov, 1, maxlen-done, msg.msg_flags);

		if (!mars_net_is_alive || brick_thread_should_stop()) {
			MARS_WRN("#%d interrupting, done = %d\n", msock->s_debug_nr, done);
			if (done > 0)
				status = -EIDRM;
			goto err;
		}

		if (status == -EAGAIN) {
			if (msock->s_recv_abort > 0 && ++msock->s_recv_cnt > msock->s_recv_abort) {
				MARS_WRN("#%d reached recv abort %d\n", msock->s_debug_nr, msock->s_recv_abort);
				status = -EINTR;
				goto err;
			}
			brick_msleep(sleeptime);
			// linearly increasing backoff
			if (sleeptime < 100) {
				sleeptime += 1000 / HZ;
			}
			continue;
		}
		msock->s_recv_cnt = 0;
		if (!status) { // EOF
			MARS_WRN("#%d got EOF from socket (done=%d, req_size=%d)\n", msock->s_debug_nr, done, maxlen - done);
			status = -EPIPE;
			goto err;
		}
		if (status < 0) {
			MARS_WRN("#%d bad recvmsg, status = %d\n", msock->s_debug_nr, status);
			goto err;
		}
		done += status;
		sleeptime = 1000 / HZ;
	}
	status = done;

err:
	if (status < 0 && msock->s_shutdown_on_err)
		mars_shutdown_socket(msock);
	mars_put_socket(msock);
final:
	if (dummy)
		brick_block_free(dummy, maxlen);
	return status;
}
EXPORT_SYMBOL_GPL(mars_recv_raw);

///////////////////////////////////////////////////////////////////////

/* Mid-level field data exchange
 */

static
int _add_fields(struct mars_desc_item *mi, const struct meta *meta, int offset, const char *prefix, int maxlen)
{
	int count = 0;
	for (; meta->field_name != NULL; meta++) {
		const char *new_prefix;
		int new_offset;
		int len;
		short this_size;

		new_prefix = mi->field_name;
		new_offset = offset + meta->field_offset;

		if (unlikely(maxlen < sizeof(struct mars_desc_item))) {
			MARS_ERR("desc cache item overflow\n");
			count = -1;
			goto done;
		}

		len = snprintf(mi->field_name, MAX_FIELD_LEN, "%s.%s", prefix, meta->field_name);
		if (unlikely(len >= MAX_FIELD_LEN)) {
			MARS_ERR("field len overflow on '%s.%s'\n", prefix, meta->field_name);
			count = -1;
			goto done;
		}
		mi->field_type = meta->field_type;
		this_size = meta->field_data_size;
		mi->field_data_size = this_size;
		mi->field_sender_size = this_size;
		this_size = meta->field_transfer_size;
		if (this_size > 0) {
			mi->field_sender_size = this_size;
		}
		mi->field_sender_offset = new_offset;
		mi->field_recver_offset = -1;

		mi++;
		maxlen -= sizeof(struct mars_desc_item);
		count++;

		if (meta->field_type == FIELD_SUB) {
			int sub_count;
			sub_count = _add_fields(mi, meta->field_ref, new_offset, new_prefix, maxlen);
			if (sub_count < 0)
				return sub_count;

			mi += sub_count;
			count += sub_count;
			maxlen -= sub_count * sizeof(struct mars_desc_item);
		}
	}
done:
	return count;
}

static
struct mars_desc_cache *make_sender_cache(struct mars_socket *msock, const struct meta *meta, int *cache_index)
{
	int orig_len = PAGE_SIZE;
	int maxlen = orig_len;
	struct mars_desc_cache *mc;
	struct mars_desc_item *mi;
	int i;
	int status;

	for (i = 0; i < MAX_DESC_CACHE; i++) {
		mc = msock->s_desc_send[i];
		if (!mc)
			break;
		if (mc->cache_sender_cookie == (u64)meta)
			goto done;
	}

	if (unlikely(i >= MAX_DESC_CACHE - 1)) {
		MARS_ERR("#%d desc cache overflow\n", msock->s_debug_nr);
		return NULL;
	}

	mc = brick_block_alloc(0, maxlen);

	memset(mc, 0, maxlen);
	mc->cache_sender_cookie = (u64)meta;
	// further bits may be used in future
	mc->cache_sender_proto = msock->s_send_proto;
	mc->cache_recver_proto = msock->s_recv_proto;

	maxlen -= sizeof(struct mars_desc_cache);
	mi = (void*)(mc + 1);

	status = _add_fields(mi, meta, 0, "", maxlen);

	if (likely(status > 0)) {
		mc->cache_items = status;
		mc->cache_is_bigendian = myself_is_bigendian;
		msock->s_desc_send[i] = mc;
		*cache_index = i;
	} else {
		brick_block_free(mc, orig_len);
		mc = NULL;
	}

done:
	return mc;
}

static
void _make_recver_cache(struct mars_desc_cache *mc, const struct meta *meta, int offset, const char *prefix)
{
	char *tmp = brick_string_alloc(MAX_FIELD_LEN);
	int i;

	for (; meta->field_name != NULL; meta++) {
		snprintf(tmp, MAX_FIELD_LEN, "%s.%s", prefix, meta->field_name);
		for (i = 0; i < mc->cache_items; i++) {
			struct mars_desc_item *mi = ((struct mars_desc_item*)(mc + 1)) + i;
			if (meta->field_type == mi->field_type &&
			    !strcmp(tmp, mi->field_name)) {
				mi->field_recver_size = meta->field_data_size;
				mi->field_recver_offset = offset + meta->field_offset;
				if (meta->field_type == FIELD_SUB) {
					_make_recver_cache(mc, meta->field_ref, mi->field_recver_offset, tmp);
				}
				goto found;
			}
		}
		MARS_WRN("field '%s' is missing\n", meta->field_name);
	found:;
	}
	brick_string_free(tmp);
}

static
void make_recver_cache(struct mars_desc_cache *mc, const struct meta *meta)
{
	int i;

	_make_recver_cache(mc, meta, 0, "");

	for (i = 0; i < mc->cache_items; i++) {
		struct mars_desc_item *mi = ((struct mars_desc_item*)(mc + 1)) + i;
		if (unlikely(mi->field_recver_offset < 0)) {
			MARS_WRN("field '%s' is not transferred\n", mi->field_name);
		}
	}
}

#define _CHECK_STATUS(_txt_)						\
	if (unlikely(status < 0)) {					\
		MARS_DBG("%s status = %d\n", _txt_, status);		\
		goto err;						\
	}

static
int _desc_send_item(struct mars_socket *msock, const void *data, const struct mars_desc_cache *mc, int index, bool cork)
{
	struct mars_desc_item *mi = ((struct mars_desc_item*)(mc + 1)) + index;
	const void *item = data + mi->field_sender_offset;
	s16 data_len = mi->field_data_size;
	s16 transfer_len = mi->field_sender_size;
	int status;
	bool is_signed = false;
	int res = -1;

	switch (mi->field_type) {
	case FIELD_REF:
		MARS_ERR("field '%s' NYI type = %d\n", mi->field_name, mi->field_type);
		goto err;
	case FIELD_SUB:
		/* skip this */
		res = 0;
		break;
	case FIELD_INT:
		is_signed = true;
		/* fallthrough */
	case FIELD_UINT:
		if (unlikely(data_len <= 0 || data_len > MAX_INT_TRANSFER)) {
			MARS_ERR("field '%s' bad data_len = %d\n", mi->field_name, data_len);
			goto err;
		}
		if (unlikely(transfer_len > MAX_INT_TRANSFER)) {
			MARS_ERR("field '%s' bad transfer_len = %d\n", mi->field_name, transfer_len);
			goto err;
		}

		if (likely(data_len == transfer_len))
			goto raw;

		if (transfer_len > data_len) {
			int diff = transfer_len - data_len;
			char empty[diff];
			char sign;

			sign = get_sign(item, data_len, myself_is_bigendian, is_signed);
			memset(empty, sign, diff);

			if (myself_is_bigendian) {
				status = mars_send_raw(msock, empty, diff, true);
				_CHECK_STATUS("send_diff");
				status = mars_send_raw(msock, item, data_len, cork);
				_CHECK_STATUS("send_item");

			} else {
				status = mars_send_raw(msock, item, data_len, true);
				_CHECK_STATUS("send_item");
				status = mars_send_raw(msock, empty, diff, cork);
				_CHECK_STATUS("send_diff");
			}

			res = data_len;
			break;
		} else if (unlikely(transfer_len <= 0)) {
			MARS_ERR("bad transfer_len = %d\n", transfer_len);
			goto err;
		} else { // transfer_len < data_len
			char check = get_sign(item, data_len, myself_is_bigendian, is_signed);
			int start;
			int end;
			int i;

			if (is_signed &&
			    unlikely(get_sign(item, transfer_len, myself_is_bigendian, true) != check)) {
				MARS_ERR("cannot sign-reduce signed integer from %d to %d bytes, byte %d !~ %d\n",
					 data_len,
					 transfer_len,
					 ((char*)item)[transfer_len - 1],
					 check);
				goto err;
			}

			if (myself_is_bigendian) {
				start = 0;
				end = data_len - transfer_len;
			} else {
				start = transfer_len;
				end = data_len;
			}

			for (i = start; i < end; i++) {
				if (unlikely(((char*)item)[i] != check)) {
					MARS_ERR("cannot sign-reduce %ssigned integer from %d to %d bytes at pos %d, byte %d != %d\n",
						 is_signed ? "" : "un",
						 data_len,
						 transfer_len,
						 i,
						 ((char*)item)[i],
						 check);
					goto err;
				}
			}

			// just omit the higher/lower bytes
			data_len = transfer_len;
			if (myself_is_bigendian) {
				item += end;
			}
			goto raw;
		}
	case FIELD_STRING:
		item = *(void**)item;
		data_len = 0;
		if (item)
			data_len = strlen(item) + 1;

		status = mars_send_raw(msock, &data_len, sizeof(data_len), true);
		_CHECK_STATUS("send_string_len");
		/* fallthrough */
	case FIELD_RAW:
	raw:
		if (unlikely(data_len < 0)) {
			MARS_ERR("field '%s' bad data_len = %d\n", mi->field_name, data_len);
			goto err;
		}
		status = mars_send_raw(msock, item, data_len, cork);
		_CHECK_STATUS("send_raw");
		res = data_len;
		break;
	default:
		MARS_ERR("field '%s' unknown type = %d\n", mi->field_name, mi->field_type);
	}
err:
	return res;
}

static
int _desc_recv_item(struct mars_socket *msock, void *data, const struct mars_desc_cache *mc, int index, int line)
{
	struct mars_desc_item *mi = ((struct mars_desc_item*)(mc + 1)) + index;
	void *item = NULL;
	s16 data_len = mi->field_recver_size;
	s16 transfer_len = mi->field_sender_size;
	int status;
	bool is_signed = false;
	int res = -1;

	if (likely(data && data_len > 0 && mi->field_recver_offset >= 0)) {
		item = data + mi->field_recver_offset;
	}

	switch (mi->field_type) {
	case FIELD_REF:
		MARS_ERR("field '%s' NYI type = %d\n", mi->field_name, mi->field_type);
		goto err;
	case FIELD_SUB:
		/* skip this */
		res = 0;
		break;
	case FIELD_INT:
		is_signed = true;
		/* fallthrough */
	case FIELD_UINT:
		if (unlikely(data_len <= 0 || data_len > MAX_INT_TRANSFER)) {
			MARS_ERR("field '%s' bad data_len = %d\n", mi->field_name, data_len);
			goto err;
		}
		if (unlikely(transfer_len > MAX_INT_TRANSFER)) {
			MARS_ERR("field '%s' bad transfer_len = %d\n", mi->field_name, transfer_len);
			goto err;
		}

		if (likely(data_len == transfer_len))
			goto raw;

		if (transfer_len > data_len) {
			int diff = transfer_len - data_len;
			char empty[diff];
			char check;

			memset(empty, 0, diff);

			if (myself_is_bigendian) {
				status = mars_recv_raw(msock, empty, diff, diff);
				_CHECK_STATUS("recv_diff");
			}

			status = mars_recv_raw(msock, item, data_len, data_len);
			_CHECK_STATUS("recv_item");
			if (unlikely(mc->cache_is_bigendian != myself_is_bigendian && item)) {
				swap_bytes(item, data_len);
			}

			if (!myself_is_bigendian) {
				status = mars_recv_raw(msock, empty, diff, diff);
				_CHECK_STATUS("recv_diff");
			}

			// check that sign extension did no harm
			check = get_sign(empty, diff, mc->cache_is_bigendian, is_signed);
			while (--diff >= 0) {
				if (unlikely(empty[diff] != check)) {
					MARS_ERR("field '%s' %sSIGNED INTEGER OVERFLOW on size reduction from %d to %d, byte %d != %d\n",
						 mi->field_name,
						 is_signed ? "" : "UN",
						 transfer_len,
						 data_len,
						 empty[diff],
						 check);
					goto err;
				}
			}
			if (is_signed && item &&
			    unlikely(get_sign(item, data_len, myself_is_bigendian, true) != check)) {
				MARS_ERR("field '%s' SIGNED INTEGER OVERLOW on reduction from size %d to %d, byte %d !~ %d\n",
					 mi->field_name,
					 transfer_len,
					 data_len,
					 ((char*)item)[data_len - 1],
					 check);
				goto err;
			}

			res = data_len;
			break;
		} else if (unlikely(transfer_len <= 0)) {
			MARS_ERR("field '%s' bad transfer_len = %d\n", mi->field_name, transfer_len);
			goto err;
		} else if (unlikely(!item)) { // shortcut without checks
			data_len = transfer_len;
			goto raw;
		} else { // transfer_len < data_len
			int diff = data_len - transfer_len;
			char *transfer_ptr = item;
			char sign;

			if (myself_is_bigendian) {
				transfer_ptr += diff;
			}

			status = mars_recv_raw(msock, transfer_ptr, transfer_len, transfer_len);
			_CHECK_STATUS("recv_transfer");
			if (unlikely(mc->cache_is_bigendian != myself_is_bigendian)) {
				swap_bytes(transfer_ptr, transfer_len);
			}

			// sign-extend from transfer_len to data_len
			sign = get_sign(transfer_ptr, transfer_len, myself_is_bigendian, is_signed);
			if (myself_is_bigendian) {
				memset(item, sign, diff);
			} else {
				memset(item + transfer_len, sign, diff);
			}

			res = data_len;
			break;
		}
	case FIELD_STRING:
		data_len = 0;
		status = mars_recv_raw(msock, &data_len, sizeof(data_len), sizeof(data_len));
		_CHECK_STATUS("recv_string_len");

		if (unlikely(mc->cache_is_bigendian != myself_is_bigendian)) {
			swap_bytes(&data_len, sizeof(data_len));
		}

		if (data_len > 0 && item) {
			char *str = _brick_string_alloc(data_len, line);
			*(void**)item = str;
			item = str;
		}

		transfer_len = data_len;
		/* fallthrough */
	case FIELD_RAW:
	raw:
		if (unlikely(data_len < 0)) {
			MARS_ERR("field = '%s' implausible data_len = %d\n", mi->field_name, data_len);
			goto err;
		}
		if (likely(data_len > 0)) {
			if (unlikely(transfer_len != data_len)) {
				MARS_ERR("cannot handle generic mismatch in transfer sizes, field = '%s', %d != %d\n", mi->field_name, transfer_len, data_len);
				goto err;
			}
			status = mars_recv_raw(msock, item, data_len, data_len);
			_CHECK_STATUS("recv_raw");
		}
		res = data_len;
		break;
	default:
		MARS_ERR("field '%s' unknown type = %d\n", mi->field_name, mi->field_type);
	}
err:
	return res;
}

static inline
int _desc_send_struct(struct mars_socket *msock, int cache_index, const void *data, int h_meta_len, bool cork)
{
	const struct mars_desc_cache *mc = msock->s_desc_send[cache_index];
	struct mars_desc_header header = {
		.h_magic = MARS_DESC_MAGIC,
		.h_cookie = mc->cache_sender_cookie,
		.h_meta_len = h_meta_len,
		.h_index = data ? cache_index : -1,
	};
	int index;
	int count = 0;
	int status = 0;

	status = mars_send_raw(msock, &header, sizeof(header), cork || data);
	_CHECK_STATUS("send_header");

	if (unlikely(h_meta_len > 0)) {
		status = mars_send_raw(msock, mc, h_meta_len, true);
		_CHECK_STATUS("send_meta");
	}

	if (likely(data)) {
		for (index = 0; index < mc->cache_items; index++) {
			status = _desc_send_item(msock, data, mc, index, cork || index < mc->cache_items-1);
			_CHECK_STATUS("send_cache_item");
			count++;
		}
	}

	if (status >= 0)
		status = count;
err:
	return status;
}

static
int desc_send_struct(struct mars_socket *msock, const void *data, const struct meta *meta, bool cork)
{
	struct mars_desc_cache *mc;
	int i;
	int h_meta_len = 0;
	int status = -EINVAL;

	for (i = 0; i < MAX_DESC_CACHE; i++) {
		mc = msock->s_desc_send[i];
		if (!mc)
			break;
		if (mc->cache_sender_cookie == (u64)meta)
			goto found;
	}

	mc = make_sender_cache(msock, meta, &i);
	if (unlikely(!mc))
		goto done;

	h_meta_len = mc->cache_items * sizeof(struct mars_desc_item) + sizeof(struct mars_desc_cache);

found:
	status = _desc_send_struct(msock, i, data, h_meta_len, cork);

done:
	return status;
}

static
int desc_recv_struct(struct mars_socket *msock, void *data, const struct meta *meta, int line)
{
	struct mars_desc_header header = {};
	struct mars_desc_cache *mc;
	int cache_index;
	int index;
	int count = 0;
	int status = 0;
	bool need_swap = false;

	status = mars_recv_raw(msock, &header, sizeof(header), sizeof(header));
	_CHECK_STATUS("recv_header");

	if (unlikely(header.h_magic != MARS_DESC_MAGIC)) {
		need_swap = true;
		SWAP_FIELD(header.h_magic);
		if (unlikely(header.h_magic != MARS_DESC_MAGIC)) {
			MARS_WRN("#%d called from line %d bad packet header magic = %llx\n", msock->s_debug_nr, line, header.h_magic);
			status = -ENOMSG;
			goto err;
		}
		SWAP_FIELD(header.h_cookie);
		SWAP_FIELD(header.h_meta_len);
		SWAP_FIELD(header.h_index);
	}

	cache_index = header.h_index;
	if (cache_index < 0) { // EOR
		goto done;
	}
	if (unlikely(cache_index >= MAX_DESC_CACHE - 1)) {
		MARS_WRN("#%d called from line %d bad cache index %d\n", msock->s_debug_nr, line, cache_index);
		status = -EBADF;
		goto err;
	}

	mc = msock->s_desc_recv[cache_index];
	if (unlikely(!mc)) {
		if (unlikely(header.h_meta_len <= 0)) {
			MARS_WRN("#%d called from line %d missing meta information\n", msock->s_debug_nr, line);
			status = -ENOMSG;
			goto err;
		}

		mc = _brick_block_alloc(0, PAGE_SIZE, line);

		status = mars_recv_raw(msock, mc, header.h_meta_len, header.h_meta_len);
		if (unlikely(status < 0)) {
			brick_block_free(mc, PAGE_SIZE);
		}
		_CHECK_STATUS("recv_meta");

		if (unlikely(need_swap)) {
			swap_mc(mc, header.h_meta_len);
		}

		make_recver_cache(mc, meta);

		msock->s_desc_recv[cache_index] = mc;
	} else if (unlikely(header.h_meta_len > 0)) {
		MARS_WRN("#%d called from line %d has %d unexpected meta bytes\n", msock->s_debug_nr, line, header.h_meta_len);
	}

	for (index = 0; index < mc->cache_items; index++) {
		status = _desc_recv_item(msock, data, mc, index, line);
		_CHECK_STATUS("recv_cache_item");
		count++;
	}

done:
	if (status >= 0)
		status = count;
err:
	return status;
}

int mars_send_struct(struct mars_socket *msock, const void *data, const struct meta *meta)
{
	return desc_send_struct(msock, data, meta, false);
}
EXPORT_SYMBOL_GPL(mars_send_struct);

int _mars_recv_struct(struct mars_socket *msock, void *data, const struct meta *meta, int line)
{
	return desc_recv_struct(msock, data, meta, line);
}
EXPORT_SYMBOL_GPL(_mars_recv_struct);

///////////////////////////////////////////////////////////////////////

/* High-level transport of mars structures
 */

const struct meta mars_cmd_meta[] = {
	META_INI_SUB(cmd_stamp, struct mars_cmd, mars_timespec_meta),
	META_INI(cmd_code, struct mars_cmd, FIELD_INT),
	META_INI(cmd_int1, struct mars_cmd, FIELD_INT),
	META_INI(cmd_str1, struct mars_cmd, FIELD_STRING),
	{}
};
EXPORT_SYMBOL_GPL(mars_cmd_meta);


int mars_send_aio(struct mars_socket *msock, struct aio_object *aio)
{
	struct mars_cmd cmd = {
		.cmd_code = CMD_AIO,
		.cmd_int1 = aio->io_id,
	};
	int seq = 0;
	int status;

	if (aio->io_rw != 0 && aio->io_data && aio->io_cs_mode < 2)
		cmd.cmd_code |= CMD_FLAG_HAS_DATA;

	get_lamport(&cmd.cmd_stamp);

	status = desc_send_struct(msock, &cmd, mars_cmd_meta, true);
	if (status < 0)
		goto done;

	seq = 0;
	status = desc_send_struct(msock, aio, mars_aio_meta, cmd.cmd_code & CMD_FLAG_HAS_DATA);
	if (status < 0)
		goto done;

	if (cmd.cmd_code & CMD_FLAG_HAS_DATA) {
		status = mars_send_raw(msock, aio->io_data, aio->io_len, false);
	}
done:
	return status;
}
EXPORT_SYMBOL_GPL(mars_send_aio);

int mars_recv_aio(struct mars_socket *msock, struct aio_object *aio, struct mars_cmd *cmd)
{
	int status;

	status = desc_recv_struct(msock, aio, mars_aio_meta, __LINE__);
	if (status < 0)
		goto done;

	set_lamport(&cmd->cmd_stamp);

	if (cmd->cmd_code & CMD_FLAG_HAS_DATA) {
		if (!aio->io_data)
			aio->io_data = brick_zmem_alloc(aio->io_len);
		status = mars_recv_raw(msock, aio->io_data, aio->io_len, aio->io_len);
		if (status < 0)
			MARS_WRN("#%d aio_len = %d, status = %d\n", msock->s_debug_nr, aio->io_len, status);
	}
done:
	return status;
}
EXPORT_SYMBOL_GPL(mars_recv_aio);

int mars_send_cb(struct mars_socket *msock, struct aio_object *aio)
{
	struct mars_cmd cmd = {
		.cmd_code = CMD_CB,
		.cmd_int1 = aio->io_id,
	};
	int seq = 0;
	int status;

	if (aio->io_rw == 0 && aio->io_data && aio->io_cs_mode < 2)
		cmd.cmd_code |= CMD_FLAG_HAS_DATA;

	get_lamport(&cmd.cmd_stamp);

	status = desc_send_struct(msock, &cmd, mars_cmd_meta, true);
	if (status < 0)
		goto done;

	seq = 0;
	status = desc_send_struct(msock, aio, mars_aio_meta, cmd.cmd_code & CMD_FLAG_HAS_DATA);
	if (status < 0)
		goto done;

	if (cmd.cmd_code & CMD_FLAG_HAS_DATA) {
		status = mars_send_raw(msock, aio->io_data, aio->io_len, false);
	}
done:
	return status;
}
EXPORT_SYMBOL_GPL(mars_send_cb);

int mars_recv_cb(struct mars_socket *msock, struct aio_object *aio, struct mars_cmd *cmd)
{
	int status;

	status = desc_recv_struct(msock, aio, mars_aio_meta, __LINE__);
	if (status < 0)
		goto done;

	set_lamport(&cmd->cmd_stamp);

	if (cmd->cmd_code & CMD_FLAG_HAS_DATA) {
		if (!aio->io_data) {
			MARS_WRN("#%d no internal buffer available\n", msock->s_debug_nr);
			status = -EINVAL;
			goto done;
		}
		status = mars_recv_raw(msock, aio->io_data, aio->io_len, aio->io_len);
	}
done:
	return status;
}
EXPORT_SYMBOL_GPL(mars_recv_cb);

////////////////// module init stuff /////////////////////////

char *(*mars_translate_hostname)(const char *name) = NULL;
EXPORT_SYMBOL_GPL(mars_translate_hostname);

bool mars_net_is_alive = false;
EXPORT_SYMBOL_GPL(mars_net_is_alive);

int __init init_mars_net(void)
{
	MARS_INF("init_net()\n");
	mars_net_is_alive = true;
	return 0;
}

void __exit exit_mars_net(void)
{
	mars_net_is_alive = false;
	MARS_INF("exit_net()\n");
}
