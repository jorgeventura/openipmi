/*
 * posix_os.c
 *
 * POSIX OS-handlers for OpenIPMI
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* Get the rwlocks for GNU. */
#define _GNU_SOURCE

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>

#include <OpenIPMI/os_handler.h>
#include <OpenIPMI/selector.h>
#include <OpenIPMI/ipmi_posix.h>

#include <OpenIPMI/internal/ipmi_int.h>

typedef struct pt_os_hnd_data_s
{
    selector_t       *sel;
    int              wake_sig;
    struct sigaction oldact;
} pt_os_hnd_data_t;


struct os_hnd_fd_id_s
{
    int             fd;
    void            *cb_data;
    os_data_ready_t data_ready;
    os_handler_t    *handler;
    os_fd_data_freed_t freed;
};

static void
fd_handler(int fd, void *data)
{
    os_hnd_fd_id_t *fd_data = (os_hnd_fd_id_t *) data;
    void            *cb_data;
    os_data_ready_t handler;

    handler = fd_data->data_ready;
    cb_data = fd_data->cb_data;
    handler(fd, cb_data, fd_data);
}

static void
free_fd_data(int fd, void *data)
{
    os_hnd_fd_id_t *fd_data = data;

    if (fd_data->freed)
        fd_data->freed(fd, fd_data->cb_data);
    free(data);
}

static int
add_fd(os_handler_t       *handler,
       int                fd,
       os_data_ready_t    data_ready,
       void               *cb_data,
       os_fd_data_freed_t freed,
       os_hnd_fd_id_t     **id)
{
    os_hnd_fd_id_t   *fd_data;
    int              rv;
    pt_os_hnd_data_t *info = handler->internal_data;
    selector_t       *posix_sel = info->sel;

    fd_data = malloc(sizeof(*fd_data));
    if (!fd_data)
	return ENOMEM;

    fd_data->fd = fd;
    fd_data->cb_data = cb_data;
    fd_data->data_ready = data_ready;
    fd_data->handler = handler;
    fd_data->freed = freed;
    sel_set_fd_write_handler(posix_sel, fd, SEL_FD_HANDLER_DISABLED);
    sel_set_fd_except_handler(posix_sel, fd, SEL_FD_HANDLER_DISABLED);
    rv = sel_set_fd_handlers(posix_sel, fd, fd_data, fd_handler, NULL, NULL,
			     free_fd_data);
    if (rv) {
	free(fd_data);
	return rv;
    }
    sel_set_fd_read_handler(posix_sel, fd, SEL_FD_HANDLER_ENABLED);

    *id = fd_data;
    return 0;
}

static int
remove_fd(os_handler_t *handler, os_hnd_fd_id_t *fd_data)
{
    pt_os_hnd_data_t *info = handler->internal_data;
    selector_t       *posix_sel = info->sel;

    sel_set_fd_read_handler(posix_sel, fd_data->fd, SEL_FD_HANDLER_DISABLED);
    sel_clear_fd_handlers(posix_sel, fd_data->fd);
    /* fd_data gets freed in the free_fd_data callback registered at
       set time. */
    return 0;
}

struct os_hnd_timer_id_s
{
    void           *cb_data;
    os_timed_out_t timed_out;
    sel_timer_t    *timer;
    int            running;
    os_handler_t   *handler;
};

static void
timer_handler(selector_t  *sel,
	      sel_timer_t *timer,
	      void        *data)
{
    os_hnd_timer_id_t *timer_data = (os_hnd_timer_id_t *) data;
    /* Make a copy of this, because the handler may delete the timer
       data. */
    void              *cb_data;
    os_timed_out_t    timed_out;

    timed_out = timer_data->timed_out;
    cb_data = timer_data->cb_data;
    timer_data->running = 0;
    timed_out(cb_data, timer_data);
}

