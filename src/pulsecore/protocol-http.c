/***
  This file is part of PulseAudio.

  Copyright 2005-2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <pulse/util.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/ioline.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/macro.h>
#include <pulsecore/log.h>
#include <pulsecore/namereg.h>
#include <pulsecore/cli-text.h>
#include <pulsecore/shared.h>
#include <pulsecore/core-error.h>

#include "protocol-http.h"

/* Don't allow more than this many concurrent connections */
#define MAX_CONNECTIONS 10

#define URL_ROOT "/"
#define URL_CSS "/style"
#define URL_STATUS "/status"
#define URL_LISTEN "/listen"
#define URL_LISTEN_SOURCE "/listen/source/"

#define MIME_HTML "text/html; charset=utf-8"
#define MIME_TEXT "text/plain; charset=utf-8"
#define MIME_CSS "text/css"

#define HTML_HEADER(t)                                                  \
    "<?xml version=\"1.0\"?>\n"                                         \
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n" \
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"                   \
    "        <head>\n"                                                  \
    "                <title>"t"</title>\n"                              \
    "                <link rel=\"stylesheet\" type=\"text/css\" href=\"style\"/>\n" \
    "        </head>\n"                                                 \
    "        <body>\n"

#define HTML_FOOTER                                                     \
    "        </body>\n"                                                 \
    "</html>\n"

#define RECORD_BUFFER_SECONDS (5)
#define DEFAULT_SOURCE_LATENCY (300*PA_USEC_PER_MSEC)

enum state {
    STATE_REQUEST_LINE,
    STATE_MIME_HEADER,
    STATE_DATA
};

struct connection {
    pa_http_protocol *protocol;
    pa_iochannel *io;
    pa_ioline *line;
    pa_memblockq *output_memblockq;
    pa_source_output *source_output;
    pa_client *client;
    enum state state;
    char *url;
    pa_module *module;
};

struct pa_http_protocol {
    PA_REFCNT_DECLARE;

    pa_core *core;
    pa_idxset *connections;
};

enum {
    SOURCE_OUTPUT_MESSAGE_POST_DATA = PA_SOURCE_OUTPUT_MESSAGE_MAX
};

/* Called from main context */
static void connection_unlink(struct connection *c) {
    pa_assert(c);

    if (c->source_output) {
        pa_source_output_unlink(c->source_output);
        pa_source_output_unref(c->source_output);
    }

    if (c->client)
        pa_client_free(c->client);

    pa_xfree(c->url);

    if (c->line)
        pa_ioline_unref(c->line);

    if (c->io)
        pa_iochannel_free(c->io);

    if (c->output_memblockq)
        pa_memblockq_free(c->output_memblockq);

    pa_idxset_remove_by_data(c->protocol->connections, c, NULL);

    pa_xfree(c);
}

/* Called from main context */
static int do_write(struct connection *c) {
    pa_memchunk chunk;
    ssize_t r;
    void *p;

    pa_assert(c);

    if (pa_memblockq_peek(c->output_memblockq, &chunk) < 0)
        return 0;

    pa_assert(chunk.memblock);
    pa_assert(chunk.length > 0);

    p = pa_memblock_acquire(chunk.memblock);
    r = pa_iochannel_write(c->io, (uint8_t*) p+chunk.index, chunk.length);
    pa_memblock_release(chunk.memblock);

    pa_memblock_unref(chunk.memblock);

    if (r < 0) {

        if (errno == EINTR || errno == EAGAIN)
            return 0;

        pa_log("write(): %s", pa_cstrerror(errno));
        return -1;
    }

    pa_memblockq_drop(c->output_memblockq, (size_t) r);

    return 0;
}

/* Called from main context */
static void do_work(struct connection *c) {
    pa_assert(c);

    if (pa_iochannel_is_hungup(c->io))
        goto fail;

    if (pa_iochannel_is_writable(c->io))
        if (do_write(c) < 0)
            goto fail;

    return;

fail:
    connection_unlink(c);
}

