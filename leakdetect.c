#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgnome/libgnome.h>	/* For i18n */

#include "memprof.h"
#include "process.h"

static void
prepare_block (gpointer key, gpointer value, gpointer data)
{
  Block *block = value;
  GPtrArray *arr = data;

  block->flags &= ~BLOCK_MARKED;
  g_ptr_array_add (arr, block);
}

static gint
compare_blocks (const void *a, const void *b)
{
  Block * const *b1 = a;
  Block * const *b2 = b;

  return ((*b1)->addr < (*b2)->addr) ? -1 : 
    ((*b1)->addr == (*b2)->addr ? 0 : 1);
}

static gboolean
read_proc_stat (int pid, char *status, guint *start_stack, guint *end_stack)
{
  gchar *fname;
  gulong tstart_stack;
  gulong tend_stack;
  char tstatus;
  FILE *in;
  
  fname = g_strdup_printf ("/proc/%d/stat", pid);
  in = fopen (fname, "r");
  if (!in)
    {
      g_warning ("Can't open %s\n", fname);
      return FALSE;
    }
  g_free (fname);

/* The fields in proc/stat are:
 * 		pid 
 * 		tsk->comm 
 * 		state 
 * 		tsk->p_pptr->pid 
 * 		tsk->pgrp 
 * 		tsk->session 
 * 	        tsk->tty ? kdev_t_to_nr(tsk->tty->device) : 0 
 * 		tty_pgrp 
 * 		tsk->flags 
 * 		tsk->min_flt 

 * 		tsk->cmin_flt 
 * 		tsk->maj_flt 
 * 		tsk->cmaj_flt 
 * 		tsk->utime 
 * 		tsk->stime 
 * 		tsk->cutime 
 * 		tsk->cstime 
 * 		priority 
 * 		nice 
 * 		tsk->timeout 

 * 		tsk->it_real_value 
 * 		tsk->start_time 
 * 		vsize 
 * 		tsk->mm ? tsk->mm->rss : 0 
 * 		tsk->rlim ? tsk->rlim[RLIMIT_RSS].rlim_cur : 0 
 * 		tsk->mm ? tsk->mm->start_code : 0 
 * 		tsk->mm ? tsk->mm->end_code : 0 
 * 		tsk->mm ? tsk->mm->start_stack : 0 
 * 		esp 
 * 		eip 

 * 		tsk->signal 
 * 		tsk->blocked 
 * 		sigignore 
 * 		sigcatch 
 * 		wchan 
 * 		tsk->nswap 
 * 		tsk->cnswap); 
  if (fscanf(in,"%*d %*s %c %*d %*d %*d %*d %*d %*lu %*lu \
%*lu %*lu %*lu %*lu %*lu %*ld %*ld %*ld %*ld %*ld \
%*ld %*lu %*lu %*ld %*lu %*lu %*lu %lu %lu %*lu \
%*lu %*lu %*lu %*lu %*lu %*lu %*lu",
*/
  if (fscanf(in,"%*d %*s %c %*d %*d %*d %*d %*d %*u %*u \
%*u %*u %*u %*u %*u %*d %*d %*d %*d %*d \
%*d %*u %*u %*d %*u %*u %*u %lu %lu %*u \
%*u %*u %*u %*u %*u %*u %*u",
	     &tstatus,
	     &tstart_stack,
	     &tend_stack) != 3)
    {
      g_warning ("Error parsing /proc/%d/stat\n", pid);
      return FALSE;
    }

  if (status)
    *status = tstatus;
  if (start_stack)
    *start_stack = tstart_stack;
  if (end_stack)
    *end_stack = tend_stack;

  fclose (in);

  return TRUE;
}

static GSList *
add_stack_roots (MPProcess *process, GSList *block_list)
{
  Block *block;
  guint start_stack, end_stack;

  /* Add stack root */

  if (!read_proc_stat (process->pid, NULL, &start_stack, &end_stack))
    return block_list;

  block = g_new (Block, 1);
  block->flags = BLOCK_IS_ROOT;
  block->addr = (void *)end_stack;
  block->size = start_stack - end_stack;
  block->stack_size = 0;
  block->stack = NULL;

  return g_slist_prepend (block_list, block);
}

