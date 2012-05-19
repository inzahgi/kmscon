/*
 * Event Loop
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Event Loop
 * This provides a basic event loop similar to those provided by glib etc.
 * It uses linux specific features like signalfd so it may not be easy to port
 * it to other platforms.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "eloop.h"
#include "log.h"
#include "misc.h"

#define LOG_SUBSYSTEM "eloop"

struct ev_eloop {
	int efd;
	unsigned long ref;
	struct ev_fd *fd;
	struct ev_counter *cnt;

	struct kmscon_dlist sig_list;
	struct kmscon_hook *idlers;

	struct epoll_event *cur_fds;
	size_t cur_fds_cnt;
	bool exit;
};

struct ev_fd {
	unsigned long ref;
	int fd;
	int mask;
	ev_fd_cb cb;
	void *data;

	bool enabled;
	struct ev_eloop *loop;
};

struct ev_timer {
	unsigned long ref;
	ev_timer_cb cb;
	void *data;

	int fd;
	struct ev_fd *efd;
};

struct ev_counter {
	unsigned long ref;
	ev_counter_cb cb;
	void *data;

	int fd;
	struct ev_fd *efd;
};

struct ev_signal_shared {
	struct kmscon_dlist list;

	struct ev_fd *fd;
	int signum;
	struct kmscon_hook *hook;
};

/*
 * Shared signals
 */

static void sig_child()
{
	pid_t pid;
	int status;

	while (1) {
		pid = waitpid(-1, &status, WNOHANG);
		if (pid == -1) {
			if (errno != ECHILD)
				log_warn("cannot wait on child: %m");
			break;
		} else if (pid == 0) {
			break;
		} else if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) != 0)
				log_debug("child %d exited with status %d",
					pid, WEXITSTATUS(status));
			else
				log_debug("child %d exited successfully", pid);
		} else if (WIFSIGNALED(status)) {
			log_debug("child %d exited by signal %d", pid,
					WTERMSIG(status));
		}
	}
}

static void shared_signal_cb(struct ev_fd *fd, int mask, void *data)
{
	struct ev_signal_shared *sig = data;
	struct signalfd_siginfo info;
	int len;

	if (mask & EV_READABLE) {
		len = read(fd->fd, &info, sizeof(info));
		if (len != sizeof(info))
			log_warn("cannot read signalfd");
		else
			kmscon_hook_call(sig->hook, sig->fd->loop, &info);

		if (info.ssi_signo == SIGCHLD)
			sig_child();
	} else if (mask & (EV_HUP | EV_ERR)) {
		log_warn("HUP/ERR on signal source");
	}
}

static int signal_new(struct ev_signal_shared **out, struct ev_eloop *loop,
			int signum)
{
	sigset_t mask;
	int ret, fd;
	struct ev_signal_shared *sig;

	if (!out || !loop || signum < 0)
		return -EINVAL;

	sig = malloc(sizeof(*sig));
	if (!sig)
		return -ENOMEM;
	memset(sig, 0, sizeof(*sig));
	sig->signum = signum;

	ret = kmscon_hook_new(&sig->hook);
	if (ret)
		goto err_free;

	sigemptyset(&mask);
	sigaddset(&mask, signum);

	fd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		goto err_hook;
	}

	ret = ev_eloop_new_fd(loop, &sig->fd, fd, EV_READABLE,
				shared_signal_cb, sig);
	if (ret)
		goto err_sig;

	pthread_sigmask(SIG_BLOCK, &mask, NULL);
	kmscon_dlist_link(&loop->sig_list, &sig->list);

	*out = sig;
	return 0;

err_sig:
	close(fd);
err_hook:
	kmscon_hook_free(sig->hook);
err_free:
	free(sig);
	return ret;
}

static void signal_free(struct ev_signal_shared *sig)
{
	int fd;

	if (!sig)
		return;

	kmscon_dlist_unlink(&sig->list);
	fd = sig->fd->fd;
	ev_eloop_rm_fd(sig->fd);
	close(fd);
	kmscon_hook_free(sig->hook);
	free(sig);
	/* We do not unblock the signal here as there may be other subsystems
	 * which blocked this signal so we do not want to interfere. If you need
	 * a clean sigmask then do it yourself.
	 */
}

/*
 * Eloop mainloop
 */

