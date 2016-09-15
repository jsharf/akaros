/* Copyright (c) 2015 Google Inc.
 * Barret Rhoden <brho@cs.berkeley.edu>
 * See LICENSE for details.
 *
 * Epoll, built on FD taps, CEQs, and blocking uthreads on event queues.
 *
 * TODO: There are a few incompatibilities with Linux's epoll, some of which are
 * artifacts of the implementation, and other issues:
 * 	- you can't epoll on an epoll fd (or any user fd).  you can only epoll on a
 * 	kernel FD that accepts your FD taps.
 * 	- there's no EPOLLONESHOT or level-triggered support.
 * 	- you can only tap one FD at a time, so you can't add the same FD to
 * 	multiple epoll sets.
 * 	- there is no support for growing the epoll set.
 * 	- closing the epoll is a little dangerous, if there are outstanding INDIR
 * 	events.  this will only pop up if you're yielding cores, maybe getting
 * 	preempted, and are unlucky.
 * 	- epoll_create1 does not support CLOEXEC.  That'd need some work in glibc's
 * 	exec and flags in struct user_fd.
 * 	- EPOLL_CTL_MOD is just a DEL then an ADD.  There might be races associated
 * 	with that.
 * 	- epoll_pwait is probably racy.
 * 	- You can't dup an epoll fd (same as other user FDs).
 * 	- If you add a BSD socket FD to an epoll set, you'll get taps on both the
 * 	data FD and the listen FD.
 * 	- If you add the same BSD socket listener to multiple epoll sets, you will
 * 	likely fail.  This is in addition to being able to tap only one FD at a
 * 	time.
 * */

#include <sys/epoll.h>
#include <parlib/parlib.h>
#include <parlib/event.h>
#include <parlib/ceq.h>
#include <parlib/uthread.h>
#include <parlib/timing.h>
#include <sys/user_fd.h>
#include <sys/close_cb.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/queue.h>
#include <sys/plan9_helpers.h>

/* Sanity check, so we can ID our own FDs */
#define EPOLL_UFD_MAGIC 		0xe9011

struct epoll_ctlr {
	TAILQ_ENTRY(epoll_ctlr)		link;
	struct event_queue			*ceq_evq;
	struct ceq					*ceq;	/* convenience pointer */
	unsigned int				size;
	uth_mutex_t					mtx;
	struct user_fd				ufd;
};

TAILQ_HEAD(epoll_ctlrs, epoll_ctlr);
static struct epoll_ctlrs all_ctlrs = TAILQ_HEAD_INITIALIZER(all_ctlrs);
static uth_mutex_t ctlrs_mtx;

/* There's some bookkeeping we need to maintain on every FD.  Right now, the FD
 * is the index into the CEQ event array, so we can just hook this into the user
 * data blob in the ceq_event.
 *
 * If we ever do not maintain a 1:1 mapping from FDs to CEQ IDs, we can use this
 * to track the CEQ ID and FD. */
struct ep_fd_data {
	struct epoll_event			ep_event;
	int							fd;
	int							filter;
};

/* Converts epoll events to FD taps. */
static int ep_events_to_taps(uint32_t ep_ev)
{
	int taps = 0;
	if (ep_ev & EPOLLIN)
		taps |= FDTAP_FILT_READABLE;
	if (ep_ev & EPOLLOUT)
		taps |= FDTAP_FILT_WRITABLE;
	if (ep_ev & EPOLLRDHUP)
		taps |= FDTAP_FILT_RDHUP;
	if (ep_ev & EPOLLPRI)
		taps |= FDTAP_FILT_PRIORITY;
	if (ep_ev & EPOLLERR)
		taps |= FDTAP_FILT_ERROR;
	if (ep_ev & EPOLLHUP)
		taps |= FDTAP_FILT_HANGUP;
	return taps;
}

/* Converts corresponding FD Taps to epoll events.  There are other taps that do
 * not make sense for epoll. */
static uint32_t taps_to_ep_events(int taps)
{
	uint32_t ep_ev = 0;
	if (taps & FDTAP_FILT_READABLE)
		ep_ev |= EPOLLIN;
	if (taps & FDTAP_FILT_WRITABLE)
		ep_ev |= EPOLLOUT;
	if (taps & FDTAP_FILT_RDHUP)
		ep_ev |= EPOLLRDHUP;
	if (taps & FDTAP_FILT_PRIORITY)
		ep_ev |= EPOLLPRI;
	if (taps & FDTAP_FILT_ERROR)
		ep_ev |= EPOLLERR;
	if (taps & FDTAP_FILT_HANGUP)
		ep_ev |= EPOLLHUP;
	return ep_ev;
}