/* Called from thread context, except when it is not */
static int source_output_process_msg(pa_msgobject *m, int code, void *userdata, int64_t offset, pa_memchunk *chunk) {
    pa_source_output *o = PA_SOURCE_OUTPUT(m);
    struct connection *c;

    pa_source_output_assert_ref(o);
    pa_assert_se(c = o->userdata);

    switch (code) {

        case SOURCE_OUTPUT_MESSAGE_POST_DATA:
            /* While this function is usually called from IO thread
             * context, this specific command is not! */
            pa_memblockq_push_align(c->output_memblockq, chunk);
            do_work(c);
            break;

        default:
            return pa_source_output_process_msg(m, code, userdata, offset, chunk);
    }

    return 0;
}

/* Called from thread context */
static void source_output_push_cb(pa_source_output *o, const pa_memchunk *chunk) {
    struct connection *c;

    pa_source_output_assert_ref(o);
    pa_assert_se(c = o->userdata);
    pa_assert(chunk);

    pa_asyncmsgq_post(pa_thread_mq_get()->outq, PA_MSGOBJECT(o), SOURCE_OUTPUT_MESSAGE_POST_DATA, NULL, 0, chunk, NULL);
}

/* Called from main context */
static void source_output_kill_cb(pa_source_output *o) {
    struct connection*c;

    pa_source_output_assert_ref(o);
    pa_assert_se(c = o->userdata);

    connection_unlink(c);
}

/* Called from main context */
static pa_usec_t source_output_get_latency_cb(pa_source_output *o) {
    struct connection*c;

    pa_source_output_assert_ref(o);
    pa_assert_se(c = o->userdata);

    return pa_bytes_to_usec(pa_memblockq_get_length(c->output_memblockq), &c->source_output->sample_spec);
}

/*** client callbacks ***/
static void client_kill_cb(pa_client *client) {
    struct connection*c;

    pa_assert(client);
    pa_assert_se(c = client->userdata);

    connection_unlink(c);
}

/*** pa_iochannel callbacks ***/
static void io_callback(pa_iochannel*io, void *userdata) {
    struct connection *c = userdata;

    pa_assert(c);
    pa_assert(io);

    do_work(c);
}

static pa_bool_t is_mime_sample_spec(const pa_sample_spec *ss, const pa_channel_map *cm) {

    pa_assert(pa_channel_map_compatible(cm, ss));

    switch (ss->format) {
        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_U8:

            if (ss->rate != 8000 &&
                ss->rate != 11025 &&
                ss->rate != 16000 &&
                ss->rate != 22050 &&
                ss->rate != 24000 &&
                ss->rate != 32000 &&
                ss->rate != 44100 &&
                ss->rate != 48000)
                return FALSE;

            if (ss->channels != 1 &&
                ss->channels != 2)
                return FALSE;

            if ((cm->channels == 1 && cm->map[0] != PA_CHANNEL_POSITION_MONO) ||
                (cm->channels == 2 && (cm->map[0] != PA_CHANNEL_POSITION_LEFT || cm->map[1] != PA_CHANNEL_POSITION_RIGHT)))
                return FALSE;

            return TRUE;

        case PA_SAMPLE_ULAW:

            if (ss->rate != 8000)
                return FALSE;

            if (ss->channels != 1)
                return FALSE;

            if (cm->map[0] != PA_CHANNEL_POSITION_MONO)
                return FALSE;

            return TRUE;

        default:
            return FALSE;
    }
}