static void eloop_event(struct ev_fd *fd, int mask, void *data)
{
	struct ev_eloop *eloop = data;

	if (mask & EV_READABLE)
		ev_eloop_dispatch(eloop, 0);
	if (mask & (EV_HUP | EV_ERR))
		log_warn("HUP/ERR on eloop source");
}

static void eloop_cnt_event(struct ev_counter *cnt, uint64_t num, void *data)
{
	struct ev_eloop *eloop = data;
	int ret;

	if (!num) {
		log_warning("HUP/ERR on eloop idle-counter");
		return;
	}

	kmscon_hook_call(eloop->idlers, eloop, NULL);
	if (kmscon_hook_num(eloop->idlers) > 0) {
		ret = ev_counter_inc(eloop->cnt, 1);
		if (ret)
			log_warning("cannot increase eloop idle-counter");
	}
}

int ev_eloop_new(struct ev_eloop **out)
{
	struct ev_eloop *loop;
	int ret;

	if (!out)
		return -EINVAL;

	loop = malloc(sizeof(*loop));
	if (!loop)
		return -ENOMEM;

	memset(loop, 0, sizeof(*loop));
	loop->ref = 1;
	kmscon_dlist_init(&loop->sig_list);

	ret = kmscon_hook_new(&loop->idlers);
	if (ret)
		goto err_free;

	loop->efd = epoll_create1(EPOLL_CLOEXEC);
	if (loop->efd < 0) {
		ret = -errno;
		goto err_idlers;
	}

	ret = ev_fd_new(&loop->fd, loop->efd, EV_READABLE, eloop_event, loop);
	if (ret)
		goto err_close;

	ret = ev_eloop_new_counter(loop, &loop->cnt, eloop_cnt_event, loop);
	if (ret)
		goto err_fd;

	log_debug("new eloop object %p", loop);
	*out = loop;
	return 0;

err_fd:
	ev_fd_unref(loop->fd);
err_close:
	close(loop->efd);
err_idlers:
	kmscon_hook_free(loop->idlers);
err_free:
	free(loop);
	return ret;
}

void ev_eloop_ref(struct ev_eloop *loop)
{
	if (!loop)
		return;

	++loop->ref;
}

void ev_eloop_unref(struct ev_eloop *loop)
{
	struct ev_signal_shared *sig;

	if (!loop || !loop->ref || --loop->ref)
		return;

	log_debug("free eloop object %p", loop);

	while (loop->sig_list.next != &loop->sig_list) {
		sig = kmscon_dlist_entry(loop->sig_list.next,
					struct ev_signal_shared,
					list);
		signal_free(sig);
	}

	ev_eloop_rm_counter(loop->cnt);
	ev_fd_unref(loop->fd);
	close(loop->efd);
	kmscon_hook_free(loop->idlers);
	free(loop);
}

void ev_eloop_flush_fd(struct ev_eloop *loop, struct ev_fd *fd)
{
	int i;

	if (!loop || !fd)
		return;

	for (i = 0; i < loop->cur_fds_cnt; ++i) {
		if (loop->cur_fds[i].data.ptr == fd)
			loop->cur_fds[i].data.ptr = NULL;
	}
}

int ev_eloop_dispatch(struct ev_eloop *loop, int timeout)
{
	struct epoll_event ep[32];
	struct ev_fd *fd;
	int i, count, mask;

	if (!loop || loop->exit)
		return -EINVAL;

	/* dispatch fd events */
	count = epoll_wait(loop->efd, ep, 32, timeout);
	if (count < 0) {
		if (errno == EINTR) {
			count = 0;
		} else {
			log_warn("epoll_wait dispatching failed: %m");
			return -errno;
		}
	}

	loop->cur_fds = ep;
	loop->cur_fds_cnt = count;

	for (i = 0; i < count; ++i) {
		fd = ep[i].data.ptr;
		if (!fd || !fd->cb || !fd->enabled)
			continue;

		mask = 0;
		if (ep[i].events & EPOLLIN)
			mask |= EV_READABLE;
		if (ep[i].events & EPOLLOUT)
			mask |= EV_WRITEABLE;
		if (ep[i].events & EPOLLERR)
			mask |= EV_ERR;
		if (ep[i].events & EPOLLHUP) {
			mask |= EV_HUP;
			ev_fd_disable(fd);
		}

		fd->cb(fd, mask, fd->data);
	}

	loop->cur_fds = NULL;
	loop->cur_fds_cnt = 0;

	return 0;
}

