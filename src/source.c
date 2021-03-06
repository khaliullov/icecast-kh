/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org, 
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 */

/* -*- c-basic-offset: 4; indent-tabs-mode: nil; -*- */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <sys/types.h>
#include <ogg/ogg.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#else
#include <winsock2.h>
#include <windows.h>
#endif

#include "thread/thread.h"
#include "avl/avl.h"
#include "httpp/httpp.h"
#include "net/sock.h"

#include "connection.h"
#include "global.h"
#include "refbuf.h"
#include "client.h"
#include "stats.h"
#include "logging.h"
#include "cfgfile.h"
#include "util.h"
#include "source.h"
#include "format.h"
#include "fserve.h"
#include "auth.h"
#include "compat.h"
#include "slave.h"

#undef CATMODULE
#define CATMODULE "source"

#define MAX_FALLBACK_DEPTH 10


/* avl tree helper */
static void _parse_audio_info (source_t *source, const char *s);
static void source_client_release (client_t *client);
static int  source_listener_release (source_t *source, client_t *client);
static int  source_client_read (client_t *client);
static int  source_client_shutdown (client_t *client);
static int  source_client_http_send (client_t *client);
static int  send_to_listener (client_t *client);
static int  send_listener (source_t *source, client_t *client);
static int  wait_for_restart (client_t *client);
static int  wait_for_other_listeners (client_t *client);

static int  http_source_listener (client_t *client);
static int  http_source_intro (client_t *client);
static int  locate_start_on_queue (source_t *source, client_t *client);
static int  listener_change_worker (client_t *client, source_t *source);
static int  source_change_worker (source_t *source);
static int  source_client_callback (client_t *client);
static int  source_set_override (const char *mount, source_t *dest_source, format_type_t type);

#ifdef _WIN32
#define source_run_script(x,y)  WARN0("on [dis]connect scripts disabled");
#else
static void source_run_script (char *command, char *mountpoint);
#endif

struct _client_functions source_client_ops = 
{
    source_client_read,
    client_destroy
};

struct _client_functions source_client_halt_ops = 
{
    source_client_shutdown,
    source_client_release
};

struct _client_functions listener_client_ops = 
{
    send_to_listener,
    client_destroy
};

struct _client_functions listener_pause_ops = 
{
    wait_for_restart,
    client_destroy
};

struct _client_functions listener_wait_ops = 
{
    wait_for_other_listeners,
    client_destroy
};

struct _client_functions source_client_http_ops =
{
    source_client_http_send,
    source_client_release
};


/* Allocate a new source with the stated mountpoint, if one already
 * exists with that mountpoint in the global source tree then return
 * NULL.
 */
source_t *source_reserve (const char *mount, int ret_exist)
{
    source_t *src = NULL;

    do
    {
        avl_tree_wlock (global.source_tree);
        src = source_find_mount_raw (mount);
        if (src)
        {
            if (ret_exist == 0)
                src = NULL;
            else if (src->flags & SOURCE_LISTENERS_SYNC)
                src = NULL;
            break;
        }

        src = calloc (1, sizeof(source_t));
        if (src == NULL)
            break;

        /* make duplicates for strings or similar */
        src->mount = strdup (mount);
        src->listener_send_trigger = 10000;
        src->format = calloc (1, sizeof(format_plugin_t));
        src->clients = avl_tree_new (client_compare, NULL);
        src->stats = stats_handle (mount);

        thread_mutex_create (&src->lock);
        stats_release (src->stats);

        avl_insert (global.source_tree, src);

    } while (0);

    avl_tree_unlock (global.source_tree);
    return src;
}


/* Find a mount with this raw name - ignoring fallbacks. You should have the
 * global source tree locked to call this.
 */
source_t *source_find_mount_raw(const char *mount)
{
    source_t *source;
    avl_node *node;
    int cmp;

    if (!mount) {
        return NULL;
    }
    /* get the root node */
    node = global.source_tree->root->right;
    
    while (node) {
        source = (source_t *)node->key;
        cmp = strcmp (mount, source->mount);
        if (cmp < 0) 
            node = node->left;
        else if (cmp > 0)
            node = node->right;
        else
            return source;
    }
    
    /* didn't find it */
    return NULL;
}


/* Search for mount, if the mount is there but not currently running then
 * check the fallback, and so on.  Must have a global source lock to call
 * this function.
 */
source_t *source_find_mount (const char *mount)
{
    source_t *source = NULL;
    ice_config_t *config;
    mount_proxy *mountinfo;
    int depth = 0;

    config = config_get_config();
    while (mount && depth < MAX_FALLBACK_DEPTH)
    {
        source = source_find_mount_raw (mount);

        if (source)
        {
            if (source_available (source))
                break;
        }

        /* we either have a source which is not active (relay) or no source
         * at all. Check the mounts list for fallback settings
         */
        mountinfo = config_find_mount (config, mount);
        source = NULL;

        if (mountinfo == NULL)
            break;
        mount = mountinfo->fallback_mount;
        depth++;
    }

    config_release_config();
    return source;
}


int source_compare_sources(void *arg, void *a, void *b)
{
    source_t *srca = (source_t *)a;
    source_t *srcb = (source_t *)b;

    return strcmp(srca->mount, srcb->mount);
}


void source_clear_source (source_t *source)
{
    int do_twice = 0;
    refbuf_t *p;

    DEBUG1 ("clearing source \"%s\"", source->mount);

    if (source->dumpfile)
    {
        INFO1 ("Closing dumpfile for %s", source->mount);
        fclose (source->dumpfile);
        source->dumpfile = NULL;
    }

    /* flush out the stream data, we don't want any left over */

    /* the source holds a reference on the very latest so that one
     * always exists */
    refbuf_release (source->stream_data_tail);

    /* remove the reference for buffers on the queue */
    p = source->stream_data;
    while (p)
    {
        refbuf_t *to_go = p;
        p = to_go->next;
        to_go->next = NULL;
        // DEBUG1 ("queue refbuf count is %d", to_go->_count);
        if (do_twice || to_go == source->min_queue_point)
        { /* burst data is also counted */
            refbuf_release (to_go); 
            do_twice = 1;
        }
        refbuf_release (to_go);
    }
    source->min_queue_point = NULL;
    source->stream_data = NULL;
    source->stream_data_tail = NULL;

    source->min_queue_size = 0;
    source->min_queue_offset = 0;
    source->default_burst_size = 0;
    source->queue_size = 0;
    source->queue_size_limit = 0;
    source->client_stats_update = 0;
    util_dict_free (source->audio_info);
    source->audio_info = NULL;

    free(source->dumpfilename);
    source->dumpfilename = NULL;

    if (source->intro_file)
    {
        fclose (source->intro_file);
        source->intro_file = NULL;
    }
}


/* the internal free function. at this point we know the source is
 * not on the source tree */
static int _free_source (void *p)
{
    source_t *source = p;
    source_clear_source (source);

    /* make sure all YP entries have gone */
    yp_remove (source->mount);

    /* There should be no listeners on this mount */
    if (source->listeners)
        WARN3("active listeners on mountpoint %s (%ld, %ld)", source->mount, source->listeners, source->termination_count);
    avl_tree_free (source->clients, NULL);

    thread_mutex_unlock (&source->lock);
    thread_mutex_destroy (&source->lock);

    INFO1 ("freeing source \"%s\"", source->mount);
    format_plugin_clear (source->format, source->client);
    free (source->format);
    free (source->mount);
    free (source);
    return 1;
}


/* Remove the provided source from the global tree and free it */
void source_free_source (source_t *source)
{
    INFO1 ("source %s to be freed", source->mount);
    avl_tree_wlock (global.source_tree);
    thread_mutex_lock (&source->lock);
    DEBUG1 ("removing source %s from tree", source->mount);
    avl_delete (global.source_tree, source, _free_source);
    avl_tree_unlock (global.source_tree);
}


client_t *source_find_client(source_t *source, int id)
{
    client_t fakeclient;
    void *result = NULL;

    fakeclient.connection.id = id;

    avl_get_by_key (source->clients, &fakeclient, &result);
    return result;
}


/* Update stats from source processing, this should be called regulary (every
 * few seconds) to keep totals up to date.
 */