static struct ceq_event *ep_get_ceq_ev(struct epoll_ctlr *ep, size_t idx)
{
	if (ep->ceq_evq->ev_mbox->ceq.nr_events <= idx)
		return 0;
	return &ep->ceq_evq->ev_mbox->ceq.events[idx];
}

static struct epoll_ctlr *fd_to_cltr(int fd)
{
	struct user_fd *ufd = ufd_lookup(fd);
	if (!ufd)
		return 0;
	if (ufd->magic != EPOLL_UFD_MAGIC) {
		errno = EBADF;
		return 0;
	}
	return container_of(ufd, struct epoll_ctlr, ufd);
}

/* Event queue helpers: */
static struct event_queue *ep_get_ceq_evq(unsigned int ceq_size)
{
	struct event_queue *ceq_evq = get_eventq_raw();
	ceq_evq->ev_mbox->type = EV_MBOX_CEQ;
	ceq_init(&ceq_evq->ev_mbox->ceq, CEQ_OR, ceq_size, ceq_size);
	ceq_evq->ev_flags = EVENT_INDIR | EVENT_SPAM_INDIR | EVENT_WAKEUP;
	evq_attach_wakeup_ctlr(ceq_evq);
	return ceq_evq;
}

static struct event_queue *ep_get_alarm_evq(void)
{
	/* Don't care about the actual message, just using it for a wakeup */
	struct event_queue *alarm_evq = get_eventq(EV_MBOX_BITMAP);
	alarm_evq->ev_flags = EVENT_INDIR | EVENT_SPAM_INDIR | EVENT_WAKEUP;
	evq_attach_wakeup_ctlr(alarm_evq);
	return alarm_evq;
}

/* Once we've closed our sources of events, we can try to clean up the event
 * queues.  These are actually dangerous, since there could be INDIRs floating
 * around for these evqs still, which are basically pointers.  We'll need to run
 * some sort of user deferred destruction. (TODO). */
static void ep_put_ceq_evq(struct event_queue *ceq_evq)
{
#if 0 /* TODO: EVQ/INDIR Cleanup */
	ceq_cleanup(&ceq_evq->ev_mbox->ceq);
	evq_remove_wakeup_ctlr(ceq_evq);
	put_eventq_raw(ceq_evq);
#endif
}

static void ep_put_alarm_evq(struct event_queue *alarm_evq)
{
#if 0 /* TODO: EVQ/INDIR Cleanup */
	evq_remove_wakeup_ctlr(alarm_evq);
	put_eventq(alarm_evq);
#endif
}

static void epoll_close(struct user_fd *ufd)
{
	struct epoll_ctlr *ep = container_of(ufd, struct epoll_ctlr, ufd);
	struct fd_tap_req *tap_reqs, *tap_req_i;
	struct ceq_event *ceq_ev_i;
	struct ep_fd_data *ep_fd_i;
	int nr_tap_req = 0;
	int nr_done = 0;

	tap_reqs = malloc(sizeof(struct fd_tap_req) * ep->size);
	memset(tap_reqs, 0, sizeof(struct fd_tap_req) * ep->size);
	/* Slightly painful, O(n) with no escape hatch */
	for (int i = 0; i < ep->size; i++) {
		ceq_ev_i = ep_get_ceq_ev(ep, i);
		/* CEQ should have been big enough for our size */
		assert(ceq_ev_i);
		ep_fd_i = (struct ep_fd_data*)ceq_ev_i->user_data;
		if (!ep_fd_i)
			continue;
		tap_req_i = &tap_reqs[nr_tap_req++];
		tap_req_i->fd = i;
		tap_req_i->cmd = FDTAP_CMD_REM;
		free(ep_fd_i);
	}
	/* Requests could fail if the tapped files are already closed.  We need to
	 * skip the failed one (the +1) and untap the rest. */
	do {
		nr_done += sys_tap_fds(tap_reqs + nr_done, nr_tap_req - nr_done);
		nr_done += 1;	/* nr_done could be more than nr_tap_req now */
	} while (nr_done < nr_tap_req);
	free(tap_reqs);
	ep_put_ceq_evq(ep->ceq_evq);
	uth_mutex_lock(ctlrs_mtx);
	TAILQ_REMOVE(&all_ctlrs, ep, link);
	uth_mutex_unlock(ctlrs_mtx);
	uth_mutex_free(ep->mtx);
	free(ep);
}