/* ev_eloop_dispatch() performs one idle-roundtrip. This function performs as
 * many idle-roundtrips as needed to run \timeout milliseconds.
 * If \timeout is 0, this is equal to ev_eloop_dispath(), if \timeout is <0,
 * this runs until \loop->exit becomes true.
 */
int ev_eloop_run(struct ev_eloop *loop, int timeout)
{
	int ret;
	struct timeval tv, start;
	int64_t off, msec;

	if (!loop)
		return -EINVAL;
	loop->exit = false;

	log_debug("run for %d msecs", timeout);
	gettimeofday(&start, NULL);

	while (!loop->exit) {
		ret = ev_eloop_dispatch(loop, timeout);
		if (ret)
			return ret;

		if (!timeout) {
			break;
		} else if (timeout > 0) {
			gettimeofday(&tv, NULL);
			off = tv.tv_sec - start.tv_sec;
			msec = (int64_t)tv.tv_usec - (int64_t)start.tv_usec;
			if (msec < 0) {
				off -= 1;
				msec = 1000000 + msec;
			}
			off *= 1000;
			off += msec / 1000;
			if (off >= timeout)
				break;
		}
	}

	return 0;
}

void ev_eloop_exit(struct ev_eloop *loop)
{
	if (!loop)
		return;

	log_debug("exiting %p", loop);

	loop->exit = true;
	if (loop->fd->loop)
		ev_eloop_exit(loop->fd->loop);
}

int ev_eloop_new_eloop(struct ev_eloop *loop, struct ev_eloop **out)
{
	struct ev_eloop *el;
	int ret;

	if (!out || !loop)
		return -EINVAL;

	ret = ev_eloop_new(&el);
	if (ret)
		return ret;

	ret = ev_eloop_add_eloop(loop, el);
	if (ret) {
		ev_eloop_unref(el);
		return ret;
	}

	ev_eloop_unref(el);
	*out = el;
	return 0;
}

int ev_eloop_add_eloop(struct ev_eloop *loop, struct ev_eloop *add)
{
	int ret;

	if (!loop || !add)
		return -EINVAL;

	if (add->fd->loop)
		return -EALREADY;

	/* This adds the epoll-fd into the parent epoll-set. This works
	 * perfectly well with registered FDs, timers, etc. However, we use
	 * shared signals in this event-loop so if the parent and child have
	 * overlapping shared-signals, then the signal will be randomly
	 * delivered to either the parent-hook or child-hook but never both.
	 * TODO:
	 * We may fix this by linking the childs-sig_list into the parent's
	 * siglist but we didn't need this, yet, so ignore it here.
	 */

	ret = ev_eloop_add_fd(loop, add->fd);
	if (ret)
		return ret;

	ev_eloop_ref(add);
	return 0;
}

void ev_eloop_rm_eloop(struct ev_eloop *rm)
{
	if (!rm || !rm->fd->loop)
		return;

	ev_eloop_rm_fd(rm->fd);
	ev_eloop_unref(rm);
}

/*
 * FD sources
 */

int ev_fd_new(struct ev_fd **out, int rfd, int mask, ev_fd_cb cb, void *data)
{
	struct ev_fd *fd;

	if (!out || rfd < 0)
		return -EINVAL;

	fd = malloc(sizeof(*fd));
	if (!fd)
		return -ENOMEM;

	memset(fd, 0, sizeof(*fd));
	fd->ref = 1;
	fd->fd = rfd;
	fd->mask = mask;
	fd->cb = cb;
	fd->data = data;
	fd->enabled = true;

	*out = fd;
	return 0;
}

void ev_fd_ref(struct ev_fd *fd)
{
	if (!fd)
		return;

	++fd->ref;
}

void ev_fd_unref(struct ev_fd *fd)
{
	if (!fd || !fd->ref || --fd->ref)
		return;

	free(fd);
}

static int fd_epoll_add(struct ev_fd *fd)
{
	struct epoll_event ep;
	int ret;

	if (!fd->loop)
		return 0;

	memset(&ep, 0, sizeof(ep));
	if (fd->mask & EV_READABLE)
		ep.events |= EPOLLIN;
	if (fd->mask & EV_WRITEABLE)
		ep.events |= EPOLLOUT;
	ep.data.ptr = fd;

	ret = epoll_ctl(fd->loop->efd, EPOLL_CTL_ADD, fd->fd, &ep);
	if (ret) {
		log_warning("cannot add fd %d to epoll set (%d): %m",
			    fd->fd, errno);
		return -EFAULT;
	}

	return 0;
}