static void mimefy_sample_spec(pa_sample_spec *ss, pa_channel_map *cm) {

    pa_assert(pa_channel_map_compatible(cm, ss));

    /* Turns the sample type passed in into the next 'better' one that
     * can be encoded for HTTP. If there is no 'better' one we pick
     * the 'best' one that is 'worse'. */

    if (ss->channels > 2)
        ss->channels = 2;

    if (ss->rate > 44100)
        ss->rate = 48000;
    else if (ss->rate > 32000)
        ss->rate = 44100;
    else if (ss->rate > 24000)
        ss->rate = 32000;
    else if (ss->rate > 22050)
        ss->rate = 24000;
    else if (ss->rate > 16000)
        ss->rate = 22050;
    else if (ss->rate > 11025)
        ss->rate = 16000;
    else if (ss->rate > 8000)
        ss->rate = 11025;
    else
        ss->rate = 8000;

    switch (ss->format) {
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_S32BE:
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_FLOAT32BE:
            ss->format = PA_SAMPLE_S24BE;
            break;

        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S16LE:
            ss->format = PA_SAMPLE_S16BE;
            break;

        case PA_SAMPLE_ULAW:
        case PA_SAMPLE_ALAW:

            if (ss->rate == 8000 && ss->channels == 1)
                ss->format = PA_SAMPLE_ULAW;
            else
                ss->format = PA_SAMPLE_S16BE;
            break;

        case PA_SAMPLE_U8:
            ss->format = PA_SAMPLE_U8;
            break;

        case PA_SAMPLE_MAX:
        case PA_SAMPLE_INVALID:
            pa_assert_not_reached();
    }

    pa_channel_map_init_auto(cm, ss->channels, PA_CHANNEL_MAP_DEFAULT);

    pa_assert(is_mime_sample_spec(ss, cm));
}

static char *sample_spec_to_mime_type(const pa_sample_spec *ss, const pa_channel_map *cm) {
    pa_assert(pa_channel_map_compatible(cm, ss));

    if (!is_mime_sample_spec(ss, cm))
        return NULL;

    switch (ss->format) {

        case PA_SAMPLE_S16BE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_U8:
            return pa_sprintf_malloc("audio/%s; rate=%u; channels=%u",
                                     ss->format == PA_SAMPLE_S16BE ? "L16" :
                                     (ss->format == PA_SAMPLE_S24BE ? "L24" : "L8"),
                                     ss->rate, ss->channels);

        case PA_SAMPLE_ULAW:
            return pa_xstrdup("audio/basic");

        default:
            pa_assert_not_reached();
    }

    pa_assert(pa_sample_spec_valid(ss));
}

static char *mimefy_and_stringify_sample_spec(const pa_sample_spec *_ss, const pa_channel_map *_cm) {
    pa_sample_spec ss = *_ss;
    pa_channel_map cm = *_cm;

    mimefy_sample_spec(&ss, &cm);

    return sample_spec_to_mime_type(&ss, &cm);
}

static char *escape_html(const char *t) {
    pa_strbuf *sb;
    const char *p, *e;

    sb = pa_strbuf_new();

    for (e = p = t; *p; p++) {

        if (*p == '>' || *p == '<' || *p == '&') {

            if (p > e) {
                pa_strbuf_putsn(sb, e, p-e);
                e = p + 1;
            }

            if (*p == '>')
                pa_strbuf_puts(sb, "&gt;");
            else if (*p == '<')
                pa_strbuf_puts(sb, "&lt;");
            else
                pa_strbuf_puts(sb, "&amp;");
        }
    }

    if (p > e)
        pa_strbuf_putsn(sb, e, p-e);

    return pa_strbuf_tostring_free(sb);
}

static void http_response(
        struct connection *c,
        int code,
        const char *msg,
        const char *mime) {

    char *s;

    pa_assert(c);
    pa_assert(msg);
    pa_assert(mime);

    s = pa_sprintf_malloc(
            "HTTP/1.0 %i %s\n"
            "Connection: close\n"
            "Content-Type: %s\n"
            "Cache-Control: no-cache\n"
            "Expires: 0\n"
            "Server: "PACKAGE_NAME"/"PACKAGE_VERSION"\n"
            "\n", code, msg, mime);
    pa_ioline_puts(c->line, s);
    pa_xfree(s);
}

static void html_response(
        struct connection *c,
        int code,
        const char *msg,
        const char *text) {

    char *s;
    pa_assert(c);

    http_response(c, code, msg, MIME_HTML);

    if (!text)
        text = msg;

    s = pa_sprintf_malloc(
            HTML_HEADER("%s")
            "%s"
            HTML_FOOTER,
            text, text);

    pa_ioline_puts(c->line, s);
    pa_xfree(s);

    pa_ioline_defer_close(c->line);
}

