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

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "memintercept.h"

#include <pthread.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

static int initialized = 0;

static int (*old_execve) (const char *filename,
			  char *const argv[],
			  char *const envp[]);
static int (*old_fork) (void);
static int (*old_vfork) (void);
static int (*old_clone) (int (*fn) (void *arg),
			 void *child_stack,
			 int flags,
			 void *arg);
static void *(*old_malloc) (size_t size);
static void *(*old_calloc) (size_t nmemb, size_t size);
static void *(*old_memalign) (size_t boundary, size_t size);
static void * (*old_realloc) (void *ptr, size_t size);
static void (*old_free) (void *ptr);
static void (*old__exit) (int status);

#define MAX_THREADS 128

static pthread_mutex_t malloc_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int tracing = 1;

static int pids[MAX_THREADS];
static int outfds[MAX_THREADS];
static char *socket_path = NULL;
static unsigned int seqno = 0;

#define STARTER_SIZE 1024
static char starter_mem[STARTER_SIZE];
int starter_alloced = 0;
int starter_last = 0;

/* #define DEBUG */

#define MI_LOCK() pthread_mutex_lock (&malloc_mutex);
#define MI_UNLOCK() pthread_mutex_unlock (&malloc_mutex);

static void
abort_unitialized (const char *call)
{
	static const char msg[] = "MemProf: unexpected library call during initialization: ";
	
	write (2, msg, sizeof(msg));
	write (2, call, strlen (call));
	write (2, "\n", 1);
	abort();
}

#ifdef DEBUG
static void
write_num (unsigned long num)
{
	char buffer[64];
	unsigned long tmp;
	char c;
	int i, n;

	if (!num) {
		write (2, "0", 1);
		return;
	} 

	write (2, "0x", 2);
	
	n = 0;
	tmp = num;
	while (tmp) {
		tmp /= 16;
		n++;
	}

	i = n;
	while (num) {
		i--;
		c = (num % 16);
		if (c < 10)
			buffer[i] = c + '0';
		else
			buffer[i] = c + 'a' - 10;
		num /= 16;
	}

	write (2, buffer, n);
}
#endif /* DEBUG */

static int
write_all (int fd, void *buf, int total)
{
	int count;
	int written = 0;

	while (written < total) {
		/* Use send() to avoid EPIPE errors */
		count = send (fd, buf + written, total - written, MSG_NOSIGNAL);
		if (count < 0) {
			if (errno != EINTR)
				goto error;
		} else {
			if (count == 0)
				goto error;
			written += count;
		}
	}

	return 1;

 error:
	tracing = 0;
	close (fd);
	putenv ("_MEMPROF_SOCKET=");
	return 0;
}

