#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <gtk/gtk.h>
#include <libgnome/libgnome.h>

#include "memintercept.h"
#include "memprof.h"
#include "process.h"

typedef struct {
  dev_t device;
  ino_t inode;
  gchar *name;
} Inode;

GHashTable *inode_table = NULL;
char *lib_location = NULL;

#define PAGE_SIZE 4096

/************************************************************
 * Inode finding code - not needed for kernel 2.2 or greater
 ************************************************************/

static guint
inode_hash (gconstpointer data)
{
  return (((Inode *)data)->device + (((Inode *)data)->inode << 11));
}

static gint
inode_compare (gconstpointer a, gconstpointer b)
{
  return ((((Inode *)a)->device == ((Inode *)b)->device) &&
	  (((Inode *)a)->inode == ((Inode *)b)->inode));
}

static void
read_inode (gchar *path)
{
  struct stat stbuf;

  g_return_if_fail (path != NULL);

  if (!inode_table)
    inode_table = g_hash_table_new (inode_hash, inode_compare);

  if (!stat (path, &stbuf))
    {
      Inode *inode = g_new (Inode, 1);
      inode->device = stbuf.st_dev;
      inode->inode = stbuf.st_ino;
      inode->name = g_strdup (path);
      g_hash_table_insert (inode_table, inode, inode);
    }
}

static void
read_inodes ()
{
  static const char *directories[] = {
    "/lib",
    "/usr/lib",
    "/usr/X11R6/lib",
    "/usr/local/lib",
    "/opt/gnome/lib",
    NULL
  };

  const char **dirname;

  for (dirname = directories; *dirname; dirname++)
    {
      DIR *dir = opendir (*dirname);
      
      if (dir)
	{
	  struct dirent *ent;
	  while ((ent = readdir (dir)))
	    {
	      gchar buf[1024];
	      snprintf(buf, 1024-1, "%s/%s", *dirname, ent->d_name);
	      read_inode (buf);
	    }
	  
	  closedir (dir);
	}
    }
}

gchar *
locate_inode (dev_t device, ino_t inode)
{
  Inode lookup;
  Inode *result;

  lookup.device = device;
  lookup.inode = inode;

  result = g_hash_table_lookup (inode_table, &lookup);
  if (result)
    return result->name;
  else
    return NULL;
}

/************************************************************
 * Code to map addresses to object files
 ************************************************************/

static gint
compare_address (const void *symbol1, const void *symbol2)
{
  return (((Symbol *)symbol1)->addr < ((Symbol *)symbol2)->addr) ?
    -1 : ((((Symbol *)symbol1)->addr == ((Symbol *)symbol2)->addr) ?
	  0 : 1);
}

void
prepare_map (Map *map)
{
  GArray *result;
  Symbol symbol;
  int i;

  g_return_if_fail (!map->prepared);
  
  map->prepared = TRUE;

  read_bfd (map);
  
  if (map->syms)
    {
      result = g_array_new (FALSE, FALSE, sizeof(Symbol));

      for (i = 0; i < map->symcount; i++)
	{
	  if (map->syms[i]->flags & BSF_FUNCTION)
	    {
	      symbol.addr = bfd_asymbol_value(map->syms[i]);
	      symbol.size = 0;
	      symbol.name = g_strdup (bfd_asymbol_name(map->syms[i]));
	      
	      g_array_append_vals (result, &symbol, 1);
	    }
	}

      /* Sort the symbols by address */
      
      qsort (result->data, result->len, sizeof(Symbol), compare_address);
  
      map->symbols =result;
    }
}

static void
read_maps (MPProcess *process)
{
  gchar buffer[1024];
  FILE *in;
  gchar perms[26];
  gchar file[256];
  guint start, end, major, minor, inode;
  
  snprintf (buffer, 1023, "/proc/%d/maps", process->pid);

  in = fopen (buffer, "r");

  if (process->map_list)
    {
      g_list_foreach (process->map_list, (GFunc)g_free, NULL);
      g_list_free (process->map_list);
      
      process->map_list = NULL;
    }

  while (fgets(buffer, 1023, in))
    {
      int count = sscanf (buffer, "%x-%x %15s %*x %u:%u %u %255s",
			  &start, &end, perms, &major, &minor, &inode, file);
      if (count >= 6)
	{
	  if (strcmp (perms, "r-xp") == 0)
	    {
	      Map *map;
	      dev_t device = makedev(major, minor);

	      map = g_new (Map, 1);
	      map->prepared = FALSE;
	      map->addr = start;
	      map->size = end - start;

	      if (count == 7)
		map->name = g_strdup (file);
	      else
		map->name = locate_inode (device, inode);

	      map->abfd = NULL;
	      map->section = NULL;
	      map->symbols = NULL;
	      map->syms = NULL;
	      map->symcount = 0;

	      map->do_offset = TRUE;

	      if (map->name)
		{
		  struct stat stat1, stat2;

		  if (stat (map->name, &stat1) != 0)
		    g_warning ("Cannot stat %s: %s\n", map->name,
			       g_strerror (errno));
		  else
		    if (stat (process->program_name, &stat2) != 0)
		      g_warning ("Cannot stat %s: %s\n", process->program_name,
				 g_strerror (errno));
		    else
		      map->do_offset = !(stat1.st_ino == stat2.st_ino &&
					 stat1.st_dev == stat2.st_dev);
		}

	      process->map_list = g_list_prepend (process->map_list, map);
	    }
	}
    }

  fclose (in);
}

