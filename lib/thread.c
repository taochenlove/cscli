/* Copyright (C) 2001-2011 IP Infusion, Inc. All Rights Reserved. */

#include <time.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/select.h>
#include <sys/types.h>
#include<sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "thread.h"

/*
   Thread.c maintains a list of all the "callbacks" waiting to run.

   It is RTOS configurable with "HAVE_RTOS_TIC", "HAVE_RTOS_TIMER",
   "RTOS_EXECUTE_ONE_THREAD".

   For Linux/Unix, all the above are undefined; the main task will
   just loop continuously on "thread_fetch" and "thread_call".
   "thread_fetch" will stay in a tight loop until all active THREADS
   are run.

   When "HAVE_RTOS_TIC" is defined, the RTOS is expected to emulate
   the "select" function as non-blocking, and call "lib_tic" for any
   I/O ready (by setting PAL_SOCK_HANDLESET_ISSET true), and must call
   "lib_tic" at least once every 1 second.

   When "HAVE_RTOS_TIMER" is defined, the RTOS must support the
   "rtos_set_timer( )", and set a timer; when it expires, it must call
   "lib_tic ( )".

   When "RTOS_EXECUTE_ONE_THREAD" is defined, the RTOS must treat
   ZebOS as strict "BACKGROUND".  The BACKGROUND MANAGER Keeps track
   of all ZebOS Routers, and calls them in strict sequence.  When
   "lib_tic" is called, ONLY ONE THREAD will be run, and a TRUE state
   will be SET to indicate that a THREAD has RUN, so the BACKGROUND
   should give control to the FOREGROUND once again.  If not SET, the
   BACKGROUND is allowed to go onto another Router to try its THREAD
   and so-forth, through all routers.
*/

#define TV_USEC_PER_SEC 1000000
#define PAL_TIME_MAX_TV_SEC 0x7fffffff
#define PAL_TIME_MAX_TV_USEC 0x7fffffff


static int
timeval_cmp (struct timeval a, struct timeval b)
{
  return (a.tv_sec == b.tv_sec ?
          a.tv_usec - b.tv_usec : a.tv_sec - b.tv_sec);
}

static struct timeval
timeval_adjust (struct timeval a)
{
  while (a.tv_usec >= TV_USEC_PER_SEC)
    {
      a.tv_usec -= TV_USEC_PER_SEC;
      a.tv_sec++;
    }

  while (a.tv_usec < 0)
    {
      a.tv_usec += TV_USEC_PER_SEC;
      a.tv_sec--;
    }

  if (a.tv_sec < 0)
    {
      a.tv_sec = 0;
      a.tv_usec = 10;
    }

  if (a.tv_sec > TV_USEC_PER_SEC)
    a.tv_sec = TV_USEC_PER_SEC;

  return a;
}

struct timeval
timeval_subtract (struct timeval a, struct timeval b)
{
  struct timeval ret;

  ret.tv_usec = a.tv_usec - b.tv_usec;
  ret.tv_sec = a.tv_sec - b.tv_sec;

  return timeval_adjust (ret);
}


/* Allocate new thread master.  */
struct thread_master *
thread_master_create ()
{
  struct thread_master *m;

  m = malloc (sizeof (struct thread_master));
  if (m == NULL)
    return NULL;
  
  return m;
}

/* Add a new thread to the list.  */
void
thread_list_add (struct thread_list *list, struct thread *thread)
{
  thread->next = NULL;
  thread->prev = list->tail;
  if (list->tail)
    list->tail->next = thread;
  else
    list->head = thread;
  list->tail = thread;
  list->count++;
}

/* Add a new thread just before the point.  */
static void
thread_list_add_before(struct thread_list *list,
                       struct thread *point,
                       struct thread *thread)
{
	thread->next = point;
	thread->prev = point->prev;
	if (point->prev)
		point->prev->next = thread;
	else
		list->head = thread;
	point->prev = thread;
	list->count++;
}

/* Delete a thread from the list. */
static struct thread *
thread_list_delete (struct thread_list *list, struct thread *thread)
{
  if (thread->next)
    thread->next->prev = thread->prev;
  else
    list->tail = thread->prev;
  if (thread->prev)
    thread->prev->next = thread->next;
  else
    list->head = thread->next;
  thread->next = thread->prev = NULL;
  list->count--;
  return thread;
}

/* Delete top of the list and return it. */
static struct thread *
thread_trim_head (struct thread_list *list)
{
  if (list->head)
    return thread_list_delete (list, list->head);
  return NULL;
}