static void update_source_stats (source_t *source)
{
    unsigned long incoming_rate = (long)rate_avg (source->format->in_bitrate);
    unsigned long kbytes_sent = source->bytes_sent_since_update/1024;
    unsigned long kbytes_read = source->bytes_read_since_update/1024;

    source->format->sent_bytes += kbytes_sent*1024;
    source->stats = stats_lock (source->stats, source->mount);
    stats_set_args (source->stats, "outgoing_kbitrate", "%ld",
            (long)(8 * rate_avg (source->format->out_bitrate))/1024);
    stats_set_args (source->stats, "incoming_bitrate", "%ld", (8 * incoming_rate));
    stats_set_args (source->stats, "total_bytes_read", "%"PRIu64, source->format->read_bytes);
    stats_set_args (source->stats, "total_bytes_sent", "%"PRIu64, source->format->sent_bytes);
    stats_set_args (source->stats, "total_mbytes_sent",
            "%"PRIu64, source->format->sent_bytes/(1024*1024));
    stats_set_args (source->stats, "queue_size", "%u", source->queue_size);
    if (source->client->connection.con_time)
    {
        worker_t *worker = source->client->worker;
        stats_set_args (source->stats, "connected", "%"PRIu64,
                (uint64_t)(worker->current_time.tv_sec - source->client->connection.con_time));
    }
    stats_release (source->stats);
    stats_event_add (NULL, "stream_kbytes_sent", kbytes_sent);
    stats_event_add (NULL, "stream_kbytes_read", kbytes_read);

    source->bytes_sent_since_update %= 1024;
    source->bytes_read_since_update %= 1024;
    source->listener_send_trigger = incoming_rate;
}


/* get some data from the source. The stream data is placed in a refbuf
 * and sent back, however NULL is also valid as in the case of a short
 * timeout and there's no data pending.
 */
int source_read (source_t *source)
{
    client_t *client = source->client;
    refbuf_t *refbuf = NULL;
    int skip = 1, loop = 2;
    time_t current = client->worker->current_time.tv_sec;
    int fds = 0;

    if (global.running != ICE_RUNNING)
        source->flags &= ~SOURCE_RUNNING;
    do
    {
        client->schedule_ms = client->worker->time_ms;
        if (source->flags & SOURCE_LISTENERS_SYNC)
        {
            if (source->termination_count)
            {
                if (client->timer_start + 1500 < client->worker->time_ms)
                {
                    source->flags &= ~(SOURCE_RUNNING|SOURCE_LISTENERS_SYNC);
                    WARN1 ("stopping %s as sync mode lasted too long", source->mount);
                }
                client->schedule_ms += 30;
                thread_mutex_unlock (&source->lock);
                return 0;
            }
            if (source->fallback.mount)
            {
                DEBUG1 ("listeners have now moved to %s", source->fallback.mount);
                free (source->fallback.mount);
                source->fallback.mount = NULL;
            }
            source->flags &= ~SOURCE_LISTENERS_SYNC;
        }
        if (source->listeners == 0)
            rate_add (source->format->out_bitrate, 0, client->worker->time_ms);
        if (source->prev_listeners != source->listeners)
        {
            INFO2("listener count on %s now %lu", source->mount, source->listeners);
            source->prev_listeners = source->listeners;
            stats_event_args (source->mount, "listeners", "%lu", source->listeners);
            if (source->listeners > source->peak_listeners)
            {
                source->peak_listeners = source->listeners;
                stats_event_args (source->mount, "listener_peak", "%lu", source->peak_listeners);
            }
        }
        if (current >= source->client_stats_update)
        {
            update_source_stats (source);
            source->client_stats_update = current + source->stats_interval;
        }
        if (current >= source->worker_balance_recheck)
        {
            int recheck = global.sources > 6 ? global.sources : 6;
            source->worker_balance_recheck = current + recheck;
            if (source_change_worker (source))
                return 1;
        }
        fds = util_timed_wait_for_fd (client->connection.sock, 0);
        if (fds < 0)
        {
            if (! sock_recoverable (sock_error()))
            {
                WARN0 ("Error while waiting on socket, Disconnecting source");
                source->flags &= ~SOURCE_RUNNING;
            }
            break;
        }
        if (fds == 0)
        {
            if (source->last_read + (time_t)3 == current)
                WARN1 ("Nothing received on %s for 3 seconds", source->mount);
            if (source->last_read + (time_t)source->timeout < current)
            {
                DEBUG3 ("last %ld, timeout %d, now %ld", (long)source->last_read,
                        source->timeout, (long)current);
                WARN1 ("Disconnecting %s due to socket timeout", source->mount);
                source->flags &= ~SOURCE_RUNNING;
                source->flags |= SOURCE_TIMEOUT;
                skip = 0;
                break;
            }
            source->skip_duration = (int)(source->skip_duration * 1.3);
            if (source->skip_duration > 400)
                source->skip_duration = 400;
            break;
        }
        source->skip_duration = (long)(source->skip_duration * 0.9);
        if (source->skip_duration < 10) /* not too low or else it will not be able to increase */
            source->skip_duration = 10;

        source->last_read = current;
        do
        {
            refbuf = source->format->get_buffer (source);
            if (refbuf)
            {
                source->bytes_read_since_update += refbuf->len;

                refbuf->flags |= SOURCE_QUEUE_BLOCK;
                /* the latest refbuf is counted twice so that it stays */
                refbuf_addref (refbuf);

                /* append buffer to the in-flight data queue,  */
                if (source->stream_data == NULL)
                {
                    source->stream_data = refbuf;
                    source->min_queue_point = refbuf;
                    source->min_queue_offset = 0;
                }
                if (source->stream_data_tail)
                {
                    if (source->min_queue_offset > source->min_queue_size)
                    {
                        ERROR3 ("queue oddity, stream %s, %d, %d", source->mount, source->min_queue_offset, source->min_queue_size);
                        source->flags &= ~SOURCE_RUNNING;
                    }
                    source->stream_data_tail->next = refbuf;
                    refbuf_release (source->stream_data_tail);
                }
                source->stream_data_tail = refbuf;
                source->queue_size += refbuf->len;

                /* increase refcount for keeping burst data */
                refbuf_addref (refbuf);

                /* move the starting point for new listeners */
                source->min_queue_offset += refbuf->len;
                while (source->min_queue_offset > source->min_queue_size)
                {
                    refbuf_t *to_release = source->min_queue_point;
                    if (to_release && to_release->next)
                    {
                        source->min_queue_offset -= to_release->len;
                        source->min_queue_point = to_release->next;
                        refbuf_release (to_release);
                        continue;
                    }
                    if (source->min_queue_point != refbuf)
                    {
                        ERROR0 ("weird state of min_queue point");
                        abort();
                    }
                    break;
                }

                /* save stream to file */
                if (source->dumpfile && source->format->write_buf_to_file)
                    source->format->write_buf_to_file (source, refbuf);
                skip = 0;
            }
            else
            {
                if (client->connection.error)
                {
                    INFO1 ("End of Stream %s", source->mount);
                    source->flags &= ~SOURCE_RUNNING;
                    skip = 0;
                }
                break;
            }
            loop--;
        } while (loop);

        /* lets see if we have too much data in the queue */
        while (source->queue_size > source->queue_size_limit ||
                (source->stream_data && source->stream_data->_count == 1))
        {
            refbuf_t *to_go = source->stream_data;
            source->stream_data = to_go->next;
            source->queue_size -= to_go->len;
            to_go->next = NULL;
            /* mark for delete to tell others holding it and release it ourselves */
            to_go->flags |= SOURCE_BLOCK_RELEASE;
            refbuf_release (to_go);
        }
    } while (0);

    if (skip)
        client->schedule_ms += (source->skip_duration | 0xF);
    else
        client->schedule_ms += 15;
    thread_mutex_unlock (&source->lock);
    return 0;
}


void source_listeners_wakeup (source_t *source)
{
    client_t *s = source->client;
    avl_node *node = avl_get_first (source->clients);
    while (node)
    {
        client_t *client = (client_t *)node->key;
        if (s->schedule_ms + 100 < client->schedule_ms)
            DEBUG2 ("listener on %s was ahead by %ld", source->mount, (long)(client->schedule_ms - s->schedule_ms));
        client->schedule_ms = 0;
        node = avl_get_next (node);
    }
}