static void fd_epoll_remove(struct ev_fd *fd)
{
	int ret;

	if (!fd->loop)
		return;

	ret = epoll_ctl(fd->loop->efd, EPOLL_CTL_DEL, fd->fd, NULL);
	if (ret)
		log_warning("cannto remote fd %d from epoll set (%d): %m",
			    fd->fd, errno);
}

static int fd_epoll_update(struct ev_fd *fd)
{
	struct epoll_event ep;
	int ret;

	if (!fd->loop)
		return 0;

	memset(&ep, 0, sizeof(ep));
	if (fd->mask & EV_READABLE)
		ep.events |= EPOLLIN;
	if (fd->mask & EV_WRITEABLE)
		ep.events |= EPOLLOUT;
	ep.data.ptr = fd;

	ret = epoll_ctl(fd->loop->efd,  EPOLL_CTL_MOD, fd->fd, &ep);
	if (ret) {
		log_warning("cannot update epoll fd %d (%d): %m",
			    fd->fd, errno);
		return -EFAULT;
	}

	return 0;
}

int ev_fd_enable(struct ev_fd *fd)
{
	int ret;

	if (!fd)
		return -EINVAL;
	if (fd->enabled)
		return 0;

	ret = fd_epoll_add(fd);
	if (ret)
		return ret;

	fd->enabled = true;
	return 0;
}

void ev_fd_disable(struct ev_fd *fd)
{
	if (!fd || !fd->enabled)
		return;

	fd->enabled = false;
	fd_epoll_remove(fd);
}

bool ev_fd_is_enabled(struct ev_fd *fd)
{
	return fd && fd->enabled;
}

bool ev_fd_is_bound(struct ev_fd *fd)
{
	return fd && fd->loop;
}

void ev_fd_set_cb_data(struct ev_fd *fd, ev_fd_cb cb, void *data)
{
	if (!fd)
		return;

	fd->cb = cb;
	fd->data = data;
}

int ev_fd_update(struct ev_fd *fd, int mask)
{
	int ret;
	int omask;

	if (!fd)
		return -EINVAL;

	omask = fd->mask;
	fd->mask = mask;

	if (!fd->enabled)
		return 0;

	ret = fd_epoll_update(fd);
	if (ret) {
		fd->mask = omask;
		return ret;
	}

	return 0;
}

int ev_eloop_new_fd(struct ev_eloop *loop, struct ev_fd **out, int rfd,
			int mask, ev_fd_cb cb, void *data)
{
	struct ev_fd *fd;
	int ret;

	if (!out || !loop || rfd < 0)
		return -EINVAL;

	ret = ev_fd_new(&fd, rfd, mask, cb, data);
	if (ret)
		return ret;

	ret = ev_eloop_add_fd(loop, fd);
	if (ret) {
		ev_fd_unref(fd);
		return ret;
	}

	ev_fd_unref(fd);
	*out = fd;
	return 0;
}

int ev_eloop_add_fd(struct ev_eloop *loop, struct ev_fd *fd)
{
	int ret;

	if (!loop || !fd || fd->loop)
		return -EINVAL;

	fd->loop = loop;

	if (fd->enabled) {
		ret = fd_epoll_add(fd);
		if (ret) {
			fd->loop = NULL;
			return ret;
		}
	}

	ev_fd_ref(fd);
	ev_eloop_ref(loop);
	return 0;
}

void ev_eloop_rm_fd(struct ev_fd *fd)
{
	struct ev_eloop *loop;
	size_t i;

	if (!fd || !fd->loop)
		return;

	loop = fd->loop;
	if (fd->enabled)
		fd_epoll_remove(fd);

	/*
	 * If we are currently dispatching events, we need to remove ourself
	 * from the temporary event list.
	 */
	for (i = 0; i < loop->cur_fds_cnt; ++i) {
		if (fd == loop->cur_fds[i].data.ptr)
			loop->cur_fds[i].data.ptr = NULL;
	}

	fd->loop = NULL;
	ev_fd_unref(fd);
	ev_eloop_unref(loop);
}

/*
 * Timer sources
 * Timer sources allow delaying a specific event by an relative timeout. The
 * timeout can be set to trigger after a specific time. Optionally, you can
 * also make the timeout trigger every next time the timeout elapses so you
 * basically get a pulse that reliably calls the callback.
 * The callback gets as parameter the number of timeouts that elapsed since it
 * was last called (in case the application couldn't call the callback fast
 * enough). The timeout can be specified with nano-seconds precision. However,
 * real precision depends on the operating-system and hardware.
 */

