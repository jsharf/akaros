/* Copyright (c) 2014 The Regents of the University of California
 * Kevin Klues <klueska@cs.berkeley.edu>
 * See LICENSE for details. */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <parlib.h>
#include <vcore.h>
#include <event.h>
#include <spinlock.h>
#include <arch/atomic.h>
#include <arch/bitmask.h>
#include <sys/queue.h>
#include <fcntl.h>
#include <pvcalarm.h>

/* Different states for enabling/disabling the per-vcore alarms. */
enum {
	S_ENABLING,
	S_ENABLED,
	S_DISABLING,
	S_DISABLED,
};

/* The data associated with each per-vcore alarm that needs to be tracked by
 * each vcore. It is ultimately stored in an __thread variable. */
struct pvcalarm_data {
	int ctlfd;
	int timerfd;
	int alarmid;
	uint64_t start_uptime;
};

/* The global state of the pvcalarm service itself */
struct pvcalarm {
	uint64_t interval;
	void (*callback) (void);

	atomic_t state;
	int busy_count;
	handle_event_t handler;
	struct pvcalarm_data *data;
};

/* The only state we need to make sure is set for the global alarm service is
 * to make sure it s in the disabled state at bootup */
static struct pvcalarm global_pvcalarm = { .state = (void*)S_DISABLED };

/* Helper functions */
static void init_pvcalarm(struct pvcalarm_data *pvcalarm_data, int vcoreid);
static void handle_pvcalarm(struct event_msg *ev_msg, unsigned int ev_type,
                            void *data);
static void handle_alarm_real(struct event_msg *ev_msg, unsigned int ev_type,
                              void *data);
static void handle_alarm_prof(struct event_msg *ev_msg, unsigned int ev_type,
                              void *data);

/* Initialize the pvcalarm service. Only call this function once */
static int init_global_pvcalarm()
{
	global_pvcalarm.interval = 0;
	global_pvcalarm.callback = NULL;
	global_pvcalarm.busy_count = 0;
	global_pvcalarm.handler = NULL;

	/* Preemptively setup timers for all possible vcores */
	global_pvcalarm.data = malloc(max_vcores() * sizeof(struct pvcalarm_data));
	for (int i=0; i<max_vcores(); i++) {
		init_pvcalarm(&global_pvcalarm.data[i], i);
	}
}

/* Run the pvc alarm associated with pvcalarm_data for the given amount of
 * time */
static void run_pvcalarm(struct pvcalarm_data *pvcalarm_data, uint64_t offset)
{
	int ret;
	char buf[20];
	ret = snprintf(buf, sizeof(buf), "%llx", read_tsc() + offset);
	ret = write(pvcalarm_data->timerfd, buf, ret);
	if (ret <= 0) {
		perror("Useralarm: Failed to set timer");
		return;
	}
}

/* Run the pvc alarm associated with pvcalarm_data for the given amount of
 * time. Also mark the start time of the alarm so we can use it for accounting
 * later. */
static void start_pvcalarm(struct pvcalarm_data *pvcalarm_data, uint64_t offset)
{
	pvcalarm_data->start_uptime = vcore_account_uptime_ticks(vcore_id());
	run_pvcalarm(pvcalarm_data, offset);
}

/* Stop the pvc alarm associated with pvcalarm_data */
static void stop_pvcalarm(struct pvcalarm_data *pvcalarm_data)
{
	int ret;
	ret = write(pvcalarm_data->ctlfd, "cancel", sizeof("cancel"));
	if (ret <= 0) {
		printf("Useralarm: unable to disarm alarm!\n");
		return;
	}
}

/* Enable the per-vcore alarm service according to one of the policies listed
 * above.  Every interval usecs the provided callback will be called on each
 * active vcore according to that policy. */
int enable_pvcalarms(int method, uint64_t interval, void (*callback) (void))
{
	assert(!in_vcore_context());
	if (method != PVCALARM_REAL && method != PVCALARM_PROF)
		return EINVAL;

	if (atomic_cas(&global_pvcalarm.state, S_ENABLED, S_ENABLED))
		return EALREADY;

	if (!atomic_cas(&global_pvcalarm.state, S_DISABLED, S_ENABLING))
		return EBUSY;

	run_once_racy(init_global_pvcalarm());

	global_pvcalarm.interval = usec2tsc(interval);
	global_pvcalarm.callback = callback;
	global_pvcalarm.busy_count = 0;
	switch (method) {
		case PVCALARM_REAL:
			global_pvcalarm.handler = handle_alarm_real;
			break;
		case PVCALARM_PROF:
			global_pvcalarm.handler = handle_alarm_prof;
			break;
	}

	/* Start the timer on all vcores to go off after interval usecs */
	for (int i=0; i<max_vcores(); i++) {
		start_pvcalarm(&global_pvcalarm.data[i], global_pvcalarm.interval);
	}

	atomic_set(&global_pvcalarm.state, S_ENABLED);
	return 0;
}

/* Disable the currently active per-vcore alarm service */
int disable_pvcalarms()
{
	assert(!in_vcore_context());
	if (atomic_cas(&global_pvcalarm.state, S_DISABLED, S_DISABLED))
		return EALREADY;

	if (!atomic_cas(&global_pvcalarm.state, S_ENABLED, S_DISABLING))
		return EBUSY;

	/* We loop here to let any vcores currently running code associated with
	 * the pvcalarms to finish what they are doing before we disable the
	 * pvcalarm service.  Since we ensure that this function is only called
	 * from non-vcore context, this is OK. */
	while(global_pvcalarm.busy_count != 0)
		cpu_relax();
	
	global_pvcalarm.interval = 0;
	global_pvcalarm.callback = NULL;
	global_pvcalarm.handler = NULL;

	/* Stop the timer on all vcores */
	for (int i=0; i<max_vcores(); i++)
		stop_pvcalarm(&global_pvcalarm.data[i]);

	atomic_set(&global_pvcalarm.state, S_DISABLED);
}