/* Move thread to unuse list. */
static void
thread_add_unuse (struct thread_master *m, struct thread *thread)
{
  assert (m != NULL);
  assert (thread->next == NULL);
  assert (thread->prev == NULL);
  assert (thread->type == THREAD_UNUSED);
  thread_list_add (&m->unuse, thread);
}

/* Free all unused thread. */
static void
thread_list_free (struct thread_master *m, struct thread_list *list)
{
  struct thread *t;
  struct thread *next;

  for (t = list->head; t; t = next)
    {
      next = t->next;
      free(t);
      list->count--;
      m->alloc--;
    }
}

void
thread_list_execute (struct thread_master *m, struct thread_list *list)
{
  struct thread *thread;

  thread = thread_trim_head (list);
  if (thread != NULL)
    {
      thread_execute (m, thread->func, thread->arg, thread->u.val);
      thread->type = THREAD_UNUSED;
      thread_add_unuse (m, thread);
    }
}

void
thread_list_clear (struct thread_master *m, struct thread_list *list)
{
  struct thread *thread;

  while ((thread = thread_trim_head (list)))
    {
      thread->type = THREAD_UNUSED;
      thread_add_unuse (m, thread);
    }
}

/* Stop thread scheduler. */
void
thread_master_finish (struct thread_master *m)
{
  thread_list_free (m, &m->queue_high);
  thread_list_free (m, &m->queue_middle);
  thread_list_free (m, &m->queue_low);
  thread_list_free (m, &m->read);
  thread_list_free (m, &m->read_high);
  thread_list_free (m, &m->write);
  
  thread_list_free (m, &m->timer);
  
  thread_list_free (m, &m->event);
  thread_list_free (m, &m->event_low);
  thread_list_free (m, &m->unuse);

  free (m);
}

/* Thread list is empty or not.  */
int
thread_empty (struct thread_list *list)
{
  return  list->head ? 0 : 1;
}

static int
system_uptime (struct timeval *tv, struct timezone *tz)
{
  int ret;
  struct timespec ts;

  ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (ret != 0)
    return -1;

  tv->tv_sec = ts.tv_sec;
  tv->tv_usec = ts.tv_nsec / 1000;

  return 0;
}

/* Return remain time in second. */
unsigned int
thread_timer_remain_second (struct thread *thread)
{
  struct timeval timer_now;

  if (thread == NULL)
    return 0;

  system_uptime (&timer_now, NULL);

  if (thread->u.sands.tv_sec - timer_now.tv_sec > 0)
    return thread->u.sands.tv_sec - timer_now.tv_sec;
  else
    return 0;
}

/* Get new thread.  */
struct thread * thread_get (struct thread_master *m, char type, int (*func) (struct thread *), void *arg)
{
  struct thread *thread;

  if (m->unuse.head)
    thread = thread_trim_head (&m->unuse);
  else
    {
      thread = malloc (sizeof (struct thread));
      if (thread == NULL)
        return NULL;

      m->alloc++;
    }
  thread->type = type;
  thread->master = m;
  thread->func = func;
  thread->arg = arg;

  return thread;
}

/* Keep track of the maximum file descriptor for read/write. */
static void
thread_update_max_fd (struct thread_master *m, int fd)
{
  if (m && m->max_fd < fd)
    m->max_fd = fd;
}

/* Add new read thread. */
struct thread * thread_add_read (struct thread_master *m,
int (*func) (struct thread *), void *arg, int fd)
{
  struct thread *thread;

  assert (m != NULL);

  if (fd < 0)
    return NULL;

  thread = thread_get (m, THREAD_READ, func, arg);
  if (thread == NULL)
    return NULL;

  thread_update_max_fd (m, fd);
  FD_SET(fd, &m->readfd);
  thread->u.fd = fd;
  thread_list_add (&m->read, thread);

  return thread;
}

/* Add new high priority read thread. */
struct thread *
thread_add_read_high (struct thread_master *m,
                 int (*func) (struct thread *), void *arg, int fd)
{
  struct thread *thread;

  assert (m != NULL);

  if (fd < 0)
    return NULL;

  thread = thread_get (m, THREAD_READ_HIGH, func, arg);
  if (thread == NULL)
    return NULL;

  thread_update_max_fd (m, fd);
  FD_SET (fd, &m->readfd);
  thread->u.fd = fd;
  thread_list_add (&m->read_high, thread);

  return thread;
}