static void html_print_field(pa_ioline *line, const char *left, const char *right) {
    char *eleft, *eright;

    eleft = escape_html(left);
    eright = escape_html(right);

    pa_ioline_printf(line,
                     "<tr><td><b>%s</b></td>"
                     "<td>%s</td></tr>\n", eleft, eright);

    pa_xfree(eleft);
    pa_xfree(eright);
}

static void handle_root(struct connection *c) {
    char *t;

    pa_assert(c);

    http_response(c, 200, "OK", MIME_HTML);

    pa_ioline_puts(c->line,
                   HTML_HEADER(PACKAGE_NAME" "PACKAGE_VERSION)
                   "<h1>"PACKAGE_NAME" "PACKAGE_VERSION"</h1>\n"
                   "<table>\n");

    t = pa_get_user_name_malloc();
    html_print_field(c->line, "User Name:", t);
    pa_xfree(t);

    t = pa_get_host_name_malloc();
    html_print_field(c->line, "Host name:", t);
    pa_xfree(t);

    t = pa_machine_id();
    html_print_field(c->line, "Machine ID:", t);
    pa_xfree(t);

    t = pa_uname_string();
    html_print_field(c->line, "System:", t);
    pa_xfree(t);

    t = pa_sprintf_malloc("%lu", (unsigned long) getpid());
    html_print_field(c->line, "Process ID:", t);
    pa_xfree(t);

    pa_ioline_puts(c->line,
                   "</table>\n"
                   "<p><a href=\"" URL_STATUS "\">Show an extensive server status report</a></p>\n"
                   "<p><a href=\"" URL_LISTEN "\">Monitor sinks and sources</a></p>\n"
                   HTML_FOOTER);

    pa_ioline_defer_close(c->line);
}

static void handle_css(struct connection *c) {
    pa_assert(c);

    http_response(c, 200, "OK", MIME_CSS);

    pa_ioline_puts(c->line,
                   "body { color: black; background-color: white; }\n"
                   "a:link, a:visited { color: #900000; }\n"
                   "div.news-date { font-size: 80%; font-style: italic; }\n"
                   "pre { background-color: #f0f0f0; padding: 0.4cm; }\n"
                   ".grey { color: #8f8f8f; font-size: 80%; }"
                   "table {  margin-left: 1cm; border:1px solid lightgrey; padding: 0.2cm; }\n"
                   "td { padding-left:10px; padding-right:10px; }\n");

    pa_ioline_defer_close(c->line);
}

static void handle_status(struct connection *c) {
    char *r;

    pa_assert(c);

    http_response(c, 200, "OK", MIME_TEXT);
    r = pa_full_status_string(c->protocol->core);
    pa_ioline_puts(c->line, r);
    pa_xfree(r);

    pa_ioline_defer_close(c->line);
}

static void handle_listen(struct connection *c) {
    pa_source *source;
    pa_sink *sink;
    uint32_t idx;

    http_response(c, 200, "OK", MIME_HTML);

    pa_ioline_puts(c->line,
                   HTML_HEADER("Listen")
                   "<h2>Sinks</h2>\n"
                   "<p>\n");

    PA_IDXSET_FOREACH(sink, c->protocol->core->sinks, idx) {
        char *t, *m;

        t = escape_html(pa_strna(pa_proplist_gets(sink->proplist, PA_PROP_DEVICE_DESCRIPTION)));
        m = mimefy_and_stringify_sample_spec(&sink->sample_spec, &sink->channel_map);

        pa_ioline_printf(c->line,
                         "<a href=\"" URL_LISTEN_SOURCE "%s\" title=\"%s\">%s</a><br/>\n",
                         sink->monitor_source->name, m, t);

        pa_xfree(t);
        pa_xfree(m);
    }

    pa_ioline_puts(c->line,
                   "</p>\n"
                   "<h2>Sources</h2>\n"
                   "<p>\n");

    PA_IDXSET_FOREACH(source, c->protocol->core->sources, idx) {
        char *t, *m;

        if (source->monitor_of)
            continue;

        t = escape_html(pa_strna(pa_proplist_gets(source->proplist, PA_PROP_DEVICE_DESCRIPTION)));
        m = mimefy_and_stringify_sample_spec(&source->sample_spec, &source->channel_map);

        pa_ioline_printf(c->line,
                         "<a href=\"" URL_LISTEN_SOURCE "%s\" title=\"%s\">%s</a><br/>\n",
                         source->name, m, t);

        pa_xfree(m);
        pa_xfree(t);

    }

    pa_ioline_puts(c->line,
                   "</p>\n"
                   HTML_FOOTER);

    pa_ioline_defer_close(c->line);
}

