/* -*- mode: C; c-file-style: "linux" -*- */

/* MemProf -- memory profiler and leak detector
 * Copyright 1999, 2000, 2001, Red Hat, Inc.
 * Copyright 2002, Kristian Rietveld
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
/*====*/

#define _GNU_SOURCE

#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "intercept.h"
#include "memintercept.h"
#include "memintercept-utils.h"
#include "stack-frame.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

/* The code in this file is written to work for threaded operation without
 * any reference to the thread library in operation. There are only
 * two places where any interaction between threads is necessary -
 * allocation of new slots in the pids[] array, and a parent thread
 * waiting for a child thread to start.
 */

typedef struct {
	uint32_t ref_count;
	pid_t pid;
	int outfd;
	pid_t clone_pid;	/* See comments in clone_thunk */
} ThreadInfo;

static void new_process (ThreadInfo *thread,
			 pid_t       old_pid,
			 MIOperation operation);
static void atexit_trap (void);

static int (*old_execve) (const char *filename,
			  char *const argv[],
			  char *const envp[]);
static int (*old_fork) (void);
static int (*old_vfork) (void);
static int (*old_clone) (int (*fn) (void *arg),
			 void *child_stack,
			 int flags,
			 void *arg);
static void (*old__exit) (int status);

static int initialized = MI_FALSE;
static MIBool tracing = MI_TRUE;

#define MAX_THREADS 128

static ThreadInfo threads[MAX_THREADS];
static char *socket_path = NULL;
static unsigned int seqno = 0;

#undef ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define MI_DEBUG(arg) mi_debug arg
#else /* !ENABLE_DEBUG */
#define MI_DEBUG(arg) (void)0
#endif /* ENABLE_DEBUG */

static ThreadInfo *
allocate_thread (pid_t pid)
{
	int i;
	
	for (i = 0; i < MAX_THREADS; i++) {
		if (threads[i].ref_count == 0) {
			unsigned int new_count = mi_atomic_increment (&threads[i].ref_count);
			if (new_count == 1) {
				threads[i].pid = pid;
				threads[i].clone_pid = 0;
				return &threads[i];
			} else
				mi_atomic_decrement (&threads[i].ref_count);
		}
	}
	
	mi_debug ("Can't find free thread slot");
	tracing = MI_FALSE;
	_exit(1);

	return NULL;
}

static void
release_thread (ThreadInfo *thread)
{
	thread->pid = 0;
	mi_atomic_decrement (&thread->ref_count);
}

static ThreadInfo *
find_thread (pid_t pid)
{
	int i;
#if 1
	ThreadInfo *thread;
#else
	/* Over-optimized GCC/GLibc extension happy version
	 * Requires gcc-3.2 and glibc-2.3.
	 */
	static __thread ThreadInfo *thread = NULL;
	int i;

a	if (__builtin_expect (thread == NULL, 0))
		return thread;
#endif	

	for (i=0; i < MAX_THREADS; i++)
		if (threads[i].pid == pid) {
			thread = &threads[i];

			if (thread->clone_pid) {
				/* See comments in clone_thunk() */
				new_process (thread, thread->clone_pid, MI_CLONE);
				thread->clone_pid = 0;
			}

			return thread;
		}

	mi_debug ("Thread not found\n");
	tracing = MI_FALSE;
	_exit(1);

	return NULL;
}

static void
initialize ()
{
	/* It's possible to get recursion here, since dlsym() can trigger
	 * memory allocation. To deal with this, we flag the initialization
	 * condition specially, then use the special knowledge that it's
	 * OK for malloc to fail during initialization (libc degrades
	 * gracefully), so we just return NULL from malloc(), realloc().
	 *
	 * This trick is borrowed from from libc's memusage.
	 */
	initialized = -1;
	old_execve = dlsym(RTLD_NEXT, "execve");
	old_fork = dlsym(RTLD_NEXT, "__fork");
	old_vfork = dlsym(RTLD_NEXT, "__vfork");
	old_clone = dlsym(RTLD_NEXT, "__clone");
	old__exit = dlsym(RTLD_NEXT, "_exit");

	atexit (atexit_trap);

	mi_init ();
	initialized = 1;
}

