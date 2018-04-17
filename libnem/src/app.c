#include "nem.h"

#include <sys/time.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <stdio.h>

static inline int
NEM_timer1_cmp(const void *vlhs, const void *vrhs)
{
	const NEM_timer1_t *lhs = vlhs;
	const NEM_timer1_t *rhs = vrhs;

	if (lhs->invoke_at.tv_sec < rhs->invoke_at.tv_sec) {
		return -1;
	}
	else if (lhs->invoke_at.tv_sec > rhs->invoke_at.tv_sec) {
		return 1;
	}
	else if (lhs->invoke_at.tv_usec < rhs->invoke_at.tv_usec) {
		return -1;
	}
	else if (lhs->invoke_at.tv_usec > rhs->invoke_at.tv_usec) {
		return 1;
	}

	return 0;
}

static inline bool
NEM_timer1_before(NEM_timer1_t *this, struct timeval t)
{
	if (this->invoke_at.tv_sec > t.tv_sec) {
		return false;
	}
	else if (this->invoke_at.tv_sec < t.tv_sec) {
		return true;
	}
	else {
		return this->invoke_at.tv_usec <= t.tv_usec;
	}
}

// XXX: Use SPLAY_PROTOTYPE_STATIC/SPLAY_GENERATE_STATIC instead here.
SPLAY_PROTOTYPE(NEM_timer1_tree_t, NEM_timer1_t, link, NEM_timer1_cmp);
SPLAY_GENERATE(NEM_timer1_tree_t, NEM_timer1_t, link, NEM_timer1_cmp);