static void line_drain_callback(pa_ioline *l, void *userdata) {
    struct connection *c;

    pa_assert(l);
    pa_assert_se(c = userdata);

    /* We don't need the line reader anymore, instead we need a real
     * binary io channel */
    pa_assert_se(c->io = pa_ioline_detach_iochannel(c->line));
    pa_iochannel_set_callback(c->io, io_callback, c);

    pa_iochannel_socket_set_sndbuf(c->io, pa_memblockq_get_length(c->output_memblockq));

    pa_ioline_unref(c->line);
    c->line = NULL;
}

static void handle_listen_prefix(struct connection *c, const char *source_name) {
    pa_source *source;
    pa_source_output_new_data data;
    pa_sample_spec ss;
    pa_channel_map cm;
    char *t;
    size_t l;

    pa_assert(c);
    pa_assert(source_name);

    pa_assert(c->line);
    pa_assert(!c->io);

    if (!(source = pa_namereg_get(c->protocol->core, source_name, PA_NAMEREG_SOURCE))) {
        html_response(c, 404, "Source not found", NULL);
        return;
    }

    ss = source->sample_spec;
    cm = source->channel_map;

    mimefy_sample_spec(&ss, &cm);

    pa_source_output_new_data_init(&data);
    data.driver = __FILE__;
    data.module = c->module;
    data.client = c->client;
    data.source = source;
    pa_proplist_update(data.proplist, PA_UPDATE_MERGE, c->client->proplist);
    pa_source_output_new_data_set_sample_spec(&data, &ss);
    pa_source_output_new_data_set_channel_map(&data, &cm);

    pa_source_output_new(&c->source_output, c->protocol->core, &data, 0);
    pa_source_output_new_data_done(&data);

    if (!c->source_output) {
        html_response(c, 403, "Cannot create source output", NULL);
        return;
    }

    c->source_output->parent.process_msg = source_output_process_msg;
    c->source_output->push = source_output_push_cb;
    c->source_output->kill = source_output_kill_cb;
    c->source_output->get_latency = source_output_get_latency_cb;
    c->source_output->userdata = c;

    pa_source_output_set_requested_latency(c->source_output, DEFAULT_SOURCE_LATENCY);

    l = (size_t) (pa_bytes_per_second(&ss)*RECORD_BUFFER_SECONDS);
    c->output_memblockq = pa_memblockq_new(
            0,
            l,
            0,
            pa_frame_size(&ss),
            1,
            0,
            0,
            NULL);

    pa_source_output_put(c->source_output);

    t = sample_spec_to_mime_type(&ss, &cm);
    http_response(c, 200, "OK", t);
    pa_xfree(t);

    pa_ioline_set_callback(c->line, NULL, NULL);

    if (pa_ioline_is_drained(c->line))
        line_drain_callback(c->line, c);
    else
        pa_ioline_set_drain_callback(c->line, line_drain_callback, c);
}

static void handle_url(struct connection *c) {
    pa_assert(c);

    pa_log_debug("Request for %s", c->url);

    if (pa_streq(c->url, URL_ROOT))
        handle_root(c);
    else if (pa_streq(c->url, URL_CSS))
        handle_css(c);
    else if (pa_streq(c->url, URL_STATUS))
        handle_status(c);
    else if (pa_streq(c->url, URL_LISTEN))
        handle_listen(c);
    else if (pa_startswith(c->url, URL_LISTEN_SOURCE))
        handle_listen_prefix(c, c->url + sizeof(URL_LISTEN_SOURCE)-1);
    else
        html_response(c, 404, "Not Found", NULL);
}