static void timer_cb(struct ev_fd *fd, int mask, void *data)
{
	struct ev_timer *timer = data;
	uint64_t expirations;
	int len;

	if (mask & (EV_HUP | EV_ERR)) {
		log_warn("HUP/ERR on timer source");
		if (timer->cb)
			timer->cb(timer, 0, timer->data);
		return;
	}

	if (mask & EV_READABLE) {
		len = read(timer->fd, &expirations, sizeof(expirations));
		if (len < 0) {
			if (errno != EAGAIN) {
				log_warning("cannot read timerfd (%d): %m",
					    errno);
				ev_timer_disable(timer);
				if (timer->cb)
					timer->cb(timer, 0, timer->data);
			}
		} else if (len == 0) {
			log_warning("EOF on timer source");
			ev_timer_disable(timer);
			if (timer->cb)
				timer->cb(timer, 0, timer->data);
		} else if (len != sizeof(expirations)) {
			log_warn("invalid size %d read on timerfd", len);
			ev_timer_disable(timer);
			if (timer->cb)
				timer->cb(timer, 0, timer->data);
		} else if (expirations > 0) {
			if (timer->cb)
				timer->cb(timer, expirations, timer->data);
		}
	}
}

int ev_timer_new(struct ev_timer **out, const struct itimerspec *spec,
		 ev_timer_cb cb, void *data)
{
	struct ev_timer *timer;
	int ret;

	if (!out)
		return -EINVAL;

	timer = malloc(sizeof(*timer));
	if (!timer)
		return -ENOMEM;

	memset(timer, 0, sizeof(*timer));
	timer->ref = 1;
	timer->cb = cb;
	timer->data = data;

	timer->fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (timer->fd < 0) {
		log_error("cannot create timerfd (%d): %m", errno);
		ret = -EFAULT;
		goto err_free;
	}

	ret = timerfd_settime(timer->fd, 0, spec, NULL);
	if (ret) {
		log_warn("cannot set timerfd (%d): %m", errno);
		ret = -EFAULT;
		goto err_close;
	}

	ret = ev_fd_new(&timer->efd, timer->fd, EV_READABLE, timer_cb, timer);
	if (ret)
		goto err_close;

	*out = timer;
	return 0;

err_close:
	close(timer->fd);
err_free:
	free(timer);
	return ret;
}

void ev_timer_ref(struct ev_timer *timer)
{
	if (!timer || !timer->ref)
		return;

	++timer->ref;
}

void ev_timer_unref(struct ev_timer *timer)
{
	if (!timer || !timer->ref || --timer->ref)
		return;

	ev_fd_unref(timer->efd);
	close(timer->fd);
	free(timer);
}

int ev_timer_enable(struct ev_timer *timer)
{
	if (!timer)
		return -EINVAL;

	return ev_fd_enable(timer->efd);
}

void ev_timer_disable(struct ev_timer *timer)
{
	if (!timer)
		return;

	ev_fd_disable(timer->efd);
}

bool ev_timer_is_enabled(struct ev_timer *timer)
{
	return timer && ev_fd_is_enabled(timer->efd);
}

bool ev_timer_is_bound(struct ev_timer *timer)
{
	return timer && ev_fd_is_bound(timer->efd);
}

void ev_timer_set_cb_data(struct ev_timer *timer, ev_timer_cb cb, void *data)
{
	if (!timer)
		return;

	timer->cb = cb;
	timer->data = data;
}

int ev_timer_update(struct ev_timer *timer, const struct itimerspec *spec)
{
	int ret;

	if (!timer || !spec)
		return -EINVAL;

	ret = timerfd_settime(timer->fd, 0, spec, NULL);
	if (ret) {
		log_warn("cannot set timerfd (%d): %m", errno);
		return -EFAULT;
	}

	return 0;
}

int ev_eloop_new_timer(struct ev_eloop *loop, struct ev_timer **out,
			const struct itimerspec *spec, ev_timer_cb cb,
			void *data)
{
	struct ev_timer *timer;
	int ret;

	if (!out || !loop)
		return -EINVAL;

	ret = ev_timer_new(&timer, spec, cb, data);
	if (ret)
		return ret;

	ret = ev_eloop_add_timer(loop, timer);
	if (ret) {
		ev_timer_unref(timer);
		return ret;
	}

	ev_timer_unref(timer);
	*out = timer;
	return 0;
}

