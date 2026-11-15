/*
 * Copyright 2011 Vincent Sanders <vince@netsurf-browser.org>
 * ... (版权头略) ...
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <libwapcaplet/libwapcaplet.h>

#include "netsurf/inttypes.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/time.h"
#include "utils/messages.h"
#include "utils/utils.h"
#include "utils/ring.h"
#include "netsurf/fetch.h"
#include "desktop/gui_internal.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/fetchers/resource.h"

/* 引入 HeliumOS 的文件系统 API */
#include "syscall.h"

#define DIRECT_ETAG_VALUE 123456

static const char *fetch_resource_paths[] = {
	"adblock.css", "default.css", "internal.css", "quirks.css", "user.css",
	"credits.html", "licence.html", "welcome.html", "favicon.ico",
	"default.ico", "netsurf.png", "icons/arrow-l.png", "icons/content.png",
	"icons/directory.png", "icons/directory2.png", "icons/hotlist-add.png",
	"icons/hotlist-rmv.png", "icons/search.png"
};

static struct fetch_resource_map_entry {
	lwc_string *path; 
	char file_path[256]; 
} fetch_resource_map[NOF_ELEMENTS(fetch_resource_paths)];

struct fetch_resource_context;

typedef bool (*fetch_resource_handler)(struct fetch_resource_context *);

struct fetch_resource_context {
	struct fetch_resource_context *r_next, *r_prev;
	struct fetch *fetchh; 
	bool aborted; 
	bool locked; 
	nsurl *url; 
	struct fetch_resource_map_entry *entry; 
	fetch_resource_handler handler;
	time_t etag;
};

static struct fetch_resource_context *ring = NULL;
static uint32_t fetch_resource_path_count;

static inline bool fetch_resource_send_callback(const fetch_msg *msg,
		struct fetch_resource_context *ctx)
{
	ctx->locked = true;
	fetch_send_callback(msg, ctx->fetchh);
	ctx->locked = false;
	return ctx->aborted;
}

static bool fetch_resource_send_header(struct fetch_resource_context *ctx,
		const char *fmt, ...)
{
	fetch_msg msg;
	char header[64];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(header, sizeof header, fmt, ap);
	va_end(ap);

	if (len >= (int)sizeof(header) || len < 0) return false;

	msg.type = FETCH_HEADER;
	msg.data.header_or_data.buf = (const uint8_t *) header;
	msg.data.header_or_data.len = len;
	return fetch_resource_send_callback(&msg, ctx);
}

static bool fetch_resource_notfound_handler(struct fetch_resource_context *ctx)
{
	fetch_msg msg;
	fetch_set_http_code(ctx->fetchh, 404);
	fetch_resource_send_header(ctx, "Content-Type: text/html");
	const char *err_html = "<html><body><h1>404 Not Found</h1></body></html>";
	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = (const uint8_t *)err_html;
	msg.data.header_or_data.len = strlen(err_html);
	fetch_resource_send_callback(&msg, ctx);
	msg.type = FETCH_FINISHED;
	fetch_resource_send_callback(&msg, ctx);
	return false;
}

/* 真正干活的 handler：直接读文件 */
static bool fetch_resource_data_handler(struct fetch_resource_context *ctx)
{
	fetch_msg msg;
	int fd = fs_open(ctx->entry->file_path, FS_O_RDONLY);
	
	if (fd < 0) {
		return fetch_resource_notfound_handler(ctx);
	}
	
	struct fstat_buf st;
	fs_fstat(fd, &st);
	uint32_t size = st.size;
	
	uint8_t *buf = malloc(size);
	if (!buf) {
		fs_close(fd);
		return fetch_resource_notfound_handler(ctx);
	}
	
	fs_read(fd, buf, size);
	fs_close(fd);

	// 推断 MIME 类型
	const char *mime = "application/octet-stream";
	const char *ext = strrchr(ctx->entry->file_path, '.');
	if (ext) {
		if (strcmp(ext, ".css") == 0) mime = "text/css";
		else if (strcmp(ext, ".html") == 0) mime = "text/html";
		else if (strcmp(ext, ".png") == 0) mime = "image/png";
		else if (strcmp(ext, ".ico") == 0) mime = "image/x-icon";
	}

	fetch_set_http_code(ctx->fetchh, 200);
	fetch_resource_send_header(ctx, "Content-Type: %s", mime);
	fetch_resource_send_header(ctx, "Content-Length: %d", size);

	msg.type = FETCH_DATA;
	msg.data.header_or_data.buf = buf;
	msg.data.header_or_data.len = size;
	fetch_resource_send_callback(&msg, ctx);

	free(buf);

	if (ctx->aborted == false) {
		msg.type = FETCH_FINISHED;
		fetch_resource_send_callback(&msg, ctx);
	}
	return true;
}