static void
abort_unitialized (const char *call)
{
	mi_printf (2, "MemProf: unexpected library call during initialization: %s\n", call);
	abort();
}

static void
stop_tracing (int fd)
{
	MI_DEBUG (("Stopping tracing\n"));

	tracing = MI_FALSE;
	close (fd);
	putenv ("_MEMPROF_SOCKET=");
}

static void
new_process (ThreadInfo *thread,
	     pid_t old_pid,
	     MIOperation operation)
{
	MIInfo info;
	struct sockaddr_un addr;
	int addrlen;
	char response;
	int outfd;
	int count;
	
	int old_errno = errno;
#ifdef ENABLE_DEBUG
	static const char *const operation_names[] = { NULL, NULL, NULL, NULL, "NEW", "FORK", "CLONE", NULL };
#endif	

	MI_DEBUG (("New process, operation = %s, old_pid = %d\n",
		   operation_names[operation], old_pid));

	memset (&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;
	strncpy (addr.sun_path, socket_path, sizeof (addr.sun_path));
	addrlen = sizeof(addr.sun_family) + strlen (addr.sun_path);
	if (addrlen > sizeof (addr))
		addrlen = sizeof(addr);

	outfd = socket (PF_UNIX, SOCK_STREAM, 0);
	if (outfd < 0) {
		mi_perror ("error creating socket");
		tracing = MI_FALSE;
		_exit(1);
	}
	
	if (connect (outfd, (struct sockaddr *)&addr, addrlen) < 0) {
		mi_perror ("Error connecting to memprof");
		tracing = MI_FALSE;
		_exit (1);
	}
	if (fcntl (outfd, F_SETFD, FD_CLOEXEC) < 0) {
		mi_perror ("error calling fcntl");
		tracing = MI_FALSE;
		_exit(1);
	}

	info.fork.operation = operation;

	info.fork.pid = old_pid;
	info.fork.new_pid = getpid();
	info.fork.seqno = 0;

	if (!thread)
		thread = allocate_thread (info.fork.new_pid);

	thread->outfd = outfd;

	if (!mi_write (outfd, &info, sizeof (MIInfo))) {
		stop_tracing (outfd);
		count = 0;
	} else {
		while (1) {
			count = read (outfd, &response, 1);
			if (count >= 0 || errno != EINTR)
				break;
		}
	}

	if (count != 1 || !response) {
		stop_tracing (outfd);
	}

	errno = old_errno;
}

static void 
memprof_init (void)
{
	int old_errno = errno;

	socket_path = getenv ("_MEMPROF_SOCKET");
	
	if (!socket_path) {
		mi_printf (2, "libmemintercept: must be used with memprof\n");
		exit (1);
	}

	MI_DEBUG (("_MEMPROF_SOCKET = %s\n", socket_path));
	
	if (socket_path[0] == '\0') /* tracing off */
		tracing = MI_FALSE;
	else
		new_process (NULL, 0, MI_NEW);

	errno = old_errno;
}

MIBool
mi_check_init (void)
{
	if (initialized <= 0) {
		if (initialized < 0)
			return MI_FALSE;

		initialize ();
	}
	
	if (!socket_path)
		memprof_init ();

	return MI_TRUE;
}

MIBool
mi_tracing (void)
{
	return tracing;
}

void
mi_write_stack (int      n_frames,
		void   **frames,
		void    *data)
{
	MIInfo *info = data;
	ThreadInfo *thread;
	int old_errno = errno;

	info->alloc.stack_size = n_frames;
	info->alloc.pid = getpid();
	info->alloc.seqno = seqno++;

	thread = find_thread (info->alloc.pid);
	
	if (!mi_write (thread->outfd, info, sizeof (MIInfo)) ||
	    !mi_write (thread->outfd, frames, n_frames * sizeof(void *)))
		stop_tracing (thread->outfd);

	errno = old_errno;
}

int
__fork (void)
{
	if (!mi_check_init ())
		abort_unitialized ("__fork");

	if (tracing) {
		int pid;
		int old_pid = getpid();

		find_thread (old_pid); /* Make sure we're registered */
		
		pid = (*old_fork) ();

		if (!pid) /* New child process */
			new_process (NULL, old_pid, MI_FORK);

		return pid;
	} else 
		return (*old_fork) ();
}

int
fork (void)
{
	return __fork ();
}

int
__vfork (void)
{
	if (!mi_check_init ())
		abort_unitialized ("__vfork");

	if (tracing) {
		int pid;
		int old_pid = getpid();

		find_thread (old_pid); /* Make sure we're registered */
		
		pid = (*old_vfork) ();

		if (!pid) /* New child process */
			new_process (NULL, old_pid, MI_FORK);

		return pid;
	} else 
		return (*old_vfork) ();
}

int
__execve (const char *filename,
	  char *const argv[],
	  char *const envp[])
{
	if (!mi_check_init ())
		abort_unitialized ("__execve");

	if (tracing) {
		/* Nothing */
	} else {
		int i;

		for (i=0; envp[i]; i++)
			if (strncmp (envp[i], "_MEMPROF_SOCKET=", 16) == 0)
				envp[i][16] = '\0';
	}
	return (*old_execve) (filename, argv, envp);
}

typedef struct 
{
	int started;
	int (*fn) (void *);
	void *arg;
	pid_t pid;
} CloneData;

static int
clone_thunk (void *arg)
{
	ThreadInfo *thread;
	CloneData *data = arg;
	int (*fn)(void *) = data->fn;
	void *sub_arg = data->arg;

	/* At this point, we can't call new_process(), because errno
	 * still points to our parent's errno structure. (We assume
	 * getpid() is safe, but about anythhing else could be dangerous.)
	 * So, we simply allocate the structure for the thread and delay
	 * the initialization.
	 */
	thread = allocate_thread (getpid());
	thread->clone_pid = data->pid;
	data->started = 1;

	return (*fn) (sub_arg);
}

int __clone (int (*fn) (void *arg),
	     void *child_stack,
	     int   flags,
	     void *arg)
{
	volatile CloneData data;
	int pid;

	if (!mi_check_init ())
		abort_unitialized ("__clone");

	if (tracing) {
		data.started = 0;
		data.fn = fn;
		data.arg = arg;
		data.pid = getpid();
		
		find_thread (data.pid); /* Make sure we're registered */
		
		pid = (*old_clone) (clone_thunk, child_stack, flags, (void *)&data);

		while (!data.started)
			/* Wait */;

		MI_DEBUG (("Clone: child=%d\n", pid));

		return pid;
	} else
		return (*old_clone) (fn, child_stack, flags, arg);
}

int clone (int (*fn) (void *arg),
	   void *child_stack,
	   int   flags,
	   void *arg)
{
	return __clone (fn, child_stack, flags, arg);
}

static void
exit_wait (void)
{
	MIInfo info;
	ThreadInfo *thread;
	int count;
	char response;
	info.any.operation = MI_EXIT;
	info.any.seqno = seqno++;
	info.any.pid = getpid();
	
	mi_stop ();
	
	thread = find_thread (info.any.pid);
	
	if (mi_write (thread->outfd, &info, sizeof (MIInfo)))
		/* Wait for a response before really exiting
		 */
		while (1) {
			count = read (thread->outfd, &response, 1);
			if (count >= 0 || errno != EINTR)
				break;
		}
	
	close (thread->outfd);
	thread->pid = 0;
	release_thread (thread);
}

/* Because _exit() isn't interposable in recent versions of
 * GNU libc, we can't depend on this, and instead use the less-good
 * atexit_trap() below. But we leave this here just in case we
 * are using an old version of libc where _exit() is interposable,
 * so we can trap a wider range of exit conditions
 */
void
_exit (int status)
{
	if (initialized <= 0)
		abort_unitialized ("_exit");

	MI_DEBUG (("_Exiting\n"));
	
	if (tracing) {
		exit_wait ();
		tracing = 0;
	}

	(*old__exit) (status);
}

static void
atexit_trap (void)
{
	if (tracing) {
		exit_wait ();
		tracing = 0;
	}
}

static void construct () __attribute__ ((constructor));

static void construct () 
{
	mi_check_init ();
	mi_start ();
}