void
process_data_root (void *addr, guint size, gpointer user_data)
{
  GSList **block_list = user_data;
  Block *block;
  
  block = g_new (Block, 1);
  block->flags = BLOCK_IS_ROOT;
  block->addr = addr;
  block->size = size;
  block->stack_size = 0;
  block->stack = NULL;

  *block_list = g_slist_prepend (*block_list, block);
}

static GSList *
add_data_roots (MPProcess *process, GSList *block_list)
{
  process_sections (process, process_data_root, &block_list);

  return block_list;
}

static Block *
find_block (GPtrArray *block_arr, void *addr)
{
  Block **data;
  guint first, middle, last;

  first = 0;
  last = block_arr->len - 1;
  middle = last;

  data = (Block **)block_arr->pdata;

  if (addr < data[first]->addr)
    {
      return NULL;
    }
  else if (addr < data[last]->addr)
    {
      /* Invariant: data[first].addr <= val < data[last].addr */

      while (first < last - 1)
	{
	  middle = (first + last) / 2;
	  if (addr < data[middle]->addr) 
	    last = middle;
	  else
	    first = middle;
	}
      if (addr < data[first]->addr + data[first]->size)
	return data[first];
      else
	return NULL;
    }
  else
    {
      if (addr < data[last]->addr + data[last]->size)
	return data[last];
      else
	return NULL;
    }
}

static GSList *
scan_block_contents (GPtrArray *block_arr,
		     GSList *block_list,
		     Block *block, void **mem)
{
  int i;
  
  for (i=0; i < block->size / sizeof(void *); i++)
    {
      Block *new_block;

      new_block = find_block (block_arr, mem[i]);

      if (new_block && !(new_block->flags & BLOCK_MARKED))
	{
	  block_list = g_slist_prepend (block_list, new_block);
	  new_block->flags |= BLOCK_MARKED;
	}
    }

  return block_list;
}

static GSList *
scan_block (pid_t pid, int memfd, GSList *block_list,
	    GPtrArray *block_arr, Block *block)
{
  void **mem;
  gint i;
  void *addr;
  size_t length = (block->size + 3) / 4;

  addr = g_new (void *, length);
  mem = (void **)addr;

  for (i = 0; i < length; i++)
    {
      mem[i] = (void *)ptrace (PTRACE_PEEKDATA, pid,
			       block->addr+i*sizeof(void *),
			       &mem[i]);
      if (errno)
	{
	  g_warning ("Cannot read word %d/%d in block %p: %s\n",
		     i, length, block->addr, g_strerror (errno));
	  g_free (addr);
	  return block_list;
	}
    }
  
  block_list = scan_block_contents (block_arr, block_list,
				    block, mem);

  g_free (addr);
  return block_list;
}

#if 0
static GSList *
scan_block (int memfd, GSList *block_list,
	    GPtrArray *block_arr, Block *block)
{
  void **mem;
  gint i;
  gint count;
  void *addr;

  addr = mmap (NULL, block->size, PROT_READ, MAP_SHARED, memfd, (off_t)block->addr);
  if (addr == (void *)-1)
    {
      g_warning ("Cannot mmap block %p: %s\n", block->addr,
		 g_strerror (errno));
      return block_list;
    }

  mem = (void **)addr;
  
#if 0  
  if (lseek (memfd, (guint)block->addr, SEEK_SET) == (off_t)-1)
    {
      g_warning ("Error seeking to block at %p: %s\n", block->addr,
		 g_strerror (errno));
      goto out;
    }

  count = read (memfd, (char *)mem, block->size);
  if (count < 0)
    {
      g_warning ("Error reading block at %p: %s\n", block->addr,
		 g_strerror (errno));
      goto out;
    }
  else if (count != block->size)
    g_warning ("Short read for block at %p (%u/%u)\n",
	       block->addr, count, block->size);
#endif

  block_list = scan_block_contents (block_arr, block_list,
				    block, addr);

  munmap (addr, block->size);

  return block_list;
}