static bool fetch_resource_initialise(lwc_string *scheme)
{
	struct fetch_resource_map_entry *e;
	uint32_t i;

	fetch_resource_path_count = 0;

	for (i = 0; i < NOF_ELEMENTS(fetch_resource_paths); i++) {
		e = &fetch_resource_map[fetch_resource_path_count];

		if (lwc_intern_string(fetch_resource_paths[i],
				strlen(fetch_resource_paths[i]), &e->path) != lwc_error_ok) {
			continue;
		}

		snprintf(e->file_path, sizeof(e->file_path), "SYS:/netsurf/res/%s", fetch_resource_paths[i]);
		fprintf(stderr, "DEBUG_RES_INIT: mapped %s -> %s\n", fetch_resource_paths[i], e->file_path);
		fetch_resource_path_count++;
	}
	return true;
}

static void fetch_resource_finalise(lwc_string *scheme)
{
	uint32_t i;
	for (i = 0; i < fetch_resource_path_count; i++) {
		lwc_string_unref(fetch_resource_map[i].path);
	}
}

static bool fetch_resource_can_fetch(const nsurl *url)
{
	return true;
}

static void *fetch_resource_setup(struct fetch *fetchh, nsurl *url,
		     bool only_2xx, bool downgrade_tls, const char *post_urlenc,
		     const struct fetch_multipart_data *post_multipart, const char **headers)
{
	struct fetch_resource_context *ctx;
	lwc_string *path;
	uint32_t i;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) return NULL;

	ctx->handler = fetch_resource_notfound_handler;

	if ((path = nsurl_get_component(url, NSURL_PATH)) != NULL) {
		bool match;
		for (i = 0; i < fetch_resource_path_count; i++) {
			if (lwc_string_isequal(path, fetch_resource_map[i].path, &match) == lwc_error_ok && match) {
				ctx->entry = &fetch_resource_map[i];
				ctx->handler = fetch_resource_data_handler;
				break;
			}
		}
		lwc_string_unref(path);
	}

	ctx->url = nsurl_ref(url);
	ctx->fetchh = fetchh;
	RING_INSERT(ring, ctx);
	return ctx;
}

static void fetch_resource_free(void *ctx)
{
	struct fetch_resource_context *c = ctx;
	if (c->url != NULL) nsurl_unref(c->url);
	free(ctx);
}

static bool fetch_resource_start(void *ctx)
{
	return true;
}

static void fetch_resource_abort(void *ctx)
{
	struct fetch_resource_context *c = ctx;
	c->aborted = true;
}

static void fetch_resource_poll(lwc_string *scheme)
{
	struct fetch_resource_context *c, *save_ring = NULL;

	while (ring != NULL) {
		c = ring;
		RING_REMOVE(ring, c);

		if (c->locked == true) {
			RING_INSERT(save_ring, c);
			continue;
		}

		if (c->aborted == false) {
			c->handler(c); 
		}

		fetch_remove_from_queues(c->fetchh);
		fetch_free(c->fetchh);
	}
	ring = save_ring;
}

nserror fetch_resource_register(void)
{
	lwc_string *scheme = lwc_string_ref(corestring_lwc_resource);
	const struct fetcher_operation_table fetcher_ops = {
		.initialise = fetch_resource_initialise,
		.acceptable = fetch_resource_can_fetch,
		.setup = fetch_resource_setup,
		.start = fetch_resource_start,
		.abort = fetch_resource_abort,
		.free = fetch_resource_free,
		.poll = fetch_resource_poll,
		.finalise = fetch_resource_finalise
	};
	return fetcher_add(scheme, &fetcher_ops);
}