static int
start_timer(os_handler_t      *handler, 
	    os_hnd_timer_id_t *id,
	    struct timeval    *timeout,
	    os_timed_out_t    timed_out,
	    void              *cb_data)
{
    struct timeval    now;

    if (id->running)
	return EBUSY;

    id->running = 1;
    id->cb_data = cb_data;
    id->timed_out = timed_out;

    gettimeofday(&now, NULL);
    now.tv_sec += timeout->tv_sec;
    now.tv_usec += timeout->tv_usec;
    while (now.tv_usec >= 1000000) {
	now.tv_usec -= 1000000;
	now.tv_sec += 1;
    }

    return sel_start_timer(id->timer, &now);
}

static int
stop_timer(os_handler_t *handler, os_hnd_timer_id_t *timer_data)
{
    return sel_stop_timer(timer_data->timer);
}

static int
alloc_timer(os_handler_t      *handler, 
	    os_hnd_timer_id_t **id)
{
    os_hnd_timer_id_t *timer_data;
    int               rv;
    pt_os_hnd_data_t  *info = handler->internal_data;
    selector_t        *posix_sel = info->sel;

    timer_data = malloc(sizeof(*timer_data));
    if (!timer_data)
	return ENOMEM;

    timer_data->running = 0;
    timer_data->timed_out = NULL;
    timer_data->handler = handler;

    rv = sel_alloc_timer(posix_sel, timer_handler, timer_data,
			 &(timer_data->timer));
    if (rv) {
	free(timer_data);
	return rv;
    }

    *id = timer_data;
    return 0;
}

static int
free_timer(os_handler_t *handler, os_hnd_timer_id_t *timer_data)
{
    sel_free_timer(timer_data->timer);
    free(timer_data);
    return 0;
}

static int
get_random(os_handler_t *handler, void *data, unsigned int len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    int rv;

    if (fd == -1)
	return errno;

    while (len > 0) {
	rv = read(fd, data, len);
	if (rv < 0) {
	    rv = errno;
	    goto out;
	}
	len -= rv;
    }

    rv = 0;

 out:
    close(fd);
    return rv;
}

static void
sposix_log(os_handler_t         *handler,
	   enum ipmi_log_type_e log_type,
	   char                 *format,
	   ...)
{
    va_list ap;

    va_start(ap, format);
    posix_vlog(format, log_type, ap);
    va_end(ap);
}

static void
sposix_vlog(os_handler_t         *handler,
	    enum ipmi_log_type_e log_type,
	    char                 *format,
	    va_list              ap)
{
    posix_vlog(format, log_type, ap);
}

struct os_hnd_lock_s
{
    pthread_mutex_t mutex;
    int             lock_count;

    /* This is volatile and we always set the owner before we set the count.
       That avoids race conditions checking the count and owner. */
    volatile pthread_t owner;
};

static int
create_lock(os_handler_t  *handler,
	    os_hnd_lock_t **id)
{
    os_hnd_lock_t *lock;
    int           rv;

    lock = malloc(sizeof(*lock));
    if (!lock)
	return ENOMEM;
    rv = pthread_mutex_init(&lock->mutex, NULL);
    if (rv)
	return rv;
    lock->lock_count = 0;
    *id = lock;
    return 0;
}

static int
destroy_lock(os_handler_t  *handler,
	     os_hnd_lock_t *id)
{
    int rv;

    if (id->lock_count != 0)
	ipmi_log(IPMI_LOG_FATAL, "Destroy of lock when count is not zero");
    rv = pthread_mutex_destroy(&id->mutex);
    if (rv)
	return rv;
    free(id);
    return 0;
}

static int
lock(os_handler_t  *handler,
     os_hnd_lock_t *id)
{
    int rv;

    if ((id->lock_count == 0) || (pthread_self() != id->owner)) {
	rv = pthread_mutex_lock(&id->mutex);
	if (rv)
	    return rv;
    }
    id->owner = pthread_self();
    id->lock_count++;
    return 0;
}

static int
unlock(os_handler_t  *handler,
       os_hnd_lock_t *id)
{
    int rv;

    if (id->lock_count == 0)
	ipmi_log(IPMI_LOG_FATAL, "lock count went negative");
    if (pthread_self() != id->owner)
	ipmi_log(IPMI_LOG_FATAL, "lock release by non-owner");
    id->lock_count--;
    if (id->lock_count == 0) {
	rv = pthread_mutex_unlock(&id->mutex);
	if (rv) {
	    id->lock_count++;
	    return rv;
	}
    }
    return 0;
}