static int source_client_read (client_t *client)
{
    source_t *source = client->shared_data;

    if (source == NULL)
    {
        INFO1 ("source client from %s hijacked", client->connection.ip);
        return -1;
    }

    thread_mutex_lock (&source->lock);
    if (client->connection.discon_time &&
            client->connection.discon_time <= client->worker->current_time.tv_sec)
    {
        source->flags &= ~SOURCE_RUNNING;
        INFO1 ("streaming duration expired on %s", source->mount);
    }
    if (source_running (source))
    {
        if (source->limit_rate)
        {
            source->incoming_rate = (long)rate_avg (source->format->in_bitrate);
            if (source->limit_rate < (8 * source->incoming_rate))
            {
                rate_add (source->format->in_bitrate, 0, client->worker->current_time.tv_sec);
                thread_mutex_unlock (&source->lock);
                client->schedule_ms += 110;
                return 0;
            }
        }
        // maybe have source read leave lock if dropping to exit ?
        return source_read (source);
    }
    else
    {
        if ((source->flags & SOURCE_TERMINATING) == 0)
            source_shutdown (source, 1);

        if (source->termination_count && source->termination_count <= source->listeners)
        {
            if (client->timer_start + 1500 < client->worker->time_ms)
            {
                WARN2 ("%ld listeners still to process in terminating %s", source->termination_count, source->mount); 
                source->flags &= ~SOURCE_TERMINATING;
            }
            else
                DEBUG3 ("%s waiting (%lu, %lu)", source->mount, source->termination_count, source->listeners);
            client->schedule_ms = client->worker->time_ms + 100;
        }
        else
        {
            if (source->listeners)
            {
                INFO1 ("listeners on terminating source %s, rechecking", source->mount);
                source->termination_count = source->listeners;
                client->timer_start = client->worker->time_ms;
                source->flags &= ~SOURCE_PAUSE_LISTENERS;
                source->flags |= SOURCE_LISTENERS_SYNC;
                source_listeners_wakeup (source);
                thread_mutex_unlock (&source->lock);
                return 0;
            }
            INFO1 ("no more listeners on %s", source->mount);
            stats_event_args (source->mount, "listeners", "%lu", source->listeners);
            client->connection.discon_time = 0;
            client->ops = &source_client_halt_ops;
            free (source->fallback.mount);
            source->fallback.mount = NULL;
            source->flags &= ~SOURCE_LISTENERS_SYNC;
        }
        thread_mutex_unlock (&source->lock);
    }
    return 0;
}


static int source_queue_advance (client_t *client)
{
    source_t *source = client->shared_data;
    refbuf_t *refbuf;

    if (client->refbuf == NULL && locate_start_on_queue (source, client) < 0)
        return -1;

    refbuf = client->refbuf;

    /* move to the next buffer if we have finished with the current one */
    if (client->pos >= refbuf->len)
    {
        if (refbuf->next == NULL)
        {
            client->schedule_ms = source->client->schedule_ms + 5;
            return -1;
        }
        client_set_queue (client, refbuf->next);
    }
    return source->format->write_buf_to_client (client);
}


static int locate_start_on_queue (source_t *source, client_t *client)
{
    refbuf_t *refbuf;
    long lag = 0;

    /* we only want to attempt a burst at connection time, not midstream
     * however streams like theora may not have the most recent page marked as
     * a starting point, so look for one from the burst point */
    if (client->connection.error || source->stream_data_tail == NULL)
        return -1;
    refbuf = source->stream_data_tail;
    if (client->connection.sent_bytes > source->min_queue_offset && (refbuf->flags & SOURCE_BLOCK_SYNC))
    {
        lag = refbuf->len;
    }
    else
    {
        const char *header = httpp_getvar (client->parser, "initial-burst");
        const char *arg = httpp_get_query_param (client->parser, "burst");
        size_t size = source->min_queue_size;
        off_t v = source->default_burst_size;
        if (arg)
            v = atol (arg);
        else if (header)
            v = atol (header);
        v -= client->connection.sent_bytes; /* have we sent data already */
        refbuf = source->min_queue_point;
        lag = source->min_queue_offset;
        // DEBUG3 ("size %lld, v %lld, lag %ld", size, v, lag);
        while (size > v && refbuf && refbuf->next)
        {
            size -= refbuf->len;
            lag -= refbuf->len;
            refbuf = refbuf->next;
        }
        if (lag < 0)
            ERROR1 ("Odd, lag is negative", lag);
    }

    while (refbuf)
    {
        if (refbuf->flags & SOURCE_BLOCK_SYNC)
        {
            client_set_queue (client, refbuf);
            client->intro_offset = -1;
            client->pos = 0;
            client->queue_pos = source->client->queue_pos - lag;
            return 0;
        }
        lag -= refbuf->len;
        refbuf = refbuf->next;
    }
    client->schedule_ms += 150;
    return -1;
}


static int http_source_introfile (client_t *client)
{
    source_t *source = client->shared_data;

    //DEBUG2 ("client intro_pos is %ld, sent bytes is %ld", client->intro_offset, client->connection.sent_bytes);
    if (format_file_read (client, source->format, source->intro_file) < 0)
    {
        if (source->stream_data_tail)
        {
            /* better find the right place in queue for this client */
            client_set_queue (client, NULL);
            client->check_buffer = source_queue_advance;
            return source_queue_advance (client);
        }
        client->schedule_ms += 100;
        client->intro_offset = 0;  /* replay intro file */
        return -1;
    }
    return source->format->write_buf_to_client (client);
}


static int http_source_intro (client_t *client)
{
    /* we only need to send the intro if nothing else has been sent */
    if (client->connection.sent_bytes > 0)
    {
        client_set_queue (client, NULL);
        client->check_buffer = source_queue_advance;
        return source_queue_advance (client);
    }
    client->intro_offset = 0;
    client->check_buffer = http_source_introfile;
    return http_source_introfile (client);
}


static int http_source_listener (client_t *client)
{
    refbuf_t *refbuf = client->refbuf;
    source_t *source = client->shared_data;
    int ret;

    if (refbuf == NULL || client->respcode)
    {
        client->check_buffer = http_source_intro;
        return http_source_intro (client);
    }
    if (source->queue_size == 0)
    {
        client->schedule_ms += 500;
        return -1;  /* postpone processing until data on queue */
    }

    if (client->respcode == 0)
    {
        int (*build_headers)(format_plugin_t *, client_t *) = format_general_headers;

        if (source_running (source) == 0)
        {
            client->schedule_ms += 200;
            return -1;
        }
        if (source->format->create_client_data)
            build_headers = source->format->create_client_data;

        refbuf->len = 0;
        if (build_headers (source->format, client) < 0)
        {
            ERROR0 ("internal problem, dropping client");
            return -1;
        }
        stats_event_inc (source->mount, "listener_connections");
    }
    ret = format_generic_write_to_client (client);
    if (client->pos == refbuf->len)
    {
        client->check_buffer = http_source_intro;
        client->intro_offset = 0;
        if (client->flags & CLIENT_HAS_INTRO_CONTENT)
        {
            client->refbuf = refbuf->next;
            refbuf->next = NULL;
            refbuf_release (refbuf);
            if (client->refbuf == NULL)
                client->flags &= ~CLIENT_HAS_INTRO_CONTENT;
            client->pos = 0;
        }
        else
            client_set_queue (client, NULL);
        client->connection.sent_bytes = 0;
        return ret;
    }
    client->schedule_ms += 200;
    return ret;
}


void source_listener_detach (source_t *source, client_t *client)
{
    if (client->check_buffer != http_source_listener)
    {
        refbuf_t *ref = client->refbuf;

        client->check_buffer = source->format->write_buf_to_client;
        if (ref && client->pos < ref->len && ref->flags&SOURCE_QUEUE_BLOCK)
        {
            /* make a private copy so that a write can complete */
            refbuf_t *copy = refbuf_copy (client->refbuf);

            refbuf_release (client->refbuf);
            client->refbuf = copy;
            client->flags |= CLIENT_HAS_INTRO_CONTENT;
        }
        if ((client->flags & CLIENT_HAS_INTRO_CONTENT) == 0)
            client_set_queue (client, NULL);
    }
    avl_delete (source->clients, client, NULL);
    source->listeners--;
}


/* used to hold listeners in waiting over a relay restart. Handling of a failed relay also
 * needs to occur.
 */
static int wait_for_restart (client_t *client)
{
    source_t *source = client->shared_data;

    if (client->worker->current_time.tv_sec - client->timer_start > 15)
    {
        INFO1 ("Dropping listener, stuck in %s too long", source->mount);
        client->connection.error = 1; // in here too long, drop client
    }

    if (source_running (source) || client->connection.error ||
            (source->flags & SOURCE_PAUSE_LISTENERS) == 0 ||
            (source->flags & (SOURCE_TERMINATING|SOURCE_LISTENERS_SYNC)))
    {
        client->ops = &listener_client_ops;
        return 0;
    }

    if (source->flags & SOURCE_LISTENERS_SYNC)
        client->schedule_ms = client->worker->time_ms + 100;
    else
        client->schedule_ms = client->worker->time_ms + 300;
    return 0;
}