static void line_callback(pa_ioline *line, const char *s, void *userdata) {
    struct connection *c = userdata;
    pa_assert(line);
    pa_assert(c);

    if (!s) {
        /* EOF */
        connection_unlink(c);
        return;
    }

    switch (c->state) {
        case STATE_REQUEST_LINE: {
            if (!pa_startswith(s, "GET "))
                goto fail;

            s +=4;

            c->url = pa_xstrndup(s, strcspn(s, " \r\n\t?"));
            c->state = STATE_MIME_HEADER;
            break;
        }

        case STATE_MIME_HEADER: {

            /* Ignore MIME headers */
            if (strcspn(s, " \r\n") != 0)
                break;

            /* We're done */
            c->state = STATE_DATA;

            handle_url(c);
            break;
        }

        default:
            ;
    }

    return;

fail:
    html_response(c, 500, "Internal Server Error", NULL);
}

void pa_http_protocol_connect(pa_http_protocol *p, pa_iochannel *io, pa_module *m) {
    struct connection *c;
    pa_client_new_data client_data;
    char pname[128];

    pa_assert(p);
    pa_assert(io);
    pa_assert(m);

    if (pa_idxset_size(p->connections)+1 > MAX_CONNECTIONS) {
        pa_log("Warning! Too many connections (%u), dropping incoming connection.", MAX_CONNECTIONS);
        pa_iochannel_free(io);
        return;
    }

    c = pa_xnew0(struct connection, 1);
    c->protocol = p;
    c->state = STATE_REQUEST_LINE;
    c->module = m;

    c->line = pa_ioline_new(io);
    pa_ioline_set_callback(c->line, line_callback, c);

    pa_client_new_data_init(&client_data);
    client_data.module = c->module;
    client_data.driver = __FILE__;
    pa_iochannel_socket_peer_to_string(io, pname, sizeof(pname));
    pa_proplist_setf(client_data.proplist, PA_PROP_APPLICATION_NAME, "HTTP client (%s)", pname);
    pa_proplist_sets(client_data.proplist, "http-protocol.peer", pname);
    c->client = pa_client_new(p->core, &client_data);
    pa_client_new_data_done(&client_data);

    if (!c->client)
        goto fail;

    c->client->kill = client_kill_cb;
    c->client->userdata = c;

    pa_idxset_put(p->connections, c, NULL);

    return;

fail:
    if (c)
        connection_unlink(c);
}

void pa_http_protocol_disconnect(pa_http_protocol *p, pa_module *m) {
    struct connection *c;
    uint32_t idx;

    pa_assert(p);
    pa_assert(m);

    PA_IDXSET_FOREACH(c, p->connections, idx)
        if (c->module == m)
            connection_unlink(c);
}

static pa_http_protocol* http_protocol_new(pa_core *c) {
    pa_http_protocol *p;

    pa_assert(c);

    p = pa_xnew(pa_http_protocol, 1);
    PA_REFCNT_INIT(p);
    p->core = c;
    p->connections = pa_idxset_new(NULL, NULL);

    pa_assert_se(pa_shared_set(c, "http-protocol", p) >= 0);

    return p;
}

pa_http_protocol* pa_http_protocol_get(pa_core *c) {
    pa_http_protocol *p;

    if ((p = pa_shared_get(c, "http-protocol")))
        return pa_http_protocol_ref(p);

    return http_protocol_new(c);
}

pa_http_protocol* pa_http_protocol_ref(pa_http_protocol *p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) >= 1);

    PA_REFCNT_INC(p);

    return p;
}

void pa_http_protocol_unref(pa_http_protocol *p) {
    struct connection *c;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) >= 1);

    if (PA_REFCNT_DEC(p) > 0)
        return;

    while ((c = pa_idxset_first(p->connections, NULL)))
        connection_unlink(c);

    pa_idxset_free(p->connections, NULL, NULL);

    pa_assert_se(pa_shared_remove(p->core, "http-protocol") >= 0);

    pa_xfree(p);
}
