/*
 * Copyright (c) 2010 Sippy Software, Inc., http://www.sippysoft.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id$
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rtpp_log.h"
#include "rtpp_defines.h"
#include "rtpp_session.h"

struct rtpp_timeout_handler {
    char *socket_name;
    int fd;
    int connected;
};

struct rtpp_notify_wi
{
    char *notify_buf;
    int len;
    struct rtpp_timeout_handler *th;
    rtpp_log_t glog;
    struct rtpp_notify_wi *next;
};

static pthread_t rtpp_notify_queue;
static pthread_cond_t rtpp_notify_queue_cond;
static pthread_mutex_t rtpp_notify_queue_mutex;
static pthread_mutex_t rtpp_notify_wi_free_mutex;

static int rtpp_notify_dropped_items;

static struct rtpp_notify_wi *rtpp_notify_wi_free;
static struct rtpp_notify_wi *rtpp_notify_wi_queue, *rtpp_notify_wi_queue_tail;

static struct rtpp_notify_wi *rtpp_notify_queue_get_free_item(void);
static void rtpp_notify_queue_put_item(struct rtpp_notify_wi *);
static void do_timeout_notification(struct rtpp_notify_wi *, int);

struct rtpp_notify_wi *
rtpp_notify_queue_get_free_item(void)
{
    struct rtpp_notify_wi *wi;

    pthread_mutex_lock(&rtpp_notify_wi_free_mutex);
    if (rtpp_notify_wi_free == NULL) {
        /* no free work items, allocate one now */
        wi = malloc(sizeof(*wi));
        if (wi == NULL)
            rtpp_notify_dropped_items++;
        memset(wi, '\0', sizeof(*wi));

        pthread_mutex_unlock(&rtpp_notify_wi_free_mutex);
        return wi;
    }

    wi = rtpp_notify_wi_free;

    /* move up rtpp_notify_wi_free */
    rtpp_notify_wi_free = rtpp_notify_wi_free->next;
    pthread_mutex_unlock(&rtpp_notify_wi_free_mutex);

    return wi;
}

static void
rtpp_notify_queue_return_free_item(struct rtpp_notify_wi *wi)
{

    pthread_mutex_lock(&rtpp_notify_wi_free_mutex);

    wi->next = rtpp_notify_wi_free;
    rtpp_notify_wi_free = wi;

    pthread_mutex_unlock(&rtpp_notify_wi_free_mutex);
}

static void
rtpp_notify_queue_put_item(struct rtpp_notify_wi *wi)
{

    pthread_mutex_lock(&rtpp_notify_queue_mutex);

    wi->next = NULL;
    if (rtpp_notify_wi_queue == NULL) {
        rtpp_notify_wi_queue = wi;
        rtpp_notify_wi_queue_tail = wi;
    } else {
        rtpp_notify_wi_queue_tail->next = wi;
        rtpp_notify_wi_queue_tail = wi;
    }

    /* notify worker thread */
    pthread_cond_signal(&rtpp_notify_queue_cond);

    pthread_mutex_unlock(&rtpp_notify_queue_mutex);
}

static void
rtpp_notify_queue_run(void)
{
    struct rtpp_notify_wi *wi;

    for (;;) {
        pthread_mutex_lock(&rtpp_notify_queue_mutex);
        while (rtpp_notify_wi_queue == NULL) {
            pthread_cond_wait(&rtpp_notify_queue_cond, &rtpp_notify_queue_mutex);
        }
        wi = rtpp_notify_wi_queue;
        rtpp_notify_wi_queue = wi->next;
        pthread_mutex_unlock(&rtpp_notify_queue_mutex);

        /* main work here */
        do_timeout_notification(wi, 1);

        /* put wi into rtpp_notify_wi_free' tail */
        rtpp_notify_queue_return_free_item(wi);
    }
}

int
rtpp_notify_init(void)
{

    rtpp_notify_wi_free = NULL;
    rtpp_notify_wi_queue = NULL;
    rtpp_notify_wi_queue_tail = NULL;

    rtpp_notify_dropped_items = 0;

    pthread_cond_init(&rtpp_notify_queue_cond, NULL);
    pthread_mutex_init(&rtpp_notify_queue_mutex, NULL);
    pthread_mutex_init(&rtpp_notify_wi_free_mutex, NULL);

    if (pthread_create(&rtpp_notify_queue, NULL, (void *(*)(void *))&rtpp_notify_queue_run, NULL) != 0)
        return -1;

    return 0;
}