/* used to hold listeners that have already been processed while other listeners
 * are still to be done
 */
static int wait_for_other_listeners (client_t *client)
{
    source_t *source = client->shared_data;

    if ((source->flags & (SOURCE_TERMINATING|SOURCE_LISTENERS_SYNC)) == SOURCE_LISTENERS_SYNC)
    {
        client->schedule_ms = client->worker->time_ms + 150;
        return 0;
    }
    client->ops = &listener_client_ops;
    return 0;
}


/* general send routine per listener.
 */
static int send_to_listener (client_t *client)
{
    source_t *source = client->shared_data;
    int ret;

    if (source == NULL)
        return -1;
    thread_mutex_lock (&source->lock);
    ret = send_listener (source, client);
    if (ret == 1)
        return 1; // client moved, and source unlocked
    if (ret < 0)
        ret = source_listener_release (source, client);
    thread_mutex_unlock (&source->lock);
    return ret;
}


int listener_waiting_on_source (source_t *source, client_t *client)
{
    source->termination_count--;
    //DEBUG2 ("termination count on %s now %lu", source->mount, source->termination_count);
    if (client->connection.error)
        return -1;
    if (source->fallback.mount)
    {
        int move_failed;

        source_listener_detach (source, client);
        thread_mutex_unlock (&source->lock);
        move_failed = move_listener (client, &source->fallback);
        thread_mutex_lock (&source->lock);
        if (move_failed == 0)
            return 0;
        source_setup_listener (source, client);
    }
    if (source->flags & SOURCE_TERMINATING)
    {
        if ((source->flags & SOURCE_PAUSE_LISTENERS) && global.running == ICE_RUNNING)
        {
            if (client->refbuf && (client->refbuf->flags & SOURCE_QUEUE_BLOCK))
                client_set_queue (client, NULL);
            client->ops = &listener_pause_ops;
            client->flags |= CLIENT_HAS_MOVED;
            client->schedule_ms = client->worker->time_ms + 60;
            client->timer_start = client->worker->current_time.tv_sec;
            return 0;
        }
        return -1;
    }
    /* wait for all source listeners to go through this */
    // DEBUG1 ("listener now waiting for the other %d listeners", source->termination_count);
    client->ops = &listener_wait_ops;
    client->schedule_ms = client->worker->time_ms + 100;
    return 0;
}


static int send_listener (source_t *source, client_t *client)
{
    int bytes;
    int loop = 12;   /* max number of iterations in one go */
    long total_written = 0, limiter = source->listener_send_trigger;
    int ret = 0, lag;
    worker_t *worker = client->worker;
    time_t now = worker->current_time.tv_sec;

    if (source->flags & SOURCE_LISTENERS_SYNC)
        return listener_waiting_on_source (source, client);

    if (client->connection.error)
        return -1;

    /* check for limited listener time */
    if (client->connection.discon_time && now >= client->connection.discon_time)
    {
        INFO1 ("time limit reached for client #%lu", client->connection.id);
        return -1;
    }
    if (source_running (source) == 0)
    {
        DEBUG0 ("source not running, listener will wait");
        client->schedule_ms += 100;
        return 0;
    }

    // do we migrate this listener to the same handler as the source client
    if (source->client_stats_update-1 == now && source->client->worker != worker)
        if (listener_change_worker (client, source))
            return 1;

    lag = source->client->queue_pos - client->queue_pos;

    if (source->incoming_rate && lag < source->incoming_rate)
        limiter = source->incoming_rate/2;

    /* progessive slowdown if nearing max bandwidth.  */
    if (global.max_rate)
    {
        if (throttle_sends > 2) /* exceeded limit, skip 30ms */
        {
            client->schedule_ms += 30;
            return 0;
        }
        if (throttle_sends > 1) /* slow down any multiple sends */
        {
            loop = 2;
            client->schedule_ms += 50;
        }
        if (throttle_sends > 0)
        {
            /* make lagging listeners, lag further on high bandwidth use */
            if (lag > (source->incoming_rate*2))
                client->schedule_ms += 150;
        }
    }
    while (1)
    {
        /* jump out if client connection has died */
        if (client->connection.error)
        {
            ret = -1;
            break;
        }
        /* lets not send too much to one client in one go, but don't
           sleep for too long if more data can be sent */
        if (loop == 0 || total_written > limiter)
        {
            client->schedule_ms = client->worker->time_ms + 15;
            break;
        }
        bytes = client->check_buffer (client);
        if (bytes < 0)
            break;  /* can't write any more */

        total_written += bytes;
        loop--;
    }
    rate_add (source->format->out_bitrate, total_written, worker->time_ms);
    global_add_bitrates (global.out_bitrate, total_written, worker->time_ms);
    source->bytes_sent_since_update += total_written;

    /* the refbuf referenced at head (last in queue) may be marked for deletion
     * if so, check to see if this client is still referring to it */
    if (client->refbuf && (client->refbuf->flags & SOURCE_BLOCK_RELEASE))
    {
        INFO3 ("Client %lu (%s) has fallen too far behind on %s, removing",
                client->connection.id, client->connection.ip, source->mount);
        stats_event_inc (source->mount, "slow_listeners");
        client_set_queue (client, NULL);
        ret = -1;
    }
    return ret;
}


/* Perform any initialisation before the stream data is processed, the header
 * info is processed by now and the format details are setup
 */
void source_init (source_t *source)
{
    mount_proxy *mountinfo;
    format_type_t type = source->format->type;

    if (source->dumpfilename != NULL)
    {
        INFO2 ("dumpfile \"%s\" for %s", source->dumpfilename, source->mount);
        source->dumpfile = fopen (source->dumpfilename, "ab");
        if (source->dumpfile == NULL)
        {
            WARN2("Cannot open dump file \"%s\" for appending: %s, disabling.",
                    source->dumpfilename, strerror(errno));
        }
    }

    /* start off the statistics */
    stats_event_inc (NULL, "source_total_connections");
    stats_event_flags (source->mount, "slow_listeners", "0", STATS_COUNTERS);
    stats_event (source->mount, "server_type", source->format->contenttype);
    stats_event_flags (source->mount, "listener_peak", "0", STATS_COUNTERS);
    stats_event_args (source->mount, "listener_peak", "%lu", source->peak_listeners);
    stats_event_flags (source->mount, "listener_connections", "0", STATS_COUNTERS);
    stats_event_time (source->mount, "stream_start", STATS_COUNTERS);
    stats_event_flags (source->mount, "total_mbytes_sent", "0", STATS_COUNTERS);
    stats_event_flags (source->mount, "total_bytes_sent", "0", STATS_COUNTERS);
    stats_event_flags (source->mount, "total_bytes_read", "0", STATS_COUNTERS);
    stats_event_flags (source->mount, "outgoing_kbitrate", "0", STATS_COUNTERS);
    stats_event_flags (source->mount, "incoming_bitrate", "0", STATS_COUNTERS);
    stats_event_flags (source->mount, "queue_size", "0", STATS_COUNTERS);
    stats_event_flags (source->mount, "connected", "0", STATS_COUNTERS);
    stats_event_flags (source->mount, "source_ip", source->client->connection.ip, STATS_COUNTERS);

    source->last_read = time(NULL);
    source->prev_listeners = -1;
    source->bytes_sent_since_update = 0;
    source->stats_interval = 5;
    /* so the first set of average stats after 3 seconds */
    source->client_stats_update = source->last_read + 3;
    source->worker_balance_recheck = source->last_read + 20;
    source->skip_duration = 80;

    util_dict_free (source->audio_info);
    source->audio_info = util_dict_new();
    if (source->client)
    {
        const char *str = httpp_getvar(source->client->parser, "ice-audio-info");
        if (str)
        {
            _parse_audio_info (source, str);
            stats_event_flags (source->mount, "audio_info", str, STATS_GENERAL);
        }
    }
    source->format->in_bitrate = rate_setup (60, 1);
    source->format->out_bitrate = rate_setup (9000, 1000);

    source->flags |= SOURCE_RUNNING;
    thread_mutex_unlock (&source->lock);

    mountinfo = config_find_mount (config_get_config(), source->mount);
    if (mountinfo)
    {
        if (mountinfo->max_stream_duration)
            source->client->connection.discon_time = source->client->worker->current_time.tv_sec + mountinfo->max_stream_duration;
        if (mountinfo->on_connect)
            source_run_script (mountinfo->on_connect, source->mount);
        auth_stream_start (mountinfo, source->mount);

        /*
         ** Now, if we have a fallback source and override is on, we want
         ** to steal its clients, because it means we've come back online
         ** after a failure and they should be gotten back from the waiting
         ** loop or jingle track or whatever the fallback is used for
         */

        if (mountinfo->fallback_override && mountinfo->fallback_mount)
            source_set_override (mountinfo->fallback_mount, source, type);
    }
    config_release_config();

    INFO1 ("Source %s initialised", source->mount);

    /* on demand relays should of already called this */
    if ((source->flags & SOURCE_ON_DEMAND) == 0)
        slave_update_all_mounts();
    source->flags &= ~SOURCE_ON_DEMAND;
}