struct os_hnd_cond_s
{
    pthread_cond_t cond;
};

static int
create_cond(os_handler_t  *handler,
	    os_hnd_cond_t **new_cond)
{
    os_hnd_cond_t *cond;
    int           rv;

    cond = malloc(sizeof(*cond));
    if (!cond)
	return ENOMEM;

    rv = pthread_cond_init(&cond->cond, NULL);

    if (rv) {
	free(cond);
	return rv;
    }

    *new_cond = cond;
    return 0;
}

static int
destroy_cond(os_handler_t  *handler,
	     os_hnd_cond_t *cond)
{
    int rv;

    rv = pthread_cond_destroy(&cond->cond);
    if (rv)
	return rv;
    free(cond);
    return 0;
}

static int
cond_wait(os_handler_t  *handler,
	  os_hnd_cond_t *cond,
	  os_hnd_lock_t *lock)
{
    return pthread_cond_wait(&cond->cond, &lock->mutex);
}

static int
cond_timedwait(os_handler_t   *handler,
	       os_hnd_cond_t  *cond,
	       os_hnd_lock_t  *lock,
	       struct timeval *timeout)
{
    struct timespec spec;
    struct timeval  now;

    gettimeofday(&now, NULL);
    spec.tv_sec = timeout->tv_sec + now.tv_sec;
    spec.tv_nsec = (timeout->tv_usec + now.tv_usec) * 1000;
    while (spec.tv_nsec > 1000000000) {
	spec.tv_sec += 1;
	spec.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(&cond->cond, &lock->mutex, &spec);
}

static int
cond_wake(os_handler_t  *handler,
	  os_hnd_cond_t *cond)
{
    return pthread_cond_signal(&cond->cond);
}

static int
cond_broadcast(os_handler_t  *handler,
	       os_hnd_cond_t *cond)
{
    return pthread_cond_broadcast(&cond->cond);
}

static int
create_thread(os_handler_t       *handler,
	      int                priority,
	      void               (*startup)(void *data),
	      void               *data)
{
    pthread_attr_t     attr, *pattr = NULL;
    struct sched_param param;
    int                rv;
    pthread_t          tid;

    if (priority) {
	rv = pthread_attr_init(&attr);
	if (rv)
	    return rv;
	rv = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	if (rv)
	    goto out;
	param.sched_priority = priority;
	rv = pthread_attr_setschedparam(&attr, &param);
	if (rv)
	    goto out;
	pattr = &attr;
    }

    rv = pthread_create(&tid, pattr, (void *(*)(void *)) startup, data);

 out:
    if (pattr)
	pthread_attr_destroy(pattr);
    return rv;
}

static int
thread_exit(os_handler_t *handler)
{
    pthread_exit(NULL);
}


void
ipmi_posix_thread_free_os_handler(os_handler_t *os_hnd)
{
    free(os_hnd->internal_data);
    free(os_hnd);
}

void
ipmi_posix_thread_os_handler_set_sel(os_handler_t *os_hnd, selector_t *sel)
{
    pt_os_hnd_data_t  *info = os_hnd->internal_data;

    info->sel = sel;
}

selector_t *
ipmi_posix_thread_os_handler_get_sel(os_handler_t *os_hnd)
{
    pt_os_hnd_data_t  *info = os_hnd->internal_data;

    return info->sel;
}

static void
posix_thread_sighandler(int sig)
{
    /* Nothing to do, sending the sig just wakes up select(). */
}

static void
posix_thread_send_sig(long thread_id, void *cb_data)
{
    pthread_t        *id = (void *) thread_id;
    pt_os_hnd_data_t *info = cb_data;

    pthread_kill(*id, info->wake_sig);
}

static int
perform_one_op(os_handler_t   *os_hnd,
	       struct timeval *timeout)
{
    pthread_t        self = pthread_self();
    pt_os_hnd_data_t *info = os_hnd->internal_data;