/* Add new write thread. */
struct thread *
thread_add_write (struct thread_master *m,
                 int (*func) (struct thread *), void *arg, int fd)
{
  struct thread *thread;

  assert (m != NULL);

  if (fd < 0 || FD_ISSET (fd, &m->writefd))
    return NULL;

  thread = thread_get (m, THREAD_WRITE, func, arg);
  if (thread == NULL)
    return NULL;

  thread_update_max_fd (m, fd);
  FD_SET (fd, &m->writefd);
  thread->u.fd = fd;
  thread_list_add (&m->write, thread);

  return thread;
}

static int
thread_add_timer_common (struct thread_master *m, struct thread *thread)
{
  thread_list_add (&m->timer, thread);

  return 0;
}

/* Add timer event thread. */
struct thread *
thread_add_timer (struct thread_master *m,
                 int (*func) (struct thread *),
                 void *arg, long timer)
{
  int ret;
  struct timeval timer_now;
  struct thread *thread;

  assert (m != NULL);
  thread = thread_get (m, THREAD_TIMER, func, arg);
  if (thread == NULL)
    return NULL;


  system_uptime (&timer_now, NULL);
  timer_now.tv_sec += timer;
  thread->u.sands = timer_now;

  /* Common process.  */
  ret = thread_add_timer_common (m, thread);
  if (ret < 0)
    return NULL;

  return thread;
}



/* Add timer event thread. */
struct thread *
thread_add_timer_timeval (struct thread_master *m,
                          int (*func) (struct thread *), void *arg,
                          struct timeval timer)
{
  int ret;
  struct timeval timer_now;
  struct thread *thread;

  assert (m != NULL);

  thread = thread_get (m, THREAD_TIMER, func, arg);
  if (thread == NULL)
    return NULL;

  /* Do we need jitter here? */
  system_uptime (&timer_now, NULL);
  timer_now.tv_sec += timer.tv_sec;
  timer_now.tv_usec += timer.tv_usec;
  while (timer_now.tv_usec >= TV_USEC_PER_SEC)
    {
      timer_now.tv_sec++;
      timer_now.tv_usec -= TV_USEC_PER_SEC;
    }

  /* Correct negative value.  */
  if (timer_now.tv_sec < 0)
    timer_now.tv_sec = PAL_TIME_MAX_TV_SEC;
  if (timer_now.tv_usec < 0)
    timer_now.tv_usec = PAL_TIME_MAX_TV_USEC;

  thread->u.sands = timer_now;

  /* Common process.  */
  ret = thread_add_timer_common (m, thread);
  if (ret < 0)
    return NULL;

  return thread;
}

/* Add simple event thread. */
struct thread *
thread_add_event (struct thread_master *m,
                  int (*func) (struct thread *), void *arg, int val)
{
  struct thread *thread;

  assert (m != NULL);

  thread = thread_get (m, THREAD_EVENT, func, arg);
  if (thread == NULL)
    return NULL;

  thread->u.val = val;
  thread_list_add (&m->event, thread);

  return thread;
}

/* Add low priority event thread. */
struct thread *
thread_add_event_low (struct thread_master *m,
                      int (*func) (struct thread *), void *arg, int val)
{
  struct thread *thread;

  assert (m != NULL);

  thread = thread_get (m, THREAD_EVENT_LOW, func, arg);
  if (thread == NULL)
    return NULL;

  thread->u.val = val;
  thread_list_add (&m->event_low, thread);

  return thread;
}

/* Add pending read thread. */
struct thread *
thread_add_read_pend (struct thread_master *m,
                      int (*func) (struct thread *), void *arg, int val)
{
  struct thread *thread;

  assert (m != NULL);

  thread = thread_get (m, THREAD_READ_PEND, func, arg);
  if (thread == NULL)
    return NULL;

  thread->u.val = val;
  thread_list_add (&m->read_pend, thread);

  return thread;
}