static int source_set_override (const char *mount, source_t *dest_source, format_type_t type)
{
    source_t *source;
    const char *dest = dest_source->mount;
    int ret = 0;

    avl_tree_rlock (global.source_tree);
    source = source_find_mount (mount);
    if (source)
    {
        if (strcmp (source->mount, dest) != 0)
        {
            thread_mutex_lock (&source->lock);
            if (source->format->type == type)
            {
                if (source->listeners && source->fallback.mount == NULL)
                {
                    source->fallback.limit = 0;
                    source->fallback.mount = strdup (dest);
                    source->fallback.type = type;
                    source->termination_count = source->listeners;
                    source->client->timer_start = dest_source->client->worker->time_ms;
                    source->flags |= SOURCE_LISTENERS_SYNC;
                    source_listeners_wakeup (source);
                    ret = 1;
                }
            }
            else
                ERROR4("%s (%d) and %s(%d) are different formats", mount, type, dest, source->format->type);
            thread_mutex_unlock (&source->lock);
        }
        avl_tree_unlock (global.source_tree);
        if (ret)
            INFO2 ("moving from %s to %s", mount, dest);
    }
    else
    {
        avl_tree_unlock (global.source_tree);
        ret = fserve_set_override (mount, dest, type);
    }
    return ret;
}


void source_set_fallback (source_t *source, const char *dest_mount)
{
    int bitrate = 0;
    client_t *client = source->client;
    time_t connected;

    if (dest_mount == NULL)
    {
        INFO1 ("No fallback on %s", source->mount);
        return;
    }
    if (source->listeners == 0)
    {
        INFO2 ("fallback on %s to %s, but no listeners", source->mount, dest_mount);
        return;
    }

    connected = client->worker->current_time.tv_sec - client->connection.con_time;
    if (connected > 40)
        bitrate = (int)rate_avg (source->format->in_bitrate);
    if (bitrate == 0 && source->limit_rate)
        bitrate = source->limit_rate;

    source->fallback.mount = strdup (dest_mount);
    source->fallback.flags = FS_FALLBACK;
    source->fallback.limit = bitrate;
    source->fallback.type = source->format->type;
    INFO4 ("fallback set on %s to %s(%d) with %d listeners", source->mount, dest_mount,
            source->fallback.limit, source->listeners);
}


void source_shutdown (source_t *source, int with_fallback)
{
    mount_proxy *mountinfo;

    INFO1("Source \"%s\" exiting", source->mount);

    source->flags &= ~(SOURCE_ON_DEMAND|SOURCE_TIMEOUT);
    source->termination_count = source->listeners;
    source->client->timer_start = source->client->worker->time_ms;
    source->flags |= (SOURCE_TERMINATING | SOURCE_LISTENERS_SYNC);
    source_listeners_wakeup (source);
    mountinfo = config_find_mount (config_get_config(), source->mount);
    if (source->client->connection.con_time)
    {
        /* only do these if source has been running */
        update_source_stats (source);
        if (mountinfo)
        {
            if (mountinfo->on_disconnect)
                source_run_script (mountinfo->on_disconnect, source->mount);
            auth_stream_end (mountinfo, source->mount);
        }
    }
    if (mountinfo && with_fallback && global.running == ICE_RUNNING)
        source_set_fallback (source, mountinfo->fallback_mount);
    config_release_config();
}


static void _parse_audio_info (source_t *source, const char *s)
{
    const char *start = s;
    unsigned int len;

    while (start != NULL && *start != '\0')
    {
        if ((s = strchr (start, ';')) == NULL)
            len = strlen (start);
        else
        {
            len = (int)(s - start);
            s++; /* skip passed the ';' */
        }
        if (len)
        {
            char name[100], value[200];
            int n = sscanf (start, "%99[^=]=%199[^;\r\n]", name, value);

            if (n == 2 && (strncmp (name, "ice-", 4) == 0 || strncmp (name, "bitrate=", 7) == 0))
            {
                char *esc = util_url_unescape (value);
                if (esc)
                {
                    util_dict_set (source->audio_info, name, esc);
                    stats_event_flags (source->mount, name, esc, STATS_COUNTERS);
                }
                free (esc);
            }
        }
        start = s;
    }
}


