/* -*- mode: C; c-file-style: "linux" -*- */

/* MemProf -- memory profiler and leak detector
 * Copyright (C) 1999 Red Hat, Inc.
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

#define MALLOC_HOOKS

#include <limits.h>            /* For PIPE_BUF */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "memintercept.h"

static void *(*old_malloc_hook) (size_t, const void *);
static void *(*old_realloc_hook) (void *ptr, size_t, const void *);
static void (*old_free_hook) (void *ptr, const void *);

static void *my_malloc_hook (size_t, const void *);
static void *my_realloc_hook (void *ptr, size_t, const void *);
static void my_free_hook (void *ptr, const void *);

int outfd = -1;

static void 
memprof_init ()
{
	char *fd_name = getenv ("_MEMPROF_FD");
	
	if (!fd_name) {
		fprintf(stderr, "libmemintercept: must be used with memprof\n");
		exit(1);
	}

	outfd = atoi(fd_name);
}

static void
set_hooks (void)
{
	__malloc_hook = my_malloc_hook;
	__realloc_hook = my_realloc_hook;
	__free_hook = my_free_hook;
}

static void
restore_hooks (void)
{
	__malloc_hook = old_malloc_hook;
	__realloc_hook = old_realloc_hook;
	__free_hook = old_free_hook;
}

#define STACK_MAX_SIZE ((PIPE_BUF - sizeof (MIInfo)) / sizeof(void *))

static void
stack_trace (MIInfo *info)
{
	int n = 0;
	void **sp;
	static char outbuf[PIPE_BUF];
	static int outbuf_size = 0;
	void **stack_buffer = NULL;
	
	MIInfo *outinfo;
	
	outinfo = outbuf;
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
			fprintf (stderr, "Stack to large for atomic write, truncating!\n");
			break;
		/* Skip over __libc_malloc and hook */
		} else if (n > 1) {
			stack_buffer[n - 2] = *(sp + 1);
		}
		sp = *sp;
		n++;
	}

	outinfo->stack_size = n - 2;
	outinfo->pid = getpid();
	
	write (outfd, outbuf, sizeof (MIInfo) + outinfo->stack_size * sizeof(void *));
}

static void *
my_malloc_hook (size_t size, const void *caller)
{
	void *result;
	MIInfo info;
	
	restore_hooks();
	
	if (outfd < 0)
		memprof_init();
	
	result = malloc (size);
	
	info.operation = MI_MALLOC;
	info.old_ptr = NULL;
	info.new_ptr = result;
	info.size = size;
	
	stack_trace (&info);
	
	set_hooks();
	
	return result;
}

static void *
my_realloc_hook (void *ptr, size_t size, const void *caller)
{
	void *result;
	MIInfo info;
  
	restore_hooks();
	
	if (outfd < 0)
		memprof_init();
	
	result = realloc (ptr, size);
	
	info.operation = MI_REALLOC;
	info.old_ptr = ptr;
	info.new_ptr = result;
	info.size = size;
	
	stack_trace (&info);
	
	set_hooks();
	
	return result;
}
     
static void
my_free_hook (void *ptr, const void *caller)
{
	MIInfo info;

	restore_hooks();

	if (outfd < 0)
		memprof_init();

	free (ptr);
	
	info.operation = MI_FREE;
	info.old_ptr = ptr;
	info.new_ptr = NULL;
	info.size = 0;
	
	stack_trace (&info);
	
	set_hooks();
}

static void initialize () __attribute__ ((constructor));

static void initialize () 
{
	old_malloc_hook = __malloc_hook;
	old_realloc_hook = __realloc_hook;
	old_free_hook = __free_hook;
	
	set_hooks();
}