/* Initialize a specific pvcalarm.  This happens once per vcore as it comes
 * online and the pvcalarm service is active */
static void init_pvcalarm(struct pvcalarm_data *pvcalarm_data, int vcoreid)
{
	int ctlfd, timerfd, alarmid, ev_flags, ret;
	char buf[20];
	char path[32];
	struct event_queue *ev_q;

	ctlfd = open("#A/clone", O_RDWR | O_CLOEXEC);
	if (ctlfd < 0) {
		perror("Pvcalarm: Can't clone an alarm");
		return;
	}
	ret = read(ctlfd, buf, sizeof(buf) - 1);
	if (ret <= 0) {
		if (!ret)
			printf("Pvcalarm: Got early EOF from ctl\n");
		else
			perror("Pvcalarm: Can't read ctl");
		return;
	}
	buf[ret] = 0;
	alarmid = atoi(buf);
	snprintf(path, sizeof(path), "#A/a%s/timer", buf);
	timerfd = open(path, O_RDWR | O_CLOEXEC);
	if (timerfd < 0) {
		perror("Pvcalarm: Can't open timer");
		return;
	}
	register_ev_handler(EV_ALARM, handle_pvcalarm, 0);
	ev_flags = EVENT_IPI | EVENT_VCORE_PRIVATE;
	ev_q = get_event_q_vcpd(vcoreid, ev_flags);
	if (!ev_q) {
		perror("Pvcalarm: Failed ev_q");
		return;
	}
	ev_q->ev_vcore = vcoreid;
	ev_q->ev_flags = ev_flags;
	ret = snprintf(path, sizeof(path), "evq %llx", ev_q);
	ret = write(ctlfd, path, ret);
	if (ret <= 0) {
		perror("Pvcalarm: Failed to write ev_q");
		return;
	}
	/* now the alarm is all set, just need to write the timer whenever we want
	 * it to go off. */
	pvcalarm_data->alarmid = alarmid;
	pvcalarm_data->ctlfd = ctlfd;
	pvcalarm_data->timerfd = timerfd;
}

/* TODO: implement a way to completely remove each per-vcore alarm and
 * deregister it from the #A device */

/* A preamble function to run anytime we are about to do anything on behalf of
 * the pvcalarms while in vcore context.  This preamble is necessary to ensure
 * we maintain proper invariants when enabling and disabling the pvcalarm
 * service in a running application. */
static inline bool __vcore_preamble()
{
	assert(in_vcore_context());
	__sync_fetch_and_add(&global_pvcalarm.busy_count, 1);
	if (atomic_cas(&global_pvcalarm.state, S_DISABLED, S_DISABLED))
		goto disabled;
	if (atomic_cas(&global_pvcalarm.state, S_DISABLING, S_DISABLING))
		goto disabled;
	return true;
disabled:
	__sync_fetch_and_add(&global_pvcalarm.busy_count, -1);
	return false;
}

/* The counterpart to the __vcore_preamble() function */
static inline void __vcore_postamble()
{
	__sync_fetch_and_add(&global_pvcalarm.busy_count, -1);
}

/* The global handler function.  It simply calls the proper underlying handler
 * function depending on whether the service is set for the REAL or PERF
 * policy. */
static void handle_pvcalarm(struct event_msg *ev_msg, unsigned int ev_type,
                            void *data)
{
	struct pvcalarm_data *pvcalarm_data = &global_pvcalarm.data[vcore_id()];
	if (!ev_msg || (ev_msg->ev_arg2 != pvcalarm_data->alarmid))
		return;
	if (!__vcore_preamble()) return;
	global_pvcalarm.handler(ev_msg, ev_type, data);
	__vcore_postamble();
}

/* The pvcalarm handler for the REAL policy.  Simply call the registered
 * callback and restart the interval alarm. */
static void handle_alarm_real(struct event_msg *ev_msg, unsigned int ev_type,
                              void *data)
{
	global_pvcalarm.callback();
	start_pvcalarm(&global_pvcalarm.data[vcore_id()], global_pvcalarm.interval);
}

/* The pvcalarm handler for the PROF policy.  Account for any time the vcore
 * has been offline.  Only when the uptime since the last interval is equal to
 * the interval time do we run the callback function.  Otherwise we restart the
 * alarm to make up the difference. */
static void handle_alarm_prof(struct event_msg *ev_msg, unsigned int ev_type,
                              void *data)
{ 
	int vcoreid = vcore_id();
	struct pvcalarm_data *pvcalarm_data = &global_pvcalarm.data[vcoreid];
	uint32_t uptime = vcore_account_uptime_ticks(vcoreid);
	uint64_t diff = uptime - pvcalarm_data->start_uptime;

	if (diff < global_pvcalarm.interval) {
		uint64_t remaining = global_pvcalarm.interval - diff;
		run_pvcalarm(pvcalarm_data, remaining);
	} else {
		global_pvcalarm.callback();
		start_pvcalarm(pvcalarm_data, global_pvcalarm.interval);
	}
}