int
rtpp_notify_schedule(struct cfg *cf, struct rtpp_session *sp)
{
    struct rtpp_notify_wi *wi;
    struct rtpp_timeout_handler *th = sp->timeout_data.handler;
    int len;
    char *notify_buf;

    if (th == NULL) {
        /* Not an error, just nothing to do */
        return 0;
    }

    wi = rtpp_notify_queue_get_free_item();
    if (wi == NULL)
        return -1;

    wi->th = th;
    if (sp->timeout_data.notify_tag == NULL) {
        /* two 5-digit numbers, space, \0 and \n */
        len = 5 + 5 + 3;
    } else {
        /* string, \0 and \n */
        len = strlen(sp->timeout_data.notify_tag) + 2;
    }
    if (wi->notify_buf == NULL) {
        wi->notify_buf = malloc(len);
        if (wi->notify_buf == NULL) {
            rtpp_notify_queue_return_free_item(wi);
            return -1;
        }
    } else {
        notify_buf = realloc(wi->notify_buf, len);
        if (notify_buf == NULL) {
            rtpp_notify_queue_return_free_item(wi);
            return -1;
        }
        wi->notify_buf = notify_buf;
    }
    wi->len = len;

    if (sp->timeout_data.notify_tag == NULL) {
        len = snprintf(wi->notify_buf, len, "%d %d\n",
          sp->ports[0], sp->ports[1]);
    } else {
        len = snprintf(wi->notify_buf, len, "%s\n",
          sp->timeout_data.notify_tag);
    }

    wi->glog = cf->stable.glog;

    rtpp_notify_queue_put_item(wi);
    return 0;
}

static void
reconnect_timeout_handler(rtpp_log_t log, struct rtpp_timeout_handler *th)
{
    struct sockaddr_un remote;

    assert(th->socket_name != NULL && th->connected == 0);

    if (th->fd == -1) {
        rtpp_log_write(RTPP_LOG_DBUG, log, "connecting timeout socket");
    } else {
        rtpp_log_write(RTPP_LOG_DBUG, log, "reconnecting timeout socket");
        close(th->fd);
    }
    th->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (th->fd == -1) {
        rtpp_log_ewrite(RTPP_LOG_ERR, log, "can't create timeout socket");
        return;
    }
    memset(&remote, '\0', sizeof(remote));
    remote.sun_family = AF_LOCAL;
    strncpy(remote.sun_path, th->socket_name, sizeof(remote.sun_path) - 1);
#if defined(HAVE_SOCKADDR_SUN_LEN)
    remote.sun_len = strlen(remote.sun_path);
#endif
    if (connect(th->fd, (struct sockaddr *)&remote, sizeof(remote)) == -1) {
        rtpp_log_ewrite(RTPP_LOG_ERR, log, "can't connect to timeout socket");
    } else {
        th->connected = 1;
    }
}

static void
do_timeout_notification(struct rtpp_notify_wi *wi, int retries)
{
    int result;

    if (wi->th->connected == 0) {
        reconnect_timeout_handler(wi->glog, wi->th);

        /* If connect fails, no notification will be sent */
        if (wi->th->connected == 0) {
            rtpp_log_write(RTPP_LOG_ERR, wi->glog, "unable to send timeout notification");
            return;
        }
    }

    do {
        result = send(wi->th->fd, wi->notify_buf, wi->len - 1, 0);
    } while (result == -1 && errno == EINTR);

    if (result < 0) {
        wi->th->connected = 0;
        rtpp_log_ewrite(RTPP_LOG_ERR, wi->glog, "failed to send timeout notification");
        if (retries > 0)
            do_timeout_notification(wi, retries - 1);
    }
}

struct rtpp_timeout_handler *
rtpp_th_init(char *socket_name, int fd, int connected)
{
    struct rtpp_timeout_handler *th;

    th = malloc(sizeof(struct rtpp_timeout_handler));
    if (th == NULL) {
        return (NULL);
    }
    th->socket_name = socket_name;
    th->fd = fd;
    th->connected = connected;
    return (th);
}

char *
rtpp_th_set_sn(struct rtpp_timeout_handler *th, const char *socket_name)
{
    if (th->socket_name != NULL) {
        free(th->socket_name);
    }
    th->socket_name = strdup(socket_name);
    return (th->socket_name);
}

const char *
rtpp_th_get_sn(struct rtpp_timeout_handler *th)
{

    return (th->socket_name);
}