static int init_ep_ctlr(struct epoll_ctlr *ep, int size)
{
	unsigned int ceq_size;

	/* TODO: we don't grow yet.  Until then, we help out a little. */
	if (size == 1)
		size = 128;
	ceq_size = ROUNDUPPWR2(size);
	ep->size = ceq_size;
	ep->mtx = uth_mutex_alloc();
	ep->ufd.magic = EPOLL_UFD_MAGIC;
	ep->ufd.close = epoll_close;
	ep->ceq_evq = ep_get_ceq_evq(ceq_size);
	return 0;
}

static void epoll_fd_closed(int fd)
{
	struct epoll_ctlr *ep;

	/* Lockless peek, avoid locking for every close() */
	if (TAILQ_EMPTY(&all_ctlrs))
		return;
	uth_mutex_lock(ctlrs_mtx);
	TAILQ_FOREACH(ep, &all_ctlrs, link)
		epoll_ctl(ep->ufd.fd, EPOLL_CTL_DEL, fd, 0);
	uth_mutex_unlock(ctlrs_mtx);
}

static void epoll_init(void)
{
	static struct close_cb epoll_close_cb = {.func = epoll_fd_closed};

	register_close_cb(&epoll_close_cb);
	ctlrs_mtx = uth_mutex_alloc();
}

int epoll_create(int size)
{
	int fd;
	struct epoll_ctlr *ep;

	run_once(epoll_init());
	/* good thing the arg is a signed int... */
	if (size < 0) {
		errno = EINVAL;
		return -1;
	}
	ep = malloc(sizeof(struct epoll_ctlr));
	memset(ep, 0, sizeof(struct epoll_ctlr));
	if (init_ep_ctlr(ep, size)) {
		free(ep);
		return -1;
	}
	fd = ufd_get_fd(&ep->ufd);
	if (fd < 0)
		free(ep);
	uth_mutex_lock(ctlrs_mtx);
	TAILQ_INSERT_TAIL(&all_ctlrs, ep, link);
	uth_mutex_unlock(ctlrs_mtx);
	return fd;
}

int epoll_create1(int flags)
{
	/* TODO: we're supposed to support CLOEXEC.  Our FD is a user_fd, so that'd
	 * require some support in glibc's exec to close our epoll ctlr. */
	return epoll_create(1);
}

static int __epoll_ctl_add(struct epoll_ctlr *ep, int fd,
                           struct epoll_event *event)
{
	struct ceq_event *ceq_ev;
	struct ep_fd_data *ep_fd;
	struct fd_tap_req tap_req = {0};
	int ret, filter, sock_listen_fd;
	struct epoll_event listen_event;