static Map *
real_locate_map (MPProcess *process, guint addr)
{
  GList *tmp_list = process->map_list;

  while (tmp_list)
    {
      Map *tmp_map = tmp_list->data;
      
      if ((addr >= tmp_map->addr) &&
	  (addr < tmp_map->addr + tmp_map->size))
	return tmp_map;
      
      tmp_list = tmp_list->next;
    }

  return NULL;
}

Map *
locate_map (MPProcess *process, guint addr)
{
  Map *map = real_locate_map (process, addr);
  if (!map)
    {
      gpointer page_addr = (gpointer) (addr - addr % PAGE_SIZE);
      if (g_list_find (process->bad_pages, page_addr))
	return NULL;

      read_maps (process);

      map = real_locate_map (process, addr);

      if (!map)
	{
	  process->bad_pages = g_list_prepend (process->bad_pages, page_addr);
	  return NULL;
	}
    }

  if (!map->prepared)
    prepare_map (map);

  return map;
}

Symbol *
process_locate_symbol (MPProcess *process, guint addr)
{
  Symbol *data;
  Map *map;
  
  guint first, middle, last;

  map = locate_map (process, addr);
  if (!map)
    return NULL;

  if (!map->symbols || (map->symbols->len == 0))
    return NULL;
  
  if (map->do_offset)
    addr -= map->addr;

  first = 0;
  last = map->symbols->len - 1;
  middle = last;

  data = (Symbol *)map->symbols->data;

  if (addr < data[last].addr)
    {
      /* Invariant: data[first].addr <= val < data[last].addr */

      while (first < last - 1)
	{
	  middle = (first + last) / 2;
	  if (addr < data[middle].addr) 
	    last = middle;
	  else
	    first = middle;
	}
      /* Size is not included in generic bfd data, so we
       * ignore it for now. (It is ELF specific)
       */
      return &data[first];
#if 0
      if (addr < data[first].addr + data[first].size)
	return &data[first];
      else
	return NULL;
#endif
    }
  else
    {
      return &data[last];
#if 0
      if (addr < data[last].addr + data[last].size)
	return &data[last];
      else
	return NULL;
#endif
    }
}

gboolean 
process_find_line (MPProcess *process, void *address,
		   const char **filename, char **functionname,
		   unsigned int *line)
{
  Map *map = locate_map (process, (guint)address);
  if (map)
    {
      bfd_vma addr = (bfd_vma)address;
      if (map->do_offset)
	addr -= map->addr;
      return find_line (map, addr, filename, functionname, line);
    }
  else
    return FALSE;
}

void
process_dump_stack (MPProcess *process, FILE *out, gint stack_size, void **stack)
{
  int i;
  for (i=0; i<stack_size; i++)
    {
      const char *filename;
      char *functionname;
      unsigned int line;
      
      if (process_find_line (process, stack[i],
			     &filename, &functionname, &line))
	{
	  fprintf(out, "\t%s(): %s:%u\n", functionname, filename, line);
	  free (functionname);
	}
      else
	fprintf(out, "\t(???)\n");
    }
}

void 
process_sections (MPProcess *process, SectionFunc func, gpointer user_data)
{
  GList *tmp_list;

  read_maps (process);

  tmp_list = process->map_list;

  while (tmp_list)
    {
      Map *map = tmp_list->data;

      if (!map->prepared)
	prepare_map (map);

      process_map_sections (map, func, user_data);
      tmp_list = tmp_list->next;
    }
}

/************************************************************
 * Communication with subprocess
 ************************************************************/

static int
instrument (MPProcess *process, char **args)
{
  int fds[2];
  int pid;

  pipe (fds);

  pid = fork();
  if (pid < 0)
    show_error (ERROR_FATAL, "Cannot fork: %s\n", g_strerror (errno));

  if (pid == 0)			/* Child  */
    {
      gchar *envstr;
      
      envstr = g_strdup_printf ("%s=%d", "_MEMPROF_FD", fds[1]);
      putenv (envstr);

      envstr = g_strdup_printf ("%s=%s", "LD_PRELOAD", lib_location);
      putenv (envstr);

      close (fds[0]);

      execvp (args[0], args);

      g_warning ("Cannot run program: %s", g_strerror (errno));
      _exit(1);
    }

  process->pid = pid;
  
  close (fds[1]);
  return fds[0];
}