/* Apply the mountinfo details to the source */
static void source_apply_mount (source_t *source, mount_proxy *mountinfo)
{
    const char *str;
    int val;
    http_parser_t *parser = NULL;

    if (mountinfo == NULL || strcmp (mountinfo->mountname, source->mount) == 0)
        INFO1 ("Applying mount information for \"%s\"", source->mount);
    else
        INFO2 ("Applying mount information for \"%s\" from \"%s\"",
                source->mount, mountinfo->mountname);

    stats_set_args (source->stats, "listener_peak", "%lu", source->peak_listeners);

    /* if a setting is available in the mount details then use it, else
     * check the parser details. */

    if (source->client)
        parser = source->client->parser;

    /* to be done before possible non-utf8 stats */
    if (source->format && source->format->apply_settings)
        source->format->apply_settings (source->format, mountinfo);

    /* public */
    if (mountinfo && mountinfo->yp_public >= 0)
        val = mountinfo->yp_public;
    else
    {
        do {
            str = httpp_getvar (parser, "ice-public");
            if (str) break;
            str = httpp_getvar (parser, "icy-pub");
            if (str) break;
            str = httpp_getvar (parser, "x-audiocast-public");
            if (str) break;
            /* handle header from icecast v2 release */
            str = httpp_getvar (parser, "icy-public");
            if (str) break;
            str = source->yp_public > 0 ? "1" : "0";
        } while (0);
        val = atoi (str);
    }
    stats_set_args (source->stats, "public", "%d", val);
    if (source->yp_public != val)
    {
        DEBUG1 ("YP changed to %d", val);
        if (val)
            yp_add (source->mount);
        else
            yp_remove (source->mount);
        source->yp_public = val;
    }

    /* stream name */
    if (mountinfo && mountinfo->stream_name)
        stats_set (source->stats, "server_name", mountinfo->stream_name);
    else
    {
        do {
            str = httpp_getvar (parser, "ice-name");
            if (str) break;
            str = httpp_getvar (parser, "icy-name");
            if (str) break;
            str = httpp_getvar (parser, "x-audiocast-name");
            if (str) break;
            str = "Unspecified name";
        } while (0);
        if (source->format)
            stats_set_conv (source->stats, "server_name", str, source->format->charset);
    }

    /* stream description */
    if (mountinfo && mountinfo->stream_description)
        stats_set (source->stats, "server_description", mountinfo->stream_description);
    else
    {
        do {
            str = httpp_getvar (parser, "ice-description");
            if (str) break;
            str = httpp_getvar (parser, "icy-description");
            if (str) break;
            str = httpp_getvar (parser, "x-audiocast-description");
            if (str) break;
        } while (0);
        if (str && source->format)
            stats_set_conv (source->stats, "server_description", str, source->format->charset);
    }

    /* stream URL */
    if (mountinfo && mountinfo->stream_url)
        stats_set (source->stats, "server_url", mountinfo->stream_url);
    else
    {
        do {
            str = httpp_getvar (parser, "ice-url");
            if (str) break;
            str = httpp_getvar (parser, "icy-url");
            if (str) break;
            str = httpp_getvar (parser, "x-audiocast-url");
            if (str) break;
        } while (0);
        if (str && source->format)
            stats_set_conv (source->stats, "server_url", str, source->format->charset);
    }

    /* stream genre */
    if (mountinfo && mountinfo->stream_genre)
        stats_set (source->stats, "genre", mountinfo->stream_genre);
    else
    {
        do {
            str = httpp_getvar (parser, "ice-genre");
            if (str) break;
            str = httpp_getvar (parser, "icy-genre");
            if (str) break;
            str = httpp_getvar (parser, "x-audiocast-genre");
            if (str) break;
            str = "various";
        } while (0);
        if (source->format)
            stats_set_conv (source->stats, "genre", str, source->format->charset);
    }

    /* stream bitrate */
    if (mountinfo && mountinfo->bitrate)
    {
        str = mountinfo->bitrate;
        stats_set (source->stats, "bitrate", str);
    }
    else
    {
        do {
            str = httpp_getvar (parser, "ice-bitrate");
            if (str) break;
            str = httpp_getvar (parser, "icy-br");
            if (str) break;
            str = httpp_getvar (parser, "x-audiocast-bitrate");
        } while (0);
        if (str)
            stats_set (source->stats, "bitrate", str);
    }

    /* handle MIME-type */
    if (mountinfo && mountinfo->type)
        stats_set (source->stats, "server_type", mountinfo->type);
    else
        if (source->format && source->format->contenttype)
            stats_set (source->stats, "server_type", source->format->contenttype);

    if (mountinfo && mountinfo->subtype)
        stats_set (source->stats, "subtype", mountinfo->subtype);

    if (mountinfo && mountinfo->auth)
        stats_set (source->stats, "authenticator", mountinfo->auth->type);
    else
        stats_set (source->stats, "authenticator", NULL);

    source->limit_rate = 0;
    if (mountinfo && mountinfo->limit_rate)
        source->limit_rate = mountinfo->limit_rate;

    /* needs a better mechanism, probably via a client_t handle */
    free (source->dumpfilename);
    source->dumpfilename = NULL;
    if (mountinfo && mountinfo->dumpfile)
    {
        time_t now = time(NULL);
        struct tm local;
        char buffer[PATH_MAX];

        localtime_r (&now, &local);
        strftime (buffer, sizeof (buffer), mountinfo->dumpfile, &local);
        source->dumpfilename = strdup (buffer);
    }
    /* handle changes in intro file setting */
    if (source->intro_file)
    {
        fclose (source->intro_file);
        source->intro_file = NULL;
    }
    if (mountinfo && mountinfo->intro_filename)
    {
        ice_config_t *config = config_get_config_unlocked ();
        unsigned int len  = strlen (config->webroot_dir) +
            strlen (mountinfo->intro_filename) + 2;
        char *path = malloc (len);
        if (path)
        {
            FILE *f;
            snprintf (path, len, "%s" PATH_SEPARATOR "%s", config->webroot_dir,
                    mountinfo->intro_filename);

            DEBUG1 ("intro file is %s", mountinfo->intro_filename);
            f = fopen (path, "rb");
            if (f)
                source->intro_file = f;
            else
                WARN2 ("Cannot open intro file \"%s\": %s", path, strerror(errno));
            free (path);
        }
    }
    if (mountinfo && mountinfo->queue_size_limit)
        source->queue_size_limit = mountinfo->queue_size_limit;

    if (mountinfo && mountinfo->source_timeout)
        source->timeout = mountinfo->source_timeout;

    if (mountinfo && mountinfo->burst_size >= 0)
        source->default_burst_size = (unsigned int)mountinfo->burst_size;

    if (mountinfo && mountinfo->min_queue_size >= 0)
        source->min_queue_size = mountinfo->min_queue_size;
    if (source->min_queue_size < source->default_burst_size)
        source->min_queue_size = source->default_burst_size;

    if (source->min_queue_size + 40000 > source->queue_size_limit)
        source->queue_size_limit = source->min_queue_size + 40000;

    source->wait_time = 0;
    if (mountinfo && mountinfo->wait_time)
        source->wait_time = (time_t)mountinfo->wait_time;
}


/* update the specified source with details from the config or mount.
 * mountinfo can be NULL in which case default settings should be taken
 */
void source_update_settings (ice_config_t *config, source_t *source, mount_proxy *mountinfo)
{
    char *listen_url;
    int len;

    /* set global settings first */
    source->queue_size_limit = config->queue_size_limit;
    source->min_queue_size = config->min_queue_size;
    source->timeout = config->source_timeout;
    source->default_burst_size = config->burst_size;
    source->stats = stats_handle (source->mount);

    len = strlen (config->hostname) + strlen(source->mount) + 16;
    listen_url = alloca (len);
    snprintf (listen_url, len, "http://%s:%d%s", config->hostname, config->port, source->mount);
    stats_set_flags (source->stats, "listenurl", listen_url, STATS_COUNTERS);

    source_apply_mount (source, mountinfo);

    if (source->dumpfilename)
        DEBUG1 ("Dumping stream to %s", source->dumpfilename);
    if (source->flags & SOURCE_ON_DEMAND)
    {
        DEBUG0 ("on_demand set");
        stats_set (source->stats, "on_demand", "1");
        stats_set_args (source->stats, "listeners", "%ld", source->listeners);
    }
    else
        stats_set (source->stats, "on_demand", NULL);

    if (mountinfo)
    {
        if (mountinfo->on_connect)
            DEBUG1 ("connect script \"%s\"", mountinfo->on_connect);
        if (mountinfo->on_disconnect)
            DEBUG1 ("disconnect script \"%s\"", mountinfo->on_disconnect);
        if (mountinfo->fallback_when_full)
            DEBUG1 ("fallback_when_full to %u", mountinfo->fallback_when_full);
        DEBUG1 ("max listeners to %d", mountinfo->max_listeners);
        stats_set_args (source->stats, "max_listeners", "%d", mountinfo->max_listeners);
        stats_set_flags (source->stats, "cluster_password", mountinfo->cluster_password, STATS_SLAVE|STATS_HIDDEN);
        if (mountinfo->hidden)
        {
            stats_set_flags (source->stats, NULL, NULL, STATS_HIDDEN);
            DEBUG0 ("hidden from public");
        }
        else
            stats_set_flags (source->stats, NULL, NULL, 0);
    }
    else
    {
        DEBUG0 ("max listeners is not specified");
        stats_set (source->stats, "max_listeners", "unlimited");
        stats_set_flags (source->stats, "cluster_password", NULL, STATS_SLAVE);
        stats_set_flags (source->stats, NULL, NULL, STATS_PUBLIC);
    }
    stats_release (source->stats);
    DEBUG1 ("public set to %d", source->yp_public);
    DEBUG1 ("queue size to %u", source->queue_size_limit);
    DEBUG1 ("min queue size to %u", source->min_queue_size);
    DEBUG1 ("burst size to %u", source->default_burst_size);
    DEBUG1 ("source timeout to %u", source->timeout);
}


static int source_client_callback (client_t *client)
{
    const char *agent;
    source_t *source = client->shared_data;

    if (client->connection.error) /* did http response fail? */
    {
        thread_mutex_unlock (&source->lock);
        global_lock();
        global.sources--;
        global_unlock();
        return -1;
    }

    agent = httpp_getvar (source->client->parser, "user-agent");
    if (agent)
        stats_event_flags (source->mount, "user_agent", agent, STATS_COUNTERS);
    stats_event_inc(NULL, "source_client_connections");
    client_set_queue (client, NULL);

    client->ops = &source_client_ops;
    if (source_running (source))
    {
        thread_mutex_unlock (&source->lock);
        stats_event_inc (NULL, "source_total_connections");
    }
    else
        source_init (source);
    return 0;
}


#ifndef _WIN32
static void source_run_script (char *command, char *mountpoint)
{
    pid_t pid, external_pid;

    /* do a fork twice so that the command has init as parent */
    external_pid = fork();
    switch (external_pid)
    {
        case 0:
            switch (pid = fork ())
            {
                case -1:
                    ERROR2 ("Unable to fork %s (%s)", command, strerror (errno));
                    break;
                case 0:  /* child */
                    DEBUG1 ("Starting command %s", command);
                    execl (command, command, mountpoint, (char *)NULL);
                    ERROR2 ("Unable to run command %s (%s)", command, strerror (errno));
                    exit(0);
                default: /* parent */
                    break;
            }
            exit (0);
        case -1:
            ERROR1 ("Unable to fork %s", strerror (errno));
            break;
        default: /* parent */
            waitpid (external_pid, NULL, 0);
            break;
    }
}
#endif