	/* Only support ET.  Also, we just ignore EPOLLONESHOT.  That might work,
	 * logically, just with spurious events firing. */
	if (!(event->events & EPOLLET)) {
		errno = EPERM;
		werrstr("Epoll level-triggered not supported");
		return -1;
	}
	/* The sockets-to-plan9 networking shims are a bit inconvenient.  The user
	 * asked us to epoll on an FD, but that FD is actually a Qdata FD.  We might
	 * need to actually epoll on the listen_fd.  Further, we don't know yet
	 * whether or not they want the listen FD.  They could epoll on the socket,
	 * then listen later and want to wake up on the listen.
	 *
	 * So in the case we have a socket FD, we'll actually open the listen FD
	 * regardless (glibc handles this), and we'll epoll on both FDs.
	 * Technically, either FD could fire and they'd get an epoll event for it,
	 * but I think socket users will use only listen or data.
	 *
	 * As far as tracking the FD goes for epoll_wait() reporting, if the app
	 * wants to track the FD they think we are using, then they already passed
	 * that in event->data. */
	sock_listen_fd = _sock_lookup_listen_fd(fd);
	if (sock_listen_fd >= 0) {
		listen_event.events = EPOLLET | EPOLLIN | EPOLLHUP;
		listen_event.data = event->data;
		ret = __epoll_ctl_add(ep, sock_listen_fd, &listen_event);
		if (ret < 0)
			return ret;
	}
	ceq_ev = ep_get_ceq_ev(ep, fd);
	if (!ceq_ev) {
		errno = ENOMEM;
		werrstr("Epoll set cannot grow yet!");
		return -1;
	}
	ep_fd = (struct ep_fd_data*)ceq_ev->user_data;
	if (ep_fd) {
		errno = EEXIST;
		return -1;
	}
	tap_req.fd = fd;
	tap_req.cmd = FDTAP_CMD_ADD;
	/* EPOLLHUP is implicitly set for all epolls. */
	filter = ep_events_to_taps(event->events | EPOLLHUP);
	tap_req.filter = filter;
	tap_req.ev_q = ep->ceq_evq;
	tap_req.ev_id = fd;	/* using FD as the CEQ ID */
	ret = sys_tap_fds(&tap_req, 1);
	if (ret != 1)
		return -1;
	ep_fd = malloc(sizeof(struct ep_fd_data));
	ep_fd->fd = fd;
	ep_fd->filter = filter;
	ep_fd->ep_event = *event;
	ep_fd->ep_event.events |= EPOLLHUP;
	ceq_ev->user_data = (uint64_t)ep_fd;
	return 0;
}

static int __epoll_ctl_del(struct epoll_ctlr *ep, int fd,
                           struct epoll_event *event)
{
	struct ceq_event *ceq_ev;
	struct ep_fd_data *ep_fd;
	struct fd_tap_req tap_req = {0};
	int ret, sock_listen_fd;

	/* If we were dealing with a socket shim FD, we tapped both the listen and
	 * the data file and need to untap both of them. */
	sock_listen_fd = _sock_lookup_listen_fd(fd);
	if (sock_listen_fd >= 0) {
		/* It's possible to fail here.  Even though we tapped it already, if the
		 * deletion was triggered from close callbacks, it's possible for the
		 * sock_listen_fd to be closed first, which would have triggered an
		 * epoll_ctl_del.  When we get around to closing the Rock FD, the listen
		 * FD was already closed. */
		__epoll_ctl_del(ep, sock_listen_fd, event);
	}
	ceq_ev = ep_get_ceq_ev(ep, fd);
	if (!ceq_ev) {
		errno = ENOENT;
		return -1;
	}
	ep_fd = (struct ep_fd_data*)ceq_ev->user_data;
	if (!ep_fd) {
		errno = ENOENT;
		return -1;
	}
	assert(ep_fd->fd == fd);
	tap_req.fd = fd;
	tap_req.cmd = FDTAP_CMD_REM;
	/* ignoring the return value; we could have failed to remove it if the FD
	 * has already closed and the kernel removed the tap. */
	sys_tap_fds(&tap_req, 1);
	ceq_ev->user_data = 0;
	free(ep_fd);
	return 0;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
	int ret;
	struct epoll_ctlr *ep = fd_to_cltr(epfd);
	if (!ep) {
		errno = EBADF;/* or EINVAL */
		return -1;
	}
	if (fd >= USER_FD_BASE) {
		errno = EINVAL;
		werrstr("Epoll can't track User FDs");
		return -1;
	}
	uth_mutex_lock(ep->mtx);
	switch (op) {
		case (EPOLL_CTL_MOD):
			/* In lieu of a proper MOD, just remove and readd.  The errors might
			 * not work out well, and there could be a missed event in the
			 * middle.  Not sure what the guarantees are, but we can fake a
			 * poke. (TODO). */
			ret = __epoll_ctl_del(ep, fd, 0);
			if (ret)
				break;
			ret = __epoll_ctl_add(ep, fd, event);
			break;
		case (EPOLL_CTL_ADD):
			ret = __epoll_ctl_add(ep, fd, event);
			break;
		case (EPOLL_CTL_DEL):
			ret = __epoll_ctl_del(ep, fd, event);
			break;
		default:
			errno = EINVAL;
			ret = -1;
	}
	uth_mutex_unlock(ep->mtx);
	return ret;
}

static bool get_ep_event_from_msg(struct epoll_ctlr *ep, struct event_msg *msg,
                                  struct epoll_event *ep_ev)
{
	struct ceq_event *ceq_ev;
	struct ep_fd_data *ep_fd;