/* Cancel thread from scheduler. */
void
thread_cancel (struct thread *thread)
{
  switch (thread->type)
    {
    case THREAD_READ:
      FD_CLR (thread->u.fd, &thread->master->readfd);
      thread_list_delete (&thread->master->read, thread);
      break;
    case THREAD_READ_HIGH:
      FD_CLR (thread->u.fd, &thread->master->readfd);
      thread_list_delete (&thread->master->read_high, thread);
      break;
    case THREAD_WRITE:
      assert (FD_ISSET (thread->u.fd, &thread->master->writefd));
      FD_CLR (thread->u.fd, &thread->master->writefd);
      thread_list_delete (&thread->master->write, thread);
      break;
    case THREAD_TIMER:
      thread_list_delete (&thread->master->timer, thread);
      break;
    case THREAD_EVENT:
      thread_list_delete (&thread->master->event, thread);
      break;
    case THREAD_READ_PEND:
      thread_list_delete (&thread->master->read_pend, thread);
      break;
    case THREAD_EVENT_LOW:
      thread_list_delete (&thread->master->event_low, thread);
      break;
    case THREAD_QUEUE:
      switch (thread->priority)
        {
        case THREAD_PRIORITY_HIGH:
          thread_list_delete (&thread->master->queue_high, thread);
          break;
        case THREAD_PRIORITY_MIDDLE:
          thread_list_delete (&thread->master->queue_middle, thread);
          break;
        case THREAD_PRIORITY_LOW:
          thread_list_delete (&thread->master->queue_low, thread);
          break;
        }
      break;
    default:
      break;
    }
  thread->type = THREAD_UNUSED;
  thread_add_unuse (thread->master, thread);
}