static gboolean 
input_func (GIOChannel  *source,
	    GIOCondition condition,
	    gpointer     data)
{
  MIInfo info;
  guint count;
  MPProcess *process = data;
  
  g_io_channel_read (source, (char *)&info, sizeof(info), &count);

  if (count == 0)
    {
      g_io_channel_unref (process->input_channel);
      process->input_channel = NULL;
      waitpid (process->pid, NULL, 0);
      return FALSE;
    }
  else
    {
      Block *block;
      void **stack;

      stack = g_new (void *, info.stack_size);
      g_io_channel_read (source, (char *)stack, sizeof(void *) * info.stack_size, &count);

      /* From a forked child */
      if (info.pid != process->pid)
	{
	  g_free (stack);
	  return TRUE;
	}

      switch (info.operation)
	{
	case MI_MALLOC:
	  process->bytes_used += info.size;
	  process->n_allocations++;
	  block = g_new (Block, 1);
	  block->flags = 0;
	  block->addr = info.new_ptr;
	  block->size = info.size;
	  block->stack_size = info.stack_size;
	  block->stack = stack;
	  g_hash_table_insert (process->block_table, info.new_ptr, block);
	  break;

	case MI_REALLOC:
	  if (info.old_ptr == NULL)
	    {
	      process->bytes_used += info.size;
	      block = g_new (Block, 1);
	    }
	  else
	    {
	      block = g_hash_table_lookup (process->block_table, info.old_ptr);
	      if (!block)
		{
		  g_warning ("Block %p not found!\n", info.old_ptr);
		  process_dump_stack (process, stderr, info.stack_size, stack);

		  block = g_new (Block, 1);
		  block->flags = 0;

		  process->bytes_used += info.size;
		}
	      else
		{
		  g_free (block->stack);
		  g_hash_table_remove (process->block_table, info.old_ptr);
		  process->n_allocations--;
	      
		  process->bytes_used += info.size - block->size;
		}
	    }

	  if (info.new_ptr)
	    {
	      block->addr = info.new_ptr;
	      block->size = info.size;
	      block->stack_size = info.stack_size;
	      block->stack = stack;
	      process->n_allocations++;
	      
	      g_hash_table_insert (process->block_table, info.new_ptr, block);
	    }
	  else
	    {
	      process->n_allocations--;
	      g_free (block);
	      g_free (stack);
	    }
	    
	  break;

	case MI_FREE:
	  block = g_hash_table_lookup (process->block_table, info.old_ptr);
	  if (!block)
	    {
	      g_warning ("Block %p not found!\n", info.old_ptr);
	      process_dump_stack (process, stderr, info.stack_size, stack);
	    }
	  else
	    {
	      process->bytes_used -= block->size;
	      process->n_allocations--;
	      g_free (block->stack);
	      g_free (block);
	      g_free (stack);
	      
	      g_hash_table_remove (process->block_table, info.old_ptr);
	      break;
	    }
	}
    }
  
  return TRUE;
}

void
process_stop_input (MPProcess *process)
{
  g_return_if_fail (process != NULL);
  
  if (process->input_tag)
    {
      g_source_remove (process->input_tag);
      process->input_tag = 0;
    }
}

void
process_start_input (MPProcess *process)
{
  g_return_if_fail (process != NULL);
  
  if (!process->input_tag && process->input_channel)
    process->input_tag = g_io_add_watch_full (process->input_channel, G_PRIORITY_LOW, G_IO_IN, input_func, process, NULL);
}

void
process_init (void)
{
  const char **dirname;
  char *path;

  static const char *directories[] = {
    ".libs",
    ".",
    LIBDIR,
    NULL
  };

  read_inodes ();

  for (dirname = directories; *dirname; dirname++)
    {
      path = g_concat_dir_and_file (*dirname, "libmemintercept.so");
      if (!access (path, R_OK))
	{
	  lib_location = path;
	  break;
	}
      g_free (path);
    }

  if (!lib_location)
    show_error (ERROR_FATAL, _("Cannot find libmemintercept.so"));
}

MPProcess *
process_run (char *exec_string)
{
  int fd;
  MPProcess *process;
  char **args;

  args = g_strsplit (exec_string, " ", -1);
  if (!g_file_exists(args[0]))
    {
      g_strfreev (args);
      return NULL;
    }

  process = g_new0 (MPProcess, 1);

  process->bytes_used = 0;
  process->n_allocations = 0;
  process->block_table = g_hash_table_new (g_direct_hash, NULL);

  process->program_name = args[0];
  read_inode (args[0]);

  fd = instrument (process, args);
  process->input_channel = g_io_channel_unix_new (fd);
  
  process_start_input (process);

  return process;
}
