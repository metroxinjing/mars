// (c) 2011 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "strategy.h"
#include "../mars_net.h"

static
char *_mars_translate_hostname(const char *name)
{
	struct mars_global *global = mars_global;
	char *res = brick_strdup(name);
	struct mars_dent *test;
	char *tmp;

	if (unlikely(!global)) {
		goto done;
	}

	for (tmp = res; *tmp; tmp++) {
		if (*tmp == ':') {
			*tmp = '\0';
			break;
		}
	}

	tmp = path_make("/mars/ips/ip-%s", res);
	if (unlikely(!tmp)) {
		goto done;
	}

	test = mars_find_dent(global, tmp);
	if (test && test->link_val) {
		MARS_DBG("'%s' => '%s'\n", tmp, test->link_val);
		brick_string_free(res);
		res = brick_strdup(test->link_val);
	} else {
		MARS_DBG("no translation for '%s'\n", tmp);
	}
	brick_string_free(tmp);

done:
	return res;
}

int mars_send_dent_list(struct mars_socket *sock, struct list_head *anchor)
{
	struct list_head *tmp;
	struct mars_dent *dent;
	int status = 0;
	for (tmp = anchor->next; tmp != anchor; tmp = tmp->next) {
		dent = container_of(tmp, struct mars_dent, dent_link);
		status = mars_send_struct(sock, dent, mars_dent_meta);
		if (status < 0)
			break;
	}
	if (status >= 0) { // send EOR
		status = mars_send_struct(sock, NULL, mars_dent_meta);
	}
	return status;
}
EXPORT_SYMBOL_GPL(mars_send_dent_list);

int mars_recv_dent_list(struct mars_socket *sock, struct list_head *anchor)
{
	int status;
	for (;;) {
		struct mars_dent *dent = brick_zmem_alloc(sizeof(struct mars_dent));

		status = mars_recv_struct(sock, dent, mars_dent_meta);
		if (status <= 0) {
			mars_free_dent(dent);
			goto done;
		}
		list_add_tail(&dent->dent_link, anchor);
		INIT_LIST_HEAD(&dent->brick_list);
	}
done:
	return status;
}
EXPORT_SYMBOL_GPL(mars_recv_dent_list);


////////////////// module init stuff /////////////////////////


int __init init_sy_net(void)
{
	MARS_INF("init_sy_net()\n");
	mars_translate_hostname = _mars_translate_hostname;
	return 0;
}

void __exit exit_sy_net(void)
{
	MARS_INF("exit_sy_net()\n");
}