/* Delete all events which has argument value arg. */
void
thread_cancel_event (struct thread_master *m, void *arg)
{
  struct thread *thread;
  struct thread *t;

  thread = m->event.head;
  while (thread)
    {
      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->event, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  /* Since Event could have been Queued search queue_high */
  thread = m->queue_high.head;
  while (thread)
    {
      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->queue_high, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  return;
}

/* Delete all low-events which has argument value arg */
void
thread_cancel_event_low (struct thread_master *m, void *arg)
{
  struct thread *thread;
  struct thread *t;

  thread = m->event_low.head;
  while (thread)
    {
      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->event_low, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  /* Since Event could have been Queued search queue_low */
  thread = m->queue_low.head;
  while (thread)
    {
      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->queue_low, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  return;
}

/* Delete all read events which has argument value arg. */
void
thread_cancel_read (struct thread_master *m, void *arg)
{
  struct thread *thread;
  struct thread *t;

  thread = m->read.head;
  while (thread)
    {
      struct thread *t;

      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->read, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  /* Since Event could have been Queued search queue_middle */
  thread = m->queue_middle.head;
  while (thread)
    {
      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->queue_middle, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  return;
}

/* Delete all write events which has argument value arg. */
void
thread_cancel_write (struct thread_master *m, void *arg)
{
  struct thread *thread;
  struct thread *t;

  thread = m->write.head;
  while (thread)
    {
      struct thread *t;

      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->write, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  /* Since Event could have been Queued search queue_middle */
  thread = m->queue_middle.head;
  while (thread)
    {
      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->queue_middle, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  return;
}

/* Delete all timer events which has argument value arg. */
void
thread_cancel_timer (struct thread_master *m, void *arg)
{
  struct thread *thread;
  struct thread *t;
  
  thread = m->timer.head;
  while (thread)
    {
      struct thread *t;

      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->timer, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  /* Since Event could have been Queued search queue_middle */
  thread = m->queue_middle.head;
  while (thread)
    {
      t = thread;
      thread = t->next;

      if (t->arg == arg)
        {
          thread_list_delete (&m->queue_middle, t);
          t->type = THREAD_UNUSED;
          thread_add_unuse (m, t);
        }
    }

  return;
}

/* Pick up smallest timer.  */
struct timeval *
thread_timer_wait (struct thread_master *m, struct timeval *timer_val)
{
  struct timeval timer_now;
  struct timeval timer_min;
  struct timeval *timer_wait;
  struct thread *thread;

  timer_wait = NULL;

  if ((thread = m->timer.head) != NULL)
    {
      if (! timer_wait)
        timer_wait = &thread->u.sands;
      else if (timeval_cmp (thread->u.sands, *timer_wait) < 0)
        timer_wait = &thread->u.sands;
    }

  if (timer_wait)
    {
      timer_min = *timer_wait;

      system_uptime (&timer_now, NULL);
      timer_min = timeval_subtract (timer_min, timer_now);

      if (timer_min.tv_sec < 0)
        {
          timer_min.tv_sec = 0;
          timer_min.tv_usec = 10;
        }

      *timer_val = timer_min;
      return timer_val;
    }
  return NULL;
}

struct thread *
thread_run (struct thread_master *m, struct thread *thread,
            struct thread *fetch)
{
  *fetch = *thread;
  thread->type = THREAD_UNUSED;
  thread_add_unuse (m, thread);
  return fetch;
}

void
thread_enqueue_high (struct thread_master *m, struct thread *thread)
{
  thread->type = THREAD_QUEUE;
  thread->priority = THREAD_PRIORITY_HIGH;
  thread_list_add (&m->queue_high, thread);
}

void
thread_enqueue_middle (struct thread_master *m, struct thread *thread)
{
  thread->type = THREAD_QUEUE;
  thread->priority = THREAD_PRIORITY_MIDDLE;
  thread_list_add (&m->queue_middle, thread);
}

void
thread_enqueue_low (struct thread_master *m, struct thread *thread)
{
  thread->type = THREAD_QUEUE;
  thread->priority = THREAD_PRIORITY_LOW;
  thread_list_add (&m->queue_low, thread);
}

/* When the file is ready move to queueu.  */
int
thread_process_fd (struct thread_master *m, struct thread_list *list,
                   fd_set *fdset, fd_set *mfdset)
{
  struct thread *thread;
  struct thread *next;
  int ready = 0;

  for (thread = list->head; thread; thread = next)
    {
      next = thread->next;

      if (FD_ISSET (THREAD_FD (thread), fdset))
        {
          FD_CLR(THREAD_FD (thread), mfdset);
          thread_list_delete (list, thread);
          thread_enqueue_middle (m, thread);
          ready++;
        }
    }
  return ready;
}

/* Fetch next ready thread. */
struct thread *
thread_fetch (struct thread_master *m, struct thread *fetch)
{
  int num;
  struct thread *thread;
  struct thread *next;
  fd_set readfd;
  fd_set writefd;
  fd_set exceptfd;
  struct timeval timer_now;
  struct timeval timer_val;
  struct timeval *timer_wait;
  struct timeval timer_nowait;

  timer_nowait.tv_sec = 0;
  timer_nowait.tv_usec = 0;


  while (1)
    {
      /* Pending read is exception. */
      if ((thread = thread_trim_head (&m->read_pend)) != NULL)
        return thread_run (m, thread, fetch);

      /* Check ready queue.  */
      if ((thread = thread_trim_head (&m->queue_high)) != NULL)
        return thread_run (m, thread, fetch);

      if ((thread = thread_trim_head (&m->queue_middle)) != NULL)
        return thread_run (m, thread, fetch);

      if ((thread = thread_trim_head (&m->queue_low)) != NULL)
        return thread_run (m, thread, fetch);

      /* Check all of available events.  */

      /* Check events.  */
      while ((thread = thread_trim_head (&m->event)) != NULL)
        thread_enqueue_high (m, thread);

      /* Check timer.  */
      system_uptime (&timer_now, NULL);

      for (thread = m->timer.head; thread; thread = next)
        {
          next = thread->next;
          if (timeval_cmp (timer_now, thread->u.sands) >= 0)
            {
              thread_list_delete (&m->timer, thread);
              thread_enqueue_middle (m, thread);
            }
          else
            break;
        }

      /* Structure copy.  */
      readfd = m->readfd;
      writefd = m->writefd;
      exceptfd = m->exceptfd;

      /* Check any thing to be execute.  */
      if (m->queue_high.head || m->queue_middle.head || m->queue_low.head)
        timer_wait = &timer_nowait;
      else
        timer_wait = thread_timer_wait (m, &timer_val);

      /* First check for sockets.  Return immediately.  */
      num = select (m->max_fd + 1, &readfd, &writefd, &exceptfd,
                             timer_wait);

      /* Error handling.  */
      if (num < 0)
        {
          if (errno == EINTR)
            continue;
        }

      /* File descriptor is readable/writable.  */
      if (num > 0)
        {
          /* High priority read thead. */
          thread_process_fd (m, &m->read_high, &readfd, &m->readfd);

          /* Normal priority read thead. */
          thread_process_fd (m, &m->read, &readfd, &m->readfd);

          /* Write thead. */
          thread_process_fd (m, &m->write, &writefd, &m->writefd);
        }

      /* Low priority events. */
      if ((thread = thread_trim_head (&m->event_low)) != NULL)
        thread_enqueue_low (m, thread);
    }
}

/* Call the thread.  */
void
thread_call (struct thread *thread)
{
  (*thread->func) (thread);
}

/* Fake execution of the thread with given arguemment.  */
struct thread *
thread_execute (struct thread_master *m,
                int (*func)(struct thread *),
                void *arg,
                int val)
{
  struct thread dummy;


  memset (&dummy, 0, sizeof (struct thread));

  dummy.type = THREAD_EVENT;
  dummy.master = NULL;
  dummy.func = func;
  dummy.arg = arg;
  dummy.u.val = val;
  thread_call (&dummy);

  return NULL;
}

