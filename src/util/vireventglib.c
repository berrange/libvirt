/*
 * vireventglib.c: GMainContext based event loop
 *
 * Copyright (C) 2008 Daniel P. Berrange
 * Copyright (C) 2010-2019 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vireventglib.h"
#include "virerror.h"
#include "virlog.h"

#ifdef G_OS_WIN32
# include <io.h>
#endif

#define VIR_FROM_THIS VIR_FROM_EVENT

VIR_LOG_INIT("util.eventglib");

struct virEventGLibHandle
{
    int watch;
    int fd;
    int events;
    int removed;
    GIOChannel *channel;
    guint source;
    virEventHandleCallback cb;
    void *opaque;
    virFreeCallback ff;
};

struct virEventGLibTimeout
{
    int timer;
    int interval;
    int removed;
    guint source;
    virEventTimeoutCallback cb;
    void *opaque;
    virFreeCallback ff;
};

static GMutex *eventlock;

static int nextwatch = 1;
static GPtrArray *handles;

static int nexttimer = 1;
static GPtrArray *timeouts;

static gboolean
virEventGLibHandleDispatch(GIOChannel *source G_GNUC_UNUSED,
                           GIOCondition condition,
                           gpointer opaque)
{
    struct virEventGLibHandle *data = opaque;
    int events = 0;

    if (condition & G_IO_IN)
        events |= VIR_EVENT_HANDLE_READABLE;
    if (condition & G_IO_OUT)
        events |= VIR_EVENT_HANDLE_WRITABLE;
    if (condition & G_IO_HUP)
        events |= VIR_EVENT_HANDLE_HANGUP;
    if (condition & G_IO_ERR)
        events |= VIR_EVENT_HANDLE_ERROR;

    VIR_DEBUG("Dispatch handler %p %d %d %d %p", data, data->watch, data->fd, events, data->opaque);

    (data->cb)(data->watch, data->fd, events, data->opaque);

    return TRUE;
}


static int
virEventGLibHandleAdd(int fd,
                      int events,
                      virEventHandleCallback cb,
                      void *opaque,
                      virFreeCallback ff)
{
    struct virEventGLibHandle *data;
    GIOCondition cond = 0;
    int ret;

    g_mutex_lock(eventlock);

    data = g_new0(struct virEventGLibHandle, 1);

    if (events & VIR_EVENT_HANDLE_READABLE)
        cond |= G_IO_IN;
    if (events & VIR_EVENT_HANDLE_WRITABLE)
        cond |= G_IO_OUT;

    data->watch = nextwatch++;
    data->fd = fd;
    data->events = events;
    data->cb = cb;
    data->opaque = opaque;
#ifdef G_OS_WIN32
    data->channel = g_io_channel_win32_new_socket(_get_osfhandle(fd));
#else
    data->channel = g_io_channel_unix_new(fd);
#endif
    data->ff = ff;

    VIR_DEBUG("Add handle %p %d %d %d %p", data, data->watch, data->fd, events, data->opaque);

    if (events != 0) {
        data->source = g_io_add_watch(data->channel,
                                      cond,
                                      virEventGLibHandleDispatch,
                                      data);
    }

    g_ptr_array_add(handles, data);

    ret = data->watch;

    g_mutex_unlock(eventlock);

    return ret;
}

static struct virEventGLibHandle *
virEventGLibHandleFind(int watch)
{
    guint i;

    for (i = 0; i < handles->len; i++) {
        struct virEventGLibHandle *h = g_ptr_array_index(handles, i);

        if (h == NULL) {
            g_warn_if_reached();
            continue;
        }

        if ((h->watch == watch) && !h->removed)
            return h;
    }

    return NULL;
}

static void
virEventGLibHandleUpdate(int watch,
                         int events)
{
    struct virEventGLibHandle *data;

    g_mutex_lock(eventlock);

    data = virEventGLibHandleFind(watch);
    if (!data) {
        VIR_DEBUG("Update for missing handle watch %d", watch);
        goto cleanup;
    }

    VIR_DEBUG("Update handle %p %d %d %d", data, watch, data->fd, events);

    if (events) {
        GIOCondition cond = 0;
        if (events == data->events)
            goto cleanup;

        if (data->source)
            g_source_remove(data->source);

        cond |= G_IO_HUP;
        if (events & VIR_EVENT_HANDLE_READABLE)
            cond |= G_IO_IN;
        if (events & VIR_EVENT_HANDLE_WRITABLE)
            cond |= G_IO_OUT;
        data->source = g_io_add_watch(data->channel,
                                      cond,
                                      virEventGLibHandleDispatch,
                                      data);
        data->events = events;
    } else {
        if (!data->source)
            goto cleanup;

        g_source_remove(data->source);
        data->source = 0;
        data->events = 0;
    }

 cleanup:
    g_mutex_unlock(eventlock);
}

static gboolean
virEventGLibHandleRemoveIdle(gpointer data)
{
    struct virEventGLibHandle *h = data;

    if (h->ff)
        (h->ff)(h->opaque);

    g_mutex_lock(eventlock);
    g_ptr_array_remove_fast(handles, h);
    g_mutex_unlock(eventlock);

    return FALSE;
}

static int
virEventGLibHandleRemove(int watch)
{
    struct virEventGLibHandle *data;
    int ret = -1;

    g_mutex_lock(eventlock);

    data = virEventGLibHandleFind(watch);
    if (!data) {
        VIR_DEBUG("Remove of missing watch %d", watch);
        goto cleanup;
    }

    VIR_DEBUG("Remove handle %p %d %d", data, watch, data->fd);

    if (data->source != 0) {
        g_source_remove(data->source);
        data->source = 0;
        data->events = 0;
    }

    g_warn_if_fail(data->channel != NULL);
    g_io_channel_unref(data->channel);
    data->channel = NULL;

    /* since the actual watch deletion is done asynchronously, a handleUpdate call may
     * reschedule the watch before it's fully deleted, that's why we need to mark it as
     * 'removed' to prevent reuse
     */
    data->removed = TRUE;
    g_idle_add(virEventGLibHandleRemoveIdle, data);

    ret = 0;

 cleanup:
    g_mutex_unlock(eventlock);
    return ret;
}