int ev_eloop_add_timer(struct ev_eloop *loop, struct ev_timer *timer)
{
	int ret;

	if (!loop || !timer)
		return -EINVAL;

	if (ev_fd_is_bound(timer->efd))
		return -EALREADY;

	ret = ev_eloop_add_fd(loop, timer->efd);
	if (ret)
		return ret;

	ev_timer_ref(timer);
	return 0;
}

void ev_eloop_rm_timer(struct ev_timer *timer)
{
	if (!timer || !ev_fd_is_bound(timer->efd))
		return;

	ev_eloop_rm_fd(timer->efd);
	ev_timer_unref(timer);
}

/*
 * Counter Sources
 * Counter sources are a very basic event notification mechanism. It is based
 * around the eventfd() system call on linux machines. Internally, there is a
 * 64bit unsigned integer that can be increased by the caller. By default it is
 * set to 0. If it is non-zero, the event-fd will be notified and the
 * user-defined callback is called. The callback gets as argument the current
 * state of the counter and the counter is reset to 0.
 *
 * If the internal counter would overflow, an increase() fails silently so an
 * overflow will never occur, however, you may loose events this way. This can
 * be ignored when increasing with small values, only.
 */

static void counter_event(struct ev_fd *fd, int mask, void *data)
{
	struct ev_counter *cnt = data;
	int ret;
	uint64_t val;

	if (mask & (EV_HUP | EV_ERR)) {
		log_warning("HUP/ERR on eventfd");
		if (cnt->cb)
			cnt->cb(cnt, 0, cnt->data);
		return;
	}

	if (!(mask & EV_READABLE))
		return;

	ret = read(cnt->fd, &val, sizeof(val));
	if (ret < 0) {
		if (errno != EAGAIN) {
			log_warning("reading eventfd failed (%d): %m", errno);
			ev_counter_disable(cnt);
			if (cnt->cb)
				cnt->cb(cnt, 0, cnt->data);
		}
	} else if (ret == 0) {
		log_warning("EOF on eventfd");
		ev_counter_disable(cnt);
		if (cnt->cb)
			cnt->cb(cnt, 0, cnt->data);
	} else if (ret != sizeof(val)) {
		log_warning("read %d bytes instead of 8 on eventfd", ret);
		ev_counter_disable(cnt);
		if (cnt->cb)
			cnt->cb(cnt, 0, cnt->data);
	} else if (val > 0) {
		if (cnt->cb)
			cnt->cb(cnt, val, cnt->data);
	}
}

int ev_counter_new(struct ev_counter **out, ev_counter_cb cb, void *data)
{
	struct ev_counter *cnt;
	int ret;

	if (!out)
		return -EINVAL;

	cnt = malloc(sizeof(*cnt));
	if (!cnt)
		return -ENOMEM;
	memset(cnt, 0, sizeof(*cnt));
	cnt->ref = 1;
	cnt->cb = cb;
	cnt->data = data;

	cnt->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (cnt->fd < 0) {
		log_error("cannot create eventfd (%d): %m", errno);
		ret = -EFAULT;
		goto err_free;
	}

	ret = ev_fd_new(&cnt->efd, cnt->fd, EV_READABLE, counter_event, cnt);
	if (ret)
		goto err_close;

	*out = cnt;
	return 0;

err_close:
	close(cnt->fd);
err_free:
	free(cnt);
	return ret;
}

void ev_counter_ref(struct ev_counter *cnt)
{
	if (!cnt || !cnt->ref)
		return;

	++cnt->ref;
}

void ev_counter_unref(struct ev_counter *cnt)
{
	if (!cnt || !cnt->ref || --cnt->ref)
		return;

	ev_fd_unref(cnt->efd);
	close(cnt->fd);
	free(cnt);
}

int ev_counter_enable(struct ev_counter *cnt)
{
	if (!cnt)
		return -EINVAL;

	return ev_fd_enable(cnt->efd);
}

void ev_counter_disable(struct ev_counter *cnt)
{
	if (!cnt)
		return;

	ev_fd_disable(cnt->efd);
}

bool ev_counter_is_enabled(struct ev_counter *cnt)
{
	return cnt && ev_fd_is_enabled(cnt->efd);
}