static GSList *
scan_block (int memfd, GSList *block_list,
	    GPtrArray *block_arr, Block *block)
{
  void **mem;
  gint i;
  gint count;
  void *addr;

  addr = mmap (NULL, block->size, PROT_READ, MAP_SHARED, memfd, (off_t)block->addr);
  if (addr == (void *)-1)
    {
      g_warning ("Cannot mmap block %p: %s\n", block->addr,
		 g_strerror (errno));
      return block_list;
    }

  mem = (void **)addr;
  
  if (lseek (memfd, (guint)block->addr, SEEK_SET) == (off_t)-1)
    {
      g_warning ("Error seeking to block at %p: %s\n", block->addr,
		 g_strerror (errno));
      goto out;
    }

  count = read (memfd, (char *)mem, block->size);
  if (count < 0)
    {
      g_warning ("Error reading block at %p: %s\n", block->addr,
		 g_strerror (errno));
      goto out;
    }
  else if (count != block->size)
    g_warning ("Short read for block at %p (%u/%u)\n",
	       block->addr, count, block->size);

  block_list = scan_block_contents (block_arr, block_list,
				    block, addr);

  g_free (addr);
  return block_list;
}
#endif

GSList *
leaks_find (MPProcess *process)
{
  int i;
  GPtrArray *block_arr;
  GSList *block_list = NULL;
  GSList *result = NULL;
  int memfd;
  gchar *fname;
  int status;

  ptrace (PTRACE_ATTACH, process->pid, 0, 0);
  kill (process->pid, SIGSTOP);

  /* Wait for the process we are tracing to actually stop */
  waitpid(process->pid, &status, WUNTRACED);
  if (!WIFSTOPPED (status))
    {
      g_warning ("Subprocess exited unexpectedly");
      return NULL;
    }
    
  fname = g_strdup_printf ("/proc/%d/mem", process->pid);
  memfd = open (fname, O_RDONLY);
  if (memfd < 0)
    {
      g_warning ("Can't open %s\n", fname);
      return NULL;
    }
  
  g_free (fname);

  /* Mark all blocks as untouched, add them to list of blocks
   */

  block_arr = g_ptr_array_new ();
  g_hash_table_foreach (process->block_table, prepare_block, block_arr);

  qsort (block_arr->pdata, block_arr->len, sizeof (Block *),
	 compare_blocks);
  
  /* Locate all the roots
   */

  block_list = add_stack_roots (process, block_list);
  block_list = add_data_roots (process, block_list);

  /* While there are blocks to check, scan each block,
   * and add each new-found block to the global list.
   */

  while (block_list)
    {
      GSList *tmp_list;
      Block *block = block_list->data;

      tmp_list = block_list->next;
      g_slist_free_1 (block_list);
      block_list = tmp_list;

      block_list = scan_block (process->pid, memfd, block_list, block_arr, block);

#if 0
      if (block->flags & BLOCK_IS_ROOT)
	g_free (block);
#endif      
    }

  close (memfd);

  /* Look for leaked blocks
   */

  for (i=0; i<block_arr->len; i++)
    {
      Block *block = block_arr->pdata[i];
      if (!(block->flags & BLOCK_MARKED))
	result = g_slist_prepend (result, block);
    }

  /* Clean up
   */

  close (memfd);
  kill (process->pid, SIGCONT);
  ptrace (PTRACE_DETACH, process->pid, 0, 0);

  return result;
}

void
leaks_print (MPProcess *process, GSList *blocks, gchar *outfile)
{
  GSList *tmp_list = blocks;
  FILE *out;

  out = fopen (outfile, "w");
  if (!out)
    {
      show_error (ERROR_MODAL, _("Cannot open output file: %s\n"), g_strerror (errno));
      return;
    }

  while (tmp_list)
    {
      Block *block = tmp_list->data;
      
      fprintf (out, "Leaked %p (%u bytes)\n", block->addr, block->size);
      process_dump_stack (process, out, block->stack_size, block->stack);

      tmp_list = tmp_list->next;
    }
  
  fclose (out);
}