    return sel_select(info->sel, posix_thread_send_sig, (long) &self, info,
		      timeout);
}

static void
operation_loop(os_handler_t *os_hnd)
{
    pthread_t        self = pthread_self();
    pt_os_hnd_data_t *info = os_hnd->internal_data;

    sel_select_loop(info->sel, posix_thread_send_sig, (long) &self, info);
}

static void
free_os_handler(os_handler_t *os_hnd)
{
    pt_os_hnd_data_t *info = os_hnd->internal_data;

    sigaction(info->wake_sig, &info->oldact, NULL);
    sel_free_selector(info->sel);
    ipmi_posix_thread_free_os_handler(os_hnd);
}

static void *
posix_malloc(int size)
{
    return malloc(size);
}

static void
posix_free(void *data)
{
    free(data);
}

static os_handler_t ipmi_posix_thread_os_handler =
{
    .mem_alloc = posix_malloc,
    .mem_free = posix_free,
    .add_fd_to_wait_for = add_fd,
    .remove_fd_to_wait_for = remove_fd,
    .start_timer = start_timer,
    .stop_timer = stop_timer,
    .alloc_timer = alloc_timer,
    .free_timer = free_timer,
    .create_lock = create_lock,
    .destroy_lock = destroy_lock,
    .lock = lock,
    .unlock = unlock,
    .get_random = get_random,
    .log = sposix_log,
    .vlog = sposix_vlog,
    .create_cond = create_cond,
    .destroy_cond = destroy_cond,
    .cond_wait = cond_wait,
    .cond_timedwait = cond_timedwait,
    .cond_wake = cond_wake,
    .cond_broadcast = cond_broadcast,
    .create_thread = create_thread,
    .thread_exit = thread_exit,
    .free_os_handler = free_os_handler,
    .perform_one_op = perform_one_op,
    .operation_loop = operation_loop,
};

os_handler_t *
ipmi_posix_thread_get_os_handler(void)
{
    os_handler_t *rv;

    rv = malloc(sizeof(*rv));
    if (!rv)
	return NULL;

    memcpy(rv, &ipmi_posix_thread_os_handler, sizeof(*rv));

    rv->internal_data = malloc(sizeof(pt_os_hnd_data_t));
    if (! rv->internal_data) {
	free(rv);
	rv = NULL;
    }
    return rv;
}

os_handler_t *
ipmi_posix_thread_setup_os_handler(int wake_sig)
{
    os_handler_t     *os_hnd;
    pt_os_hnd_data_t *info;
    struct sigaction act;
    int              rv;

    os_hnd = ipmi_posix_thread_get_os_handler();
    if (!os_hnd)
	return NULL;

    info = os_hnd->internal_data;
    info->wake_sig = wake_sig;

    rv = sel_alloc_selector(os_hnd, &info->sel);
    if (rv) {
	ipmi_posix_thread_free_os_handler(os_hnd);
	os_hnd = NULL;
	goto out;
    }

    act.sa_handler = posix_thread_sighandler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    rv = sigaction(wake_sig, &act, &info->oldact);
    if (rv) {
	ipmi_posix_thread_free_os_handler(os_hnd);
	os_hnd = NULL;
	goto out;
    }

 out:
    return os_hnd;
}

/*
 * Support for the selector code.
 */

int
posix_mutex_alloc(void **val)
{
    void *m = malloc(sizeof(pthread_mutex_t));
    if (!m)
	return ENOMEM;

    *val = m;
    return 0;
}

void posix_mutex_free(void *val)
{
    free(val);
}

void posix_mutex_lock(void *val)
{
    pthread_mutex_lock(val);
}

void posix_mutex_unlock(void *val)
{
    pthread_mutex_unlock(val);
}

/*
 * Cruft below, do not use these any more.
 */
int
ipmi_posix_thread_sel_select(os_handler_t   *os_hnd,
			     struct timeval *timeout)
{
    return perform_one_op(os_hnd, timeout);
}

void
ipmi_posix_thread_sel_select_loop(os_handler_t *os_hnd)
{
    operation_loop(os_hnd);
}

void
ipmi_posix_thread_cleanup_os_handler(os_handler_t *os_hnd)
{
    free_os_handler(os_hnd);
}