bool ev_counter_is_bound(struct ev_counter *cnt)
{
	return cnt && ev_fd_is_bound(cnt->efd);
}

void ev_counter_set_cb_data(struct ev_counter *cnt, ev_counter_cb cb,
			    void *data)
{
	if (!cnt)
		return;

	cnt->cb = cb;
	cnt->data = data;
}

int ev_counter_inc(struct ev_counter *cnt, uint64_t val)
{
	int ret;

	if (!cnt || !val)
		return -EINVAL;

	if (val == 0xffffffffffffffffULL) {
		log_warning("increasing counter with invalid value %llu", val);
		return -EINVAL;;
	}

	ret = write(cnt->fd, &val, sizeof(val));
	if (ret < 0) {
		if (errno == EAGAIN)
			log_warning("eventfd overflow while writing %llu", val);
		else
			log_warning("eventfd write error (%d): %m", errno);
		return -EFAULT;
	} else if (ret != sizeof(val)) {
		log_warning("wrote %d bytes instead of 8 to eventdfd", ret);
		return -EFAULT;
	}

	return 0;
}

int ev_eloop_new_counter(struct ev_eloop *eloop, struct ev_counter **out,
			 ev_counter_cb cb, void *data)
{
	int ret;
	struct ev_counter *cnt;

	if (!eloop || !out)
		return -EINVAL;

	ret = ev_counter_new(&cnt, cb, data);
	if (ret)
		return ret;

	ret = ev_eloop_add_counter(eloop, cnt);
	if (ret) {
		ev_counter_unref(cnt);
		return ret;
	}

	ev_counter_unref(cnt);
	*out = cnt;
	return 0;
}

int ev_eloop_add_counter(struct ev_eloop *eloop, struct ev_counter *cnt)
{
	int ret;

	if (!eloop || !cnt)
		return -EINVAL;

	if (ev_fd_is_bound(cnt->efd))
		return -EALREADY;

	ret = ev_eloop_add_fd(eloop, cnt->efd);
	if (ret)
		return ret;

	ev_counter_ref(cnt);
	return 0;
}

void ev_eloop_rm_counter(struct ev_counter *cnt)
{
	if (!cnt || !ev_fd_is_bound(cnt->efd))
		return;

	ev_eloop_rm_fd(cnt->efd);
	ev_counter_unref(cnt);
}

/*
 * Shared signals
 */

int ev_eloop_register_signal_cb(struct ev_eloop *loop, int signum,
				ev_signal_shared_cb cb, void *data)
{
	struct ev_signal_shared *sig;
	int ret;
	struct kmscon_dlist *iter;

	if (!loop || signum < 0 || !cb)
		return -EINVAL;

	kmscon_dlist_for_each(iter, &loop->sig_list) {
		sig = kmscon_dlist_entry(iter, struct ev_signal_shared, list);
		if (sig->signum == signum)
			break;
	}

	if (iter == &loop->sig_list) {
		ret = signal_new(&sig, loop, signum);
		if (ret)
			return ret;
	}

	return kmscon_hook_add_cast(sig->hook, cb, data);
}

void ev_eloop_unregister_signal_cb(struct ev_eloop *loop, int signum,
					ev_signal_shared_cb cb, void *data)
{
	struct ev_signal_shared *sig;
	struct kmscon_dlist *iter;

	if (!loop)
		return;

	kmscon_dlist_for_each(iter, &loop->sig_list) {
		sig = kmscon_dlist_entry(iter, struct ev_signal_shared, list);
		if (sig->signum == signum) {
			kmscon_hook_rm_cast(sig->hook, cb, data);
			if (!kmscon_hook_num(sig->hook))
				signal_free(sig);
			return;
		}
	}
}

/*
 * Idle sources
 */

int ev_eloop_register_idle_cb(struct ev_eloop *eloop, ev_idle_cb cb,
			      void *data)
{
	int ret;

	if (!eloop)
		return -EINVAL;

	ret = kmscon_hook_add_cast(eloop->idlers, cb, data);
	if (ret)
		return ret;

	ret = ev_counter_inc(eloop->cnt, 1);
	if (ret)
		log_warning("cannot increase eloop idle-counter");

	return 0;
}

void ev_eloop_unregister_idle_cb(struct ev_eloop *eloop, ev_idle_cb cb,
				 void *data)
{
	if (!eloop)
		return;

	kmscon_hook_rm_cast(eloop->idlers, cb, data);
}