static gboolean
virEventGLibTimeoutDispatch(void *opaque)
{
    struct virEventGLibTimeout *data = opaque;
    VIR_DEBUG("Dispatch timeout %p %p %d %p", data, data->cb, data->timer, data->opaque);
    (data->cb)(data->timer, data->opaque);

    return TRUE;
}

static int
virEventGLibTimeoutAdd(int interval,
                       virEventTimeoutCallback cb,
                       void *opaque,
                       virFreeCallback ff)
{
    struct virEventGLibTimeout *data;
    int ret;

    g_mutex_lock(eventlock);

    data = g_new0(struct virEventGLibTimeout, 1);
    data->timer = nexttimer++;
    data->interval = interval;
    data->cb = cb;
    data->opaque = opaque;
    data->ff = ff;
    if (interval >= 0)
        data->source = g_timeout_add(interval,
                                     virEventGLibTimeoutDispatch,
                                     data);

    g_ptr_array_add(timeouts, data);

    VIR_DEBUG("Add timeout %p %d %p %p %d", data, interval, cb, opaque, data->timer);

    ret = data->timer;

    g_mutex_unlock(eventlock);

    return ret;
}


static struct virEventGLibTimeout *
virEventGLibTimeoutFind(int timer)
{
    guint i;

    g_return_val_if_fail(timeouts != NULL, NULL);

    for (i = 0; i < timeouts->len; i++) {
        struct virEventGLibTimeout *t = g_ptr_array_index(timeouts, i);

        if (t == NULL) {
            g_warn_if_reached();
            continue;
        }

        if ((t->timer == timer) && !t->removed)
            return t;
    }

    return NULL;
}


static void
virEventGLibTimeoutUpdate(int timer,
                          int interval)
{
    struct virEventGLibTimeout *data;

    g_mutex_lock(eventlock);

    data = virEventGLibTimeoutFind(timer);
    if (!data) {
        VIR_DEBUG("Update of missing timer %d", timer);
        goto cleanup;
    }

    VIR_DEBUG("Update timeout %p %d %d", data, timer, interval);

    if (interval >= 0) {
        if (data->source)
            g_source_remove(data->source);

        data->interval = interval;
        data->source = g_timeout_add(data->interval,
                                     virEventGLibTimeoutDispatch,
                                     data);
    } else {
        if (!data->source)
            goto cleanup;

        g_source_remove(data->source);
        data->source = 0;
    }

 cleanup:
    g_mutex_unlock(eventlock);
}

static gboolean
virEventGLibTimeoutRemoveIdle(gpointer data)
{
    struct virEventGLibTimeout *t = data;

    if (t->ff)
        (t->ff)(t->opaque);

    g_mutex_lock(eventlock);
    g_ptr_array_remove_fast(timeouts, t);
    g_mutex_unlock(eventlock);

    return FALSE;
}

static int
virEventGLibTimeoutRemove(int timer)
{
    struct virEventGLibTimeout *data;
    int ret = -1;

    g_mutex_lock(eventlock);

    data = virEventGLibTimeoutFind(timer);
    if (!data) {
        VIR_DEBUG("Remove of missing timer %d", timer);
        goto cleanup;
    }

    VIR_DEBUG("Remove timeout %p %d", data, timer);

    if (data->source != 0) {
        g_source_remove(data->source);
        data->source = 0;
    }

    /* since the actual timeout deletion is done asynchronously, a timeoutUpdate call may
     * reschedule the timeout before it's fully deleted, that's why we need to mark it as
     * 'removed' to prevent reuse
     */
    data->removed = TRUE;
    g_idle_add(virEventGLibTimeoutRemoveIdle, data);

    ret = 0;

 cleanup:
    g_mutex_unlock(eventlock);
    return ret;
}


static gpointer virEventGLibRegisterOnce(gpointer data G_GNUC_UNUSED)
{
    eventlock = g_new0(GMutex, 1);
    timeouts = g_ptr_array_new_with_free_func(g_free);
    handles = g_ptr_array_new_with_free_func(g_free);
    virEventRegisterImpl(virEventGLibHandleAdd,
                         virEventGLibHandleUpdate,
                         virEventGLibHandleRemove,
                         virEventGLibTimeoutAdd,
                         virEventGLibTimeoutUpdate,
                         virEventGLibTimeoutRemove);
    return NULL;
}


void virEventGLibRegister(void)
{
    static GOnce once = G_ONCE_INIT;

    g_once(&once, virEventGLibRegisterOnce, NULL);
}


int virEventGLibRunOnce(void)
{
    GMainContext *ctx = g_main_context_default();

    if (!g_main_context_acquire(ctx)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Another thread has acquired the main loop context"));
        return -1;
    }
    g_main_context_iteration(ctx, TRUE);

    g_main_context_release(ctx);

    return 0;
}
