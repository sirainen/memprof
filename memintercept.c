#define MALLOC_HOOKS

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "memintercept.h"

static void *(*old_malloc_hook) (size_t);
static void *(*old_realloc_hook) (void *ptr, size_t);
static void (*old_free_hook) (void *ptr);

static void *my_malloc_hook (size_t);
static void *my_realloc_hook (void *ptr, size_t);
static void my_free_hook (void *ptr);

int outfd = -1;

static void 
memprof_init ()
{
  char *fd_name = getenv ("_MEMPROF_FD");

  if (!fd_name)
    {
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

#define STACK_INCREMENT 512

static void
stack_trace (MIInfo *info)
{
  int n = 0;
  void **sp;
  static void **stack_buffer = NULL;
  static int stack_size = 0;

  /* Stack frame is:
   * (0) pointer to previous stack frame
   * (1) calling function address
   * (2) first argument
   * (3) ...
   */
  sp = (void **)&info - 2;

  while (sp)
    {
      /* Skip over __libc_malloc and hook */
      if (n > 1)
	{
	  if (n -1 >= stack_size)
	    {
	      stack_size += STACK_INCREMENT;
	      stack_buffer = realloc (stack_buffer, stack_size * sizeof(void *));
	    }
	  stack_buffer[n - 2] = *(sp + 1);
	}
      sp = *sp;
      n++;
    }

  info->stack_size = n - 2;
  info->pid = getpid();

  write (outfd, info, sizeof (*info));
  write (outfd, stack_buffer, info->stack_size * sizeof(void *));
}

static void *
my_malloc_hook (size_t size)
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
my_realloc_hook (void *ptr, size_t size)
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
my_free_hook (void *ptr)
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