	ceq_ev = ep_get_ceq_ev(ep, msg->ev_type);
	/* should never get a tap FD > size of the epoll set */
	assert(ceq_ev);
	ep_fd = (struct ep_fd_data*)ceq_ev->user_data;
	if (!ep_fd) {
		/* it's possible the FD was unregistered and this was an old
		 * event sent to this epoll set. */
		return FALSE;
	}
	ep_ev->data = ep_fd->ep_event.data;
	ep_ev->events = taps_to_ep_events(msg->ev_arg2);
	return TRUE;
}

/* We should be able to have multiple waiters.  ep shouldn't be closed or
 * anything, since we have the FD (that'd be bad programming on the user's
 * behalf).  We could have concurrent ADD/MOD/DEL operations (which lock). */
static int __epoll_wait(struct epoll_ctlr *ep, struct epoll_event *events,
                        int maxevents, int timeout)
{
	struct event_msg msg = {0};
	struct event_msg dummy_msg;
	struct event_queue *which_evq;
	struct event_queue *alarm_evq;
	int nr_ret = 0;
	int recurse_ret;
	struct syscall sysc;

	/* Locking to protect get_ep_event_from_msg, specifically that the ep_fd
	 * stored at ceq_ev->user_data does not get concurrently removed and
	 * freed. */
	uth_mutex_lock(ep->mtx);
	for (int i = 0; i < maxevents; i++) {
		if (uth_check_evqs(&msg, &which_evq, 1, ep->ceq_evq)) {
			if (get_ep_event_from_msg(ep, &msg, &events[i]))
				nr_ret++;
		}
	}
	uth_mutex_unlock(ep->mtx);
	if (nr_ret)
		return nr_ret;
	if (timeout == 0)
		return 0;
	if (timeout != -1) {
		alarm_evq = ep_get_alarm_evq();
		syscall_async(&sysc, SYS_block, timeout * 1000);
		if (!register_evq(&sysc, alarm_evq)) {
			/* timeout occurred before we could even block! */
			ep_put_alarm_evq(alarm_evq);
			return 0;
		}
		uth_blockon_evqs(&msg, &which_evq, 2, ep->ceq_evq, alarm_evq);
		if (which_evq != alarm_evq) {
			/* sysc may or may not have finished yet.  this will force it to
			 * *start* to finish iff it is still a submitted syscall. */
			sys_abort_sysc(&sysc);
			/* But we still need to wait until the syscall completed.  Need a
			 * dummy msg, since we don't want to clobber the real msg. */
			uth_blockon_evqs(&dummy_msg, 0, 1, alarm_evq);
		}
		/* TODO: Slightly dangerous, due to spammed INDIRs */
		ep_put_alarm_evq(alarm_evq);
		if (which_evq == alarm_evq)
			return 0;
	} else {
		uth_blockon_evqs(&msg, &which_evq, 1, ep->ceq_evq);
	}
	uth_mutex_lock(ep->mtx);
	if (get_ep_event_from_msg(ep, &msg, &events[0]))
		nr_ret++;
	uth_mutex_unlock(ep->mtx);
	/* We might not have gotten one yet.  And regardless, there might be more
	 * available.  Let's try again, with timeout == 0 to ensure no blocking.  We
	 * use nr_ret (0 or 1 now) to adjust maxevents and events accordingly. */
	recurse_ret = __epoll_wait(ep, events + nr_ret, maxevents - nr_ret, 0);
	if (recurse_ret > 0)
		nr_ret += recurse_ret;
	return nr_ret;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
               int timeout)
{
	struct epoll_ctlr *ep = fd_to_cltr(epfd);
	int ret;
	if (!ep) {
		errno = EBADF;/* or EINVAL */
		return -1;
	}
	if (maxevents <= 0) {
		errno = EINVAL;
		return -1;
	}
	ret = __epoll_wait(ep, events, maxevents, timeout);
	return ret;
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
                int timeout, const sigset_t *sigmask)
{
	int ready;
	sigset_t origmask;
	/* TODO: this is probably racy */
	sigprocmask(SIG_SETMASK, sigmask, &origmask);
	ready = epoll_wait(epfd, events, maxevents, timeout);
	sigprocmask(SIG_SETMASK, &origmask, NULL);
	return ready;
}