static int is_mount_template (const char *mount)
{
    if (strchr (mount, '*') || strchr (mount, '?') || strchr (mount, '['))
        return 1;
    return 0;
}


/* rescan the mount list, so that xsl files are updated to show
 * unconnected but active fallback mountpoints
 */
void source_recheck_mounts (int update_all)
{
    ice_config_t *config = config_get_config();
    mount_proxy *mount = config->mounts;

    avl_tree_rlock (global.source_tree);

    stats_clear_virtual_mounts ();

    if (update_all)
    {
        avl_node *node = avl_get_first (global.source_tree);
        while (node)
        {
            source_t *source = (source_t*)node->key;

            if (source_available (source))
                source_update_settings (config, source, config_find_mount (config, source->mount));
            node = avl_get_next (node);
        }
    }

    while (mount)
    {
        source_t *source;

        if (is_mount_template (mount->mountname))
        {
            mount = mount->next;
            continue;
        }
        source = source_find_mount_raw (mount->mountname);
        if (source == NULL || source_available (source) == 0)
        {
            source = source_find_mount (mount->mountname);
            DEBUG2 ("fallback checking %s %p", mount->mountname, source);
            if (source)
            {
                long stats = stats_handle (mount->mountname);
                stats_set_flags (stats, NULL, NULL, mount->hidden?STATS_HIDDEN:0);
                stats_set_args (stats, "listenurl", "http://%s:%d%s",
                        config->hostname, config->port, mount->mountname);
                stats_set (stats, "listeners", "0");
                if (mount->max_listeners < 0)
                    stats_set (stats, "max_listeners", "unlimited");
                else
                    stats_set_args (stats, "max_listeners", "%d", mount->max_listeners);
                stats_release (stats);
            }
        }
        mount = mount->next;
    }
    avl_tree_unlock (global.source_tree);
    config_release_config();
}


/* Check whether this listener is on this source. This is only called when
 * there is auth. This may flag an existing listener to terminate.
 * return 1 if ok to add or 0 to prevent
 */
static int check_duplicate_logins (source_t *source, client_t *client, auth_t *auth)
{
    avl_node *node;

    if (auth == NULL || auth->allow_duplicate_users)
        return 1;

    /* allow multiple authenticated relays */
    if (client->username == NULL || client->flags & CLIENT_IS_SLAVE)
        return 1;

    node = avl_get_first (source->clients);
    while (node)
    {
        client_t *existing_client = (client_t *)node->key;
        if (existing_client->username && 
                strcmp (existing_client->username, client->username) == 0)
        {
            if (auth->drop_existing_listener)
            {
                INFO2 ("Found %s on %s, dropping previous account", existing_client->username, source->mount);
                existing_client->connection.error = 1;
                return 1;
            }
            else
                return 0;
        }
        node = avl_get_next (node);
    }       
    return 1;
}


/* listeners have now detected the source shutting down, now wait for them to 
 * exit the handlers
 */
static int source_client_shutdown (client_t *client)
{
    source_t *source = client->shared_data;
    int ret = -1;

    client->schedule_ms = client->worker->time_ms + 100;
    if (client->connection.discon_time)
    {
        if (client->connection.discon_time >= client->worker->current_time.tv_sec)
            return 0;
        else
            return -1;
    }
    thread_mutex_lock (&source->lock);
    if (source->listeners)
        INFO1 ("remaining listeners to process is %d", source->listeners);
    /* listeners handled now */
    if (source->wait_time)
    {
        /* set a wait time for leaving the source reserved */
        client->connection.discon_time = client->worker->current_time.tv_sec + source->wait_time;
        INFO2 ("keeping %s reserved for %d seconds", source->mount, source->wait_time);
        ret = 0;
    }
    thread_mutex_unlock (&source->lock);
    global_lock();
    global.sources--;
    stats_event_args (NULL, "sources", "%d", global.sources);
    global_unlock();
    return ret;
}


/* clean up what is left from the source. */
void source_client_release (client_t *client)
{
    source_t *source = client->shared_data;

    global_reduce_bitrate_sampling (global.out_bitrate);

    thread_mutex_lock (&source->lock);
    source->flags &= ~(SOURCE_RUNNING|SOURCE_ON_DEMAND);
    client->flags &= ~CLIENT_AUTHENTICATED;
    /* log bytes read in access log */
    if (source->format)
        client->connection.sent_bytes = source->format->read_bytes;

    thread_mutex_unlock (&source->lock);

    source_free_source (source);
    slave_update_all_mounts();
    client_destroy (client);
}


static int source_listener_release (source_t *source, client_t *client)
{
    int ret;
    ice_config_t *config;
    mount_proxy *mountinfo;

    /* search through sources client list to find previous link in list */
    source_listener_detach (source, client);
    client->shared_data = NULL;
    if (source->listeners == 0)
        rate_reduce (source->format->out_bitrate, 1000);

    stats_event_dec (NULL, "listeners");
    /* change of listener numbers, so reduce scope of global sampling */
    global_reduce_bitrate_sampling (global.out_bitrate);

    config = config_get_config ();
    mountinfo = config_find_mount (config, source->mount);

    if (mountinfo && mountinfo->access_log.name)
        logging_access_id (&mountinfo->access_log, client);

    ret = auth_release_listener (client, source->mount, mountinfo);
    config_release_config();
    return ret;
}


int source_add_listener (const char *mount, mount_proxy *mountinfo, client_t *client)
{
    int loop = 10, rate = 0, do_process = 0;
    int within_limits;
    source_t *source;
    mount_proxy *minfo = mountinfo;
    const char *passed_mount = mount;
    ice_config_t *config = config_get_config_unlocked();

    do
    {
        int64_t stream_bitrate = 0;

        do
        {
            if (loop == 0)
            {
                WARN0 ("preventing a fallback loop");
                return client_send_403 (client, "Fallback through too many mountpoints");
            }
            avl_tree_rlock (global.source_tree);
            source = source_find_mount_raw (mount);
            if (source)
            {
                thread_mutex_lock (&source->lock);
                if (source_available (source))
                    break;
                thread_mutex_unlock (&source->lock);
            }
            avl_tree_unlock (global.source_tree);
            if (minfo && minfo->limit_rate)
                rate = minfo->limit_rate;
            if (minfo == NULL || minfo->fallback_mount == NULL)
            {
                if (rate == 0)
                    if (sscanf (mount, "%*[^[][%d]", &rate) == 1)
                        rate = rate * 1000 / 8;
                if (rate)
                {
                    fbinfo f;
                    f.flags = FS_FALLBACK;
                    f.mount = (char *)mount;
                    f.fallback = NULL;
                    f.limit = rate;
                    f.type = FORMAT_TYPE_UNDEFINED;
                    if (move_listener (client, &f) == 0)
                    {
                        /* source dead but fallback to file found */
                        stats_event_inc (NULL, "listeners");
                        stats_event_inc (NULL, "listener_connections");
                        return 0;
                    }
                }
                return -2;
            }
            mount = minfo->fallback_mount;
            minfo = config_find_mount (config_get_config_unlocked(), mount);
            loop--;
        } while (1);

        /* ok, we found a source and it is locked */
        avl_tree_unlock (global.source_tree);

        if (client->flags & CLIENT_IS_SLAVE)
        {
            INFO0 ("client is from a slave, bypassing limits");
            break;
        }
        if (source->format)
        {
            stream_bitrate  = 8 * rate_avg (source->format->in_bitrate);

            if (config->max_bandwidth)
            {
                int64_t global_rate = (int64_t)8 * global_getrate_avg (global.out_bitrate);

                DEBUG1 ("server outgoing bitrate is %" PRId64, global_rate);
                if (global_rate + stream_bitrate > config->max_bandwidth)
                {
                    thread_mutex_unlock (&source->lock);
                    INFO0 ("server-wide outgoing bandwidth limit reached");
                    return client_send_403redirect (client, passed_mount, "server bandwidth reached");
                }
            }
        }

        if (mountinfo == NULL)
            break; /* allow adding listeners, no mount limits imposed */

        if (check_duplicate_logins (source, client, mountinfo->auth) == 0)
        {
            thread_mutex_unlock (&source->lock);
            return client_send_403 (client, "Account already in use");
        }

        /* set a per-mount disconnect time if auth hasn't set one already */
        if (mountinfo->max_listener_duration && client->connection.discon_time == 0)
            client->connection.discon_time = time(NULL) + mountinfo->max_listener_duration;

        INFO3 ("max on %s is %d (cur %lu)", source->mount,
                mountinfo->max_listeners, source->listeners);
        within_limits = 1;
        if (mountinfo->max_bandwidth > -1 && stream_bitrate)
        {
            DEBUG3 ("checking bandwidth limits for %s (%" PRId64 ", %" PRId64 ")",
                    mountinfo->mountname, stream_bitrate, mountinfo->max_bandwidth);
            if ((source->listeners+1) * stream_bitrate > mountinfo->max_bandwidth)
            {
                INFO1 ("bandwidth limit reached on %s", source->mount);
                within_limits = 0;
            }
        }
        if (within_limits)
        {
            if (mountinfo->max_listeners == -1)
                break;

            if (source->listeners < (unsigned long)mountinfo->max_listeners)
                break;
            INFO1 ("max listener count reached on %s", source->mount);
        }
        /* minfo starts off as mountinfo put cascades through fallbacks */
        if (minfo && minfo->fallback_when_full && minfo->fallback_mount)
        {
            thread_mutex_unlock (&source->lock);
            mount = minfo->fallback_mount;
            INFO1 ("stream full trying %s", mount);
            loop--;
            continue;
        }

        /* now we fail the client */
        thread_mutex_unlock (&source->lock);
        return client_send_403redirect (client, passed_mount, "max listeners reached");

    } while (1);
    client->connection.sent_bytes = 0;

    client->refbuf->len = PER_CLIENT_REFBUF_SIZE;
    memset (client->refbuf->data, 0, PER_CLIENT_REFBUF_SIZE);

    source_setup_listener (source, client);
    if ((client->flags & CLIENT_ACTIVE) && (source->flags & SOURCE_RUNNING))
        do_process = 1;
    else
    {
        client->flags |= CLIENT_ACTIVE; // from an auth thread context
        worker_wakeup (client->worker);
    }
    thread_mutex_unlock (&source->lock);
    global_reduce_bitrate_sampling (global.out_bitrate);

    stats_event_inc (NULL, "listeners");
    stats_event_inc (NULL, "listener_connections");

    if (do_process) // send something back quickly
        return client->ops->process (client);
    return 0;
}