static NEM_err_t
NEM_app_timer1_schedule(NEM_app_t *this, struct timeval now, NEM_timer1_t *next)
{
	struct kevent ev;

	if (NULL == next) {
		// Timers are configured as one-shot, so we don't have to bother
		// clearing anything here. We just don't set another timer event.
		return NEM_err_none;
	}

	// Otherwise set the timer.
	intptr_t after_ms = 0;
	after_ms += 1000 * (next->invoke_at.tv_sec - now.tv_sec);
	after_ms += (next->invoke_at.tv_usec - now.tv_usec) / 1000;

	// NB: I'm not sure why this needs to be done. According to the
	// documentation, EV_ADD with the same ident should _update_ the timer
	// data field -- but this does not appear to be working for some
	// godforsaken reason. Manually delete and re-add the timer. This returns
	// ENOENT if the timer isn't currently configured to run, which is ignored.
	EV_SET(&ev, this->kq, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	kevent(this->kq, &ev, 1, NULL, 0, NULL);

	EV_SET(
		&ev,
		this->kq,
		EVFILT_TIMER,
		EV_ADD | EV_ONESHOT,
		NOTE_MSECONDS,
		after_ms,
		this->on_timer
	);
	if (0 != kevent(this->kq, &ev, 1, NULL, 0, NULL)) {
		return NEM_err_errno();
	}

	return NEM_err_none;
}

static inline void
NEM_app_timer1_now(struct timeval *t)
{
	if (gettimeofday(t, NULL)) {
		NEM_panicf("gettimeofday: %s", strerror(errno));
	}
}

static inline void
NEM_app_timer1_add_ms(struct timeval *t, int ms)
{
	t->tv_sec += ms / 1000;
	t->tv_usec += (ms % 1000) * 1000;

	// NB: Overflow these values for collation reasons.
	t->tv_sec += t->tv_usec / (1000 * 1000);
	t->tv_usec = t->tv_usec % (1000 * 1000);
}

static void
NEM_app_on_timer(NEM_thunk_t *thunk, void *varg)
{
	NEM_app_t *this = NEM_thunk_ptr(thunk);
	struct timeval now;
	NEM_app_timer1_now(&now);

	for (;;) {
		NEM_timer1_t *timer = SPLAY_MIN(NEM_timer1_tree_t, &this->timers);
		if (NULL == timer || !NEM_timer1_before(timer, now)) {
			break;
		}

		NEM_thunk1_invoke(&timer->thunk, NULL);
		SPLAY_REMOVE(NEM_timer1_tree_t, &this->timers, timer);
		free(timer);
	}

	NEM_err_t err = NEM_app_timer1_schedule(
		this,
		now,
		SPLAY_MIN(NEM_timer1_tree_t, &this->timers)
	);
	if (!NEM_err_ok(err)) {
		NEM_panicf("NEM_app_on_timer: %s", NEM_err_string(err));
	}
}

static void
NEM_app_on_stop(NEM_thunk1_t *thunk, void *varg)
{
	NEM_app_t *this = NEM_thunk1_ptr(thunk);
	this->running = false;
}

static NEM_err_t
NEM_app_init_internal(NEM_app_t *this)
{
	bzero(this, sizeof(*this));
	this->kq = kqueue();
	if (-1 == this->kq) {
		return NEM_err_errno();
	}

	this->on_timer = NEM_thunk_new_ptr(&NEM_app_on_timer, this);
	SPLAY_INIT(&this->timers);
	return NEM_err_none;
}

static void
NEM_app_free_fd(NEM_thunk1_t *thunk, void *varg)
{
	NEM_fd_t *fd = NEM_thunk1_ptr(thunk);
	NEM_fd_free(fd);
	free(fd);
}

NEM_err_t
NEM_app_init(NEM_app_t *this)
{
	// Pre-emptively check to see if the fd is valid.
	if (-1 == fcntl(NEM_APP_FILENO, F_GETFD)) {
		if (EBADF == errno) {
			return NEM_err_static("NEM_app_init: didn't get passed a parent fd?");
		}
		return NEM_err_errno();
	}

	NEM_err_t err = NEM_app_init_internal(this);
	if (!NEM_err_ok(err)) {
		return err;
	}

	NEM_fd_t *fd = NEM_malloc(sizeof(NEM_fd_t));
	err = NEM_fd_init(fd, this->kq, NEM_APP_FILENO);
	if (!NEM_err_ok(err)) {
		free(fd);
		return err;
	}

	NEM_fd_on_close(fd, NEM_thunk1_new_ptr(
		&NEM_app_free_fd,
		fd
	));

	this->chan = NEM_malloc(sizeof(NEM_chan_t));
	NEM_chan_init(this->chan, NEM_fd_as_stream(fd));
	return NEM_err_none;
}

NEM_err_t
NEM_app_init_root(NEM_app_t *this)
{
	return NEM_app_init_internal(this);
}

void
NEM_app_free(NEM_app_t *this)
{
	if (this->running) {
		NEM_panic("NEM_app_free: app still running!");
	}

	if (NULL != this->chan) {
		NEM_chan_free(this->chan);
		free(this->chan);
	}

	NEM_timer1_t *timer = NULL;
	while (NULL != (timer = SPLAY_MIN(NEM_timer1_tree_t, &this->timers))) {
		SPLAY_REMOVE(NEM_timer1_tree_t, &this->timers, timer);
		NEM_thunk1_discard(&timer->thunk);
		free(timer);
	}

	NEM_thunk_free(this->on_timer);
	if (0 != close(this->kq)) {
		NEM_panicf_errno("NEM_app_free: close(kq): %s");
	}

	this->kq = 0;
}

static void
NEM_app_insert_timer(NEM_app_t *this, NEM_timer1_t *timer, struct timeval now)
{
	SPLAY_INSERT(NEM_timer1_tree_t, &this->timers, timer);

	// If the inserted timer should occur before the currently scheduled
	// interruption, update the thingy to fire sooner.
	NEM_timer1_t *next_timer = SPLAY_MIN(NEM_timer1_tree_t, &this->timers);
	if (timer == next_timer) {
		NEM_err_t err = NEM_app_timer1_schedule(this, now, next_timer);
		if (!NEM_err_ok(err)) {
			NEM_panicf("NEM_app_insert_timer: %s", NEM_err_string(err));
		}
	}
}

static NEM_timer1_t*
NEM_app_after_internal(NEM_app_t *this, uint64_t ms, NEM_thunk1_t *cb)
{
	struct timeval now;
	NEM_app_timer1_now(&now);

	NEM_timer1_t *timer = NEM_malloc(sizeof(NEM_timer1_t));
	timer->thunk = cb;
	timer->invoke_at = now;
	NEM_app_timer1_add_ms(&timer->invoke_at, ms);

	NEM_app_insert_timer(this, timer, now);

	return timer;
}

void
NEM_app_after(NEM_app_t *this, uint64_t ms, NEM_thunk1_t *cb)
{
	NEM_app_after_internal(this, ms, cb);
}

void
NEM_app_defer(NEM_app_t *this, NEM_thunk1_t *cb)
{
	NEM_app_after(this, 0, cb);
}

void
NEM_timer_init(NEM_timer_t *this, NEM_app_t *app, NEM_thunk_t *thunk)
{
	bzero(this, sizeof(*this));
	this->app = app;
	this->thunk = thunk;
}

static void
NEM_timer_on_timer(NEM_thunk1_t *thunk, void *varg)
{
	NEM_timer_t *this = NEM_thunk1_ptr(thunk);
	this->active = NULL;

	NEM_thunk_invoke(this->thunk, this);
}

void
NEM_timer_set(NEM_timer_t *this, uint64_t ms_after)
{
	if (NULL != this->active) {
		// Sneakily adjust the underlying timer entry.
		SPLAY_REMOVE(NEM_timer1_tree_t, &this->app->timers, this->active);

		struct timeval now;
		NEM_app_timer1_now(&now);
		this->active->invoke_at = now;
		NEM_app_timer1_add_ms(&this->active->invoke_at, ms_after);

		NEM_app_insert_timer(this->app, this->active, now);
	}
	else {
		// Otherwise, need to create a new timer entry.
		this->active = NEM_app_after_internal(
			this->app,
			ms_after,
			NEM_thunk1_new_ptr(
				&NEM_timer_on_timer,
				this
			)
		);
	}
}

void
NEM_timer_cancel(NEM_timer_t *this)
{
	if (NULL == this->active) {
		return;
	}

	SPLAY_REMOVE(NEM_timer1_tree_t, &this->app->timers, this->active);
	NEM_thunk1_discard(&this->active->thunk);
	free(this->active);
	this->active = NULL;
}

void
NEM_timer_free(NEM_timer_t *this)
{
	NEM_timer_cancel(this);
	NEM_thunk_free(this->thunk);
}

static const char *
evfilt_str(int ev)
{
	struct {
		int filt;
		const char *name;
	}
	evs[] = {
		{ EVFILT_READ,   "read"   },
		{ EVFILT_WRITE,  "write"  },
		{ EVFILT_AIO,    "aio"    },
		{ EVFILT_VNODE,  "vnode"  },
		{ EVFILT_PROC,   "proc"   },
		{ EVFILT_SIGNAL, "signal" },
		{ EVFILT_TIMER,  "timer"  },
		{ EVFILT_USER,   "user"   },
	};

	for (size_t i = 0; i < NEM_ARRSIZE(evs); i += 1) {
		if (evs[i].filt == ev) {
			return evs[i].name;
		}
	}

	return "unknown";
}

NEM_err_t
NEM_app_run(NEM_app_t *this)
{
	if (0 == this->kq) {
		NEM_panic("NEM_app_run: app not initialized");
	}

	this->running = true;

	while (this->running) {
		struct kevent trig;
		if (-1 == kevent(this->kq, NULL, 0, &trig, 1, NULL)) {
			NEM_panic("NEM_app_run: kevent");
		}

		if (EV_ERROR == (trig.flags & EV_ERROR)) {
			// XXX: The thunks should process these errors and we shouldn't
			// be logging them.
			fprintf(stderr, "NEM_app_run: EV_ERROR: %s", strerror(trig.data));
			break;
		}

		NEM_thunk_t *thunk = trig.udata;
		if (NULL == trig.udata) {
			NEM_panicf("NEM_app_run: NULL udata filter=%d", trig.filter);
		}
		NEM_thunk_invoke(thunk, &trig);
	}

	return NEM_err_none;
}

void
NEM_app_stop(NEM_app_t *this)
{
	// NB: Setting a timer procs the kqueue to deliver a message, breaking
	// it out of the wait loop.
	NEM_app_after(this, 0, NEM_thunk1_new_ptr(&NEM_app_on_stop, this));
}