static void
new_process (pid_t old_pid, MIOperation operation)
{
	MIInfo info;
	struct sockaddr_un addr;
	int addrlen;
	char response;
	int outfd;
	int i, count;
	int old_errno = errno;

	memset (&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;
	strncpy (addr.sun_path, socket_path, sizeof (addr.sun_path));
	addrlen = sizeof(addr.sun_family) + strlen (addr.sun_path);
	if (addrlen > sizeof (addr))
		addrlen = sizeof(addr);

	outfd = socket (PF_UNIX, SOCK_STREAM, 0);
	if (outfd < 0) {
			write (2, "FRUG", 4);
			_exit(1);
	}
	
	if (connect (outfd, (struct sockaddr *)&addr, addrlen) < 0) {
		fprintf (stderr, "Error connecting to memprof: %s!\n",
			 strerror (errno));
		_exit (1);
	}
	if (fcntl (outfd, F_SETFD, FD_CLOEXEC) < 0) {
			write (2, "FRAG", 4);
			_exit(1);
	}

	info.fork.operation = operation;

	info.fork.pid = old_pid;
	info.fork.new_pid = getpid();
	info.fork.seqno = 0;

	while (1) {
		for (i = 0; outfds[i] && i < MAX_THREADS; i++)
			/* nothing */;
		outfds[i] = outfd;
		if (outfds[i] == outfd)
			break;
	}
	pids[i] = info.fork.new_pid;

	if (!write_all (outfd, &info, sizeof (MIInfo)))
		count = 0;
	else {
		while (1) {
			count = read (outfd, &response, 1);
			if (count >= 0 || errno != EINTR)
				break;
		}
	}

	if (count != 1 || !response) {
		/* Stop tracing */
		tracing = 0;
		close (outfd);
		putenv ("_MEMPROF_SOCKET=");
	}

	errno = old_errno;
}

static void 
memprof_init ()
{
	int old_errno = errno;
	
	socket_path = getenv ("_MEMPROF_SOCKET");
	
	if (!socket_path) {
		fprintf(stderr, "libmemintercept: must be used with memprof\n");
		exit(1);
	}

	if (socket_path[0] == '\0') /* tracing off */
		tracing = 0;
	else
		new_process (0, MI_NEW);

	errno = old_errno;
}

#define OUT_BUF_SIZE 4096
#define STACK_MAX_SIZE ((OUT_BUF_SIZE - sizeof (MIInfo)) / sizeof(void *))

static void
stack_trace (MIInfo *info)
{
	int n = 0;
	void **sp;
	char outbuf[OUT_BUF_SIZE];
	void **stack_buffer = NULL;
	int i;
	int old_errno = errno;
	
	MIInfo *outinfo;
	
	outinfo = (MIInfo *)outbuf;
	memcpy (outbuf, info, sizeof(MIInfo));
	stack_buffer = (void **)(outbuf + sizeof(MIInfo));
	
	/* Stack frame is:
	 * (0) pointer to previous stack frame
	 * (1) calling function address
	 * (2) first argument
	 * (3) ...
	 */
	sp = (void **)&info - 2;
	
	while (sp) {
		if (n - 2 == STACK_MAX_SIZE) {
			fprintf (stderr, "Stack too large for atomic write, truncating!\n");
			break;
		/* Skip over __libc_malloc and hook */
		} else if (n > 1) {
			stack_buffer[n - 2] = *(sp + 1);
		}
		sp = *sp;
		n++;
	}

	outinfo->alloc.stack_size = n - 2;
	outinfo->alloc.pid = getpid();
	outinfo->alloc.seqno = seqno++;

	for (i=0; pids[i] && i<MAX_THREADS; i++)
		if (pids[i] == outinfo->alloc.pid)
			break;

	if (i == MAX_THREADS) {
		write (2, "ARGH", 4);
	}

	write_all (outfds[i], outbuf, sizeof (MIInfo) + outinfo->alloc.stack_size * sizeof(void *));

	errno = old_errno;
}

void *
__libc_malloc (size_t size)
{
	void *result;
	MIInfo info;

	if (!old_malloc) {
		/* oh s*** we are being called at initialization and can't
		 * depend upon anything!
		 */
		size = (size + 3) & ~3;
		if (starter_alloced + size > STARTER_SIZE) {
			static const char msg[] = "MemProf: Starter malloc exceeded available space\n";
			write (2, msg, sizeof(msg));
			abort ();
		} else {
			result = starter_mem + starter_alloced;
			starter_last = starter_alloced;
			starter_alloced += size;
		}

#ifdef DEBUG
		{
			static const char msg[] = "Starter malloc: ";
		
			write (2, msg, sizeof (msg));
			write_num ((unsigned long)result);
			write (2, " (", 2);
			write_num (size);
			write (2, ")\n", 2);
		}
#endif /* DEBUG	*/
		
		return result;
	}
	
	MI_LOCK ();

	if (!socket_path)
		memprof_init();

	result = (*old_malloc) (size);

	if (tracing) {
		info.alloc.operation = MI_MALLOC;
		info.alloc.old_ptr = NULL;
		info.alloc.new_ptr = result;
		info.alloc.size = size;
	
		stack_trace (&info);
	}
		
	MI_UNLOCK ();
	
	return result;
}

void *
malloc (size_t size)
{
	return __libc_malloc (size);
}

void *
__libc_memalign (size_t boundary, size_t size)
{
	void *result;
	MIInfo info;

	if (!initialized)
		abort_unitialized ("memalign");
	
	MI_LOCK ();
	
	if (!socket_path)
		memprof_init();

	result = (*old_memalign) (boundary, size);

	if (tracing) {
		info.alloc.operation = MI_MALLOC;
		info.alloc.old_ptr = NULL;
		info.alloc.new_ptr = result;
		info.alloc.size = size;
	
		stack_trace (&info);
	}

	MI_UNLOCK ();
	
	return result;
}

void *
memalign (size_t boundary, size_t size)
{
	return __libc_memalign (boundary, size);
}

void *
__libc_calloc (size_t nmemb, size_t size)
{
	int total = nmemb * size;
	void *result = __libc_malloc (total);
	memset (result, 0, total);
	
	return result;
}

void *
calloc (size_t nmemb, size_t size)
{
	return __libc_calloc (nmemb, size);
}

void *
__libc_realloc (void *ptr, size_t size)
{
	void *result;
	MIInfo info;

	if (!initialized)
		abort_unitialized ("realloc");
	
	MI_LOCK ();

	if (!socket_path)
		memprof_init();
	
	result = (*old_realloc) (ptr, size);

	if (tracing) {
		info.alloc.operation = MI_REALLOC;
		info.alloc.old_ptr = ptr;
		info.alloc.new_ptr = result;
		info.alloc.size = size;
	
		stack_trace (&info);
	}

	MI_UNLOCK ();
	
	return result;
}
     
void *
realloc (void *ptr, size_t size)
{
	return __libc_realloc (ptr, size);
}

void
__libc_free (void *ptr)
{
	MIInfo info;

	if ((ptr >= (void *)starter_mem &&
	     ptr < (void *)(starter_mem + starter_alloced))) {
		/* Freeing memory allocated from starter pool */
#ifdef DEBUG		
		static const char msg[] = "Starter free: ";
		
		write (2, msg, sizeof (msg));
		write_num ((unsigned long)ptr);
#endif /* DEBUG */

		if (ptr == starter_mem + starter_last)
			starter_alloced = starter_last;
#ifdef DEBUG
		else
			write (2, " (ignored)", 10);
		write (2, "\n", 1);
#endif /* DEBUG	*/
		return;
	}

	if (!initialized)
		abort_unitialized ("free");
	
	MI_LOCK ();
	
	if (!socket_path)
		memprof_init();

	(*old_free) (ptr);

	if (tracing) {
		info.alloc.operation = MI_FREE;
		info.alloc.old_ptr = ptr;
		info.alloc.new_ptr = NULL;
		info.alloc.size = 0;

		stack_trace (&info);
	}

	MI_UNLOCK ();
}

void
free (void *ptr)
{
	__libc_free (ptr);
}

int
__fork (void)
{
	if (!initialized)
		abort_unitialized ("__fork");
	
	if (tracing) {
		int pid;
		int old_pid = getpid();
		
		pid = (*old_fork) ();

		if (!pid) /* New child process */
			new_process (old_pid, MI_FORK);

		return pid;
	} else 
		return (*old_fork) ();
}

int
__vfork (void)
{
	if (!initialized)
		abort_unitialized ("__vfork");
	
	if (tracing) {
		int pid;
		int old_pid = getpid();
		
		pid = (*old_vfork) ();

		if (!pid) /* New child process */
			new_process (old_pid, MI_FORK);

		return pid;
	} else 
		return (*old_vfork) ();
}

int
execve (const char *filename,
	char *const argv[],
	char *const envp[])
{
	if (!initialized)
		abort_unitialized ("execve");
	
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

/* Some scary primitive threading support */

typedef struct 
{
	int started;
	int (*fn) (void *);
	void *arg;
	pid_t pid;
} CloneData;

int clone_thunk (void *arg)
{
	CloneData *data = arg;
	int (*fn)(void *) = data->fn;
	void *sub_arg = data->arg;

	new_process (data->pid, MI_CLONE);
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

	if (!initialized)
		abort_unitialized ("clone");
	
	if (tracing) {
		data.started = 0;
		data.fn = fn;
		data.arg = arg;
		data.pid = getpid();
		
		pid = (*old_clone) (clone_thunk, child_stack, flags, (void *)&data);

		while (!data.started)
			/* Wait */;

		return pid;
	} else
		return (*old_clone) (fn, child_stack, flags, arg);
}

void
_exit (int status)
{
	if (!initialized)
		abort_unitialized ("exit");
	
	if (tracing) {
		MIInfo info;
		int i;
		int count;
		char response;
		info.any.operation = MI_EXIT;
		info.any.seqno = seqno++;
		info.any.pid = getpid();
		
		for (i=0; pids[i] && i<MAX_THREADS; i++)
			if (pids[i] == info.any.pid)
				break;

		if (write_all (outfds[i], &info, sizeof (MIInfo)))
			/* Wait for a response before really exiting
			 */
			while (1) {
				count = read (outfds[i], &response, 1);
				if (count >= 0 || errno != EINTR)
					break;
			}

		close (outfds[i]);
		
	}
	
	(*old__exit) (status);
}

static void initialize () __attribute__ ((constructor));

static void initialize () 
{
	old_malloc = dlsym(RTLD_NEXT, "__libc_malloc");
	old_realloc = dlsym(RTLD_NEXT, "__libc_realloc");
	old_free = dlsym(RTLD_NEXT, "__libc_free");
	old_calloc = dlsym(RTLD_NEXT, "__libc_calloc");
	old_memalign = dlsym(RTLD_NEXT, "__libc_memalign");
	old_execve = dlsym(RTLD_NEXT, "execve");
	old_fork = dlsym(RTLD_NEXT, "__fork");
	old_vfork = dlsym(RTLD_NEXT, "__vfork");
	old_clone = dlsym(RTLD_NEXT, "__clone");
	old__exit = dlsym(RTLD_NEXT, "_exit");

	initialized = 1;
}