/* call with the source lock held, but expect the lock released on exit
 * as the listener may of changed threads and therefore lock needed to be
 * released
 */
void source_setup_listener (source_t *source, client_t *client)
{
    if (source->flags & SOURCE_LISTENERS_SYNC)
        client->ops = &listener_wait_ops;
    else if ((source->flags & (SOURCE_RUNNING|SOURCE_ON_DEMAND)) == SOURCE_ON_DEMAND)
        client->ops = &listener_pause_ops;
    else
        client->ops = &listener_client_ops;
    client->shared_data = source;
    client->queue_pos = 0;
    client->mount = source->mount;
    client->flags &= ~CLIENT_IN_FSERVE;
    client->timer_start = client->worker->current_time.tv_sec;

    client->check_buffer = http_source_listener;
    // add client to the source
    avl_insert (source->clients, client);
    source->listeners++;
    if ((source->flags & (SOURCE_ON_DEMAND|SOURCE_RUNNING)) == SOURCE_ON_DEMAND)
    {
        source->client->schedule_ms = 0;
        client->schedule_ms += 300;
        worker_wakeup (source->client->worker);
        DEBUG0 ("woke up relay");
    }
}


static int source_client_http_send (client_t *client)
{
    refbuf_t *stream;
    source_t *source = client->shared_data;

    if (client->pos < client->refbuf->len)
    {
        int ret = format_generic_write_to_client (client);
        if (ret > 0 && ret < client->refbuf->len)
            return 0; /* trap for short writes */
    }
    stream = client->refbuf->associated;
    client->refbuf->associated = NULL;
    refbuf_release (client->refbuf);
    client->refbuf = stream;
    client->pos = client->intro_offset;
    client->intro_offset = 0;
    thread_mutex_lock (&source->lock);
    return source_client_callback (client);
}


static void source_swap_client (source_t *source, client_t *client)
{
    client_t *old_client = source->client;

    INFO1 ("source %s hijacked by another client, terminating old one", source->mount);
    client->shared_data = source;
    source->client = client;

    old_client->schedule_ms = client->worker->time_ms;
    old_client->shared_data = NULL;
    old_client->flags &= ~CLIENT_AUTHENTICATED;
    old_client->connection.sent_bytes = source->format->read_bytes;

    source->format->read_bytes = 0;
    source->format->parser = source->client->parser;
    if (source->format->swap_client)
        source->format->swap_client (client, old_client);

    worker_wakeup (old_client->worker);
}


int source_startup (client_t *client, const char *uri)
{
    source_t *source;
    source = source_reserve (uri, (client->flags & CLIENT_HIJACKER));

    if (source)
    {
        thread_mutex_lock (&source->lock);

        if ((client->flags & CLIENT_HIJACKER) && source_running (source))
        {
            source_swap_client (source, client);
        }
        else
        {
            ice_config_t *config = config_get_config();
            int source_limit = config->source_limit;

            config_release_config();
            thread_mutex_unlock (&source->lock);
            global_lock();
            if (global.sources >= source_limit)
            {
                WARN1 ("Request to add source when maximum source limit reached %d", global.sources);
                global_unlock();
                client_send_403 (client, "too many streams connected");
                source_free_source (source);
                return 0;
            }
            global.sources++;
            INFO1 ("sources count is now %d", global.sources);
            stats_event_args (NULL, "sources", "%d", global.sources);
            global_unlock();
            thread_mutex_lock (&source->lock);
            source->client = client;
            if (connection_complete_source (source) < 0)
            {
                source->client = NULL;
                client_send_403 (client, "content type not supported");
                thread_mutex_unlock (&source->lock);
                source_free_source (source);
                return 0;
            }
        }
        client->respcode = 200;
        client->shared_data = source;

        if (client->server_conn && client->server_conn->shoutcast_compat)
        {
            source->flags |= SOURCE_SHOUTCAST_COMPAT;
            source_client_callback (client);
        }
        else
        {
            refbuf_t *ok = refbuf_new (PER_CLIENT_REFBUF_SIZE);
            snprintf (ok->data, PER_CLIENT_REFBUF_SIZE,
                    "HTTP/1.0 200 OK\r\n\r\n");
            ok->len = strlen (ok->data);
            /* we may have unprocessed data read in, so don't overwrite it */
            ok->associated = client->refbuf;
            client->refbuf = ok;
            client->intro_offset = client->pos;
            client->pos = 0;
            client->ops = &source_client_http_ops;
            thread_mutex_unlock (&source->lock);
        }
        client->flags |= CLIENT_ACTIVE;
        worker_wakeup (client->worker);
    }
    else
    {
        client_send_403 (client, "Mountpoint in use");
        WARN1 ("Mountpoint %s in use", uri);
    }
    return 0;
}


/* check to see if the source client can be moved to a less busy worker thread.
 * we only move the source client, not the listeners, they will move later
 */
int source_change_worker (source_t *source)
{
    client_t *client = source->client;
    worker_t *this_worker = client->worker, *worker;
    int ret = 0;

    thread_rwlock_rlock (&workers_lock);
    worker = find_least_busy_handler ();
    if (worker && worker != client->worker)
    {
        if (worker->count + source->listeners + 10 < client->worker->count)
        {
            thread_mutex_unlock (&source->lock);
            ret = client_change_worker (client, worker);
            if (ret)
                DEBUG2 ("moving source from %p to %p", this_worker, worker);
            else
                thread_mutex_lock (&source->lock);
        }
    }
    thread_rwlock_unlock (&workers_lock);
    return ret;
}


/* move listener client to worker theread that the source is on. This will
 * help cache but prevent overloading a single worker with many listeners.
 */
int listener_change_worker (client_t *client, source_t *source)
{
    worker_t *this_worker = client->worker, *dest_worker;
    long diff, trigger = source->listeners + 10;
    int ret = 0;

    if (trigger < 1000) trigger = 1000;

    thread_rwlock_rlock (&workers_lock);
    dest_worker = source->client->worker;
    diff = dest_worker->count - this_worker->count;

    if (diff < trigger && this_worker != dest_worker)
    {
        thread_mutex_unlock (&source->lock);
        ret = client_change_worker (client, dest_worker);
        if (ret)
            DEBUG2 ("moving listener from %p to %p", this_worker, dest_worker);
        else
            thread_mutex_lock (&source->lock);
    }
    thread_rwlock_unlock (&workers_lock);
    return ret;
}

