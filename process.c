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

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>

#include <gtk/gtksignal.h>

#include <libgnome/libgnome.h>

#include "memintercept.h"
#include "memprof.h"
#include "process.h"
#include "server.h"

enum {
	STATUS_CHANGED,
	RESET,
	LAST_SIGNAL
};
static guint process_signals[LAST_SIGNAL] = { 0 };

static void mp_process_class_init (MPProcessClass *class);
static void mp_process_init (MPProcess *process);
static void mp_process_finalize (GtkObject *object);

#define MP_PAGE_SIZE 4096

/* Code to keep a queue of commands read
 */
typedef struct {
	MIInfo info;
	void **stack;
} Command;

static gint
queue_compare (gconstpointer a, gconstpointer b)
{
	const Command *commanda = a;
	const Command *commandb = b;

	return (commanda->info.any.seqno < commandb->info.any.seqno ? -1 :
		(commanda->info.any.seqno > commandb->info.any.seqno  ? 1 : 0));
}

static void
queue_command (MPProcess *process, MIInfo *info, void **stack)
{
	Command *command = g_new (Command, 1);
	command->info = *info;
	command->stack = stack;

	process->command_queue = g_list_insert_sorted (process->command_queue, command, queue_compare);
}

static gboolean
unqueue_command (MPProcess *process, MIInfo *info, void ***stack)
{
	Command *command = process->command_queue->data;
	GList *tmp_list;
	
	if (command->info.any.seqno == process->seqno) {
		*stack = command->stack;
		*info = command->info;

		tmp_list = process->command_queue;
		process->command_queue = g_list_remove_link (process->command_queue, tmp_list);
		g_free (command);
		g_list_free_1 (tmp_list);

		return TRUE;
	} else
		return FALSE;
}

/************************************************************
 * Block Manipulation
 ************************************************************/

static void
block_unref (Block *block)
{
	block->refcount--;
	if (block->refcount == 0)
	{
		g_free (block->stack);
		g_free (block);
	}
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
  
	if (map->syms) {
		result = g_array_new (FALSE, FALSE, sizeof(Symbol));

		for (i = 0; i < map->symcount; i++) {
			if (map->syms[i]->flags & BSF_FUNCTION)	{
				symbol.addr = bfd_asymbol_value(map->syms[i]);
				symbol.size = 0;
				symbol.name = demangle (map, bfd_asymbol_name(map->syms[i]));
	      
				g_array_append_vals (result, &symbol, 1);
			}
		}

		/* Sort the symbols by address */
      
		qsort (result->data, result->len, sizeof(Symbol), compare_address);
  
		map->symbols =result;
	}
}

static void
process_read_maps (MPProcess *process)
{
	gchar buffer[1024];
	FILE *in;
	gchar perms[26];
	gchar file[256];
	guint start, end, major, minor, inode;
  
	snprintf (buffer, 1023, "/proc/%d/maps", process->pid);

	in = fopen (buffer, "r");

	if (process->map_list) {
		g_list_foreach (process->map_list, (GFunc)g_free, NULL);
		g_list_free (process->map_list);
      
		process->map_list = NULL;
	}

	while (fgets(buffer, 1023, in)) {
		int count = sscanf (buffer, "%x-%x %15s %*x %u:%u %u %255s",
				    &start, &end, perms, &major, &minor, &inode, file);
		if (count >= 6)	{
			if (strcmp (perms, "r-xp") == 0) {
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

				if (map->name) {
					struct stat stat1, stat2;
					char *progname = g_strdup_printf ("/proc/%d/exe", process->pid);
		  
					if (stat (map->name, &stat1) != 0)
						g_warning ("Cannot stat %s: %s\n", map->name,
							   g_strerror (errno));
					else
						if (stat (progname, &stat2) != 0)
							g_warning ("Cannot stat %s: %s\n", process->program_name,
								   g_strerror (errno));
						else
							map->do_offset = !(stat1.st_ino == stat2.st_ino &&
									   stat1.st_dev == stat2.st_dev);

					g_free (progname);
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
		gpointer page_addr = (gpointer) (addr - addr % MP_PAGE_SIZE);
		if (g_list_find (process->bad_pages, page_addr))
			return NULL;

		process_read_maps (process);

		map = real_locate_map (process, addr);

		if (!map) {
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

	if (addr < data[last].addr) {
		/* Invariant: data[first].addr <= val < data[last].addr */

		while (first < last - 1) {
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
	if (map) {
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
				       &filename, &functionname, &line)) {
			fprintf(out, "\t%s(): %s:%u\n", functionname, filename, line);
			free (functionname);
		} else
			fprintf(out, "\t(???)\n");
	}
}

void 
process_sections (MPProcess *process, SectionFunc func, gpointer user_data)
{
	GList *tmp_list;

	process_read_maps (process);

	tmp_list = process->map_list;

	while (tmp_list) {
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

static void
process_free_block (gpointer key, gpointer value, gpointer data)
{
	block_unref (value);
}

static void
process_duplicate_block (gpointer key, gpointer value, gpointer data)
{
	GHashTable *new_table = data;
	Block *block = value;

	block->refcount++;

	g_hash_table_insert (new_table, key, value);
}

MPProcess *
process_duplicate (MPProcess *process)
{
	MPProcess *new_process = process_new (process->server);

	g_hash_table_foreach (process->block_table,
			      process_duplicate_block,
			      new_process->block_table);

	new_process->bytes_used = process->bytes_used;
	new_process->n_allocations = process->n_allocations;
	new_process->seqno = process->seqno;

	new_process->parent = process;

	return new_process;
}

void
process_exec_reset (MPProcess *process)
{
	process_stop_input (process);

	if (process->input_channel) {
		g_io_channel_unref (process->input_channel);
		process->input_channel = NULL;
	}

	process->bytes_used = 0;
	process->n_allocations = 0;
	process->seqno = 0;

	if (process->map_list) {
		g_list_foreach (process->map_list, (GFunc)g_free, NULL);
		g_list_free (process->map_list);
      
		process->map_list = NULL;
	}

	if (process->bad_pages) {
		g_list_free (process->bad_pages);
		process->bad_pages = NULL;
	}

	g_hash_table_foreach (process->block_table, process_free_block, NULL);
	g_hash_table_destroy (process->block_table);
	process->block_table = g_hash_table_new (g_direct_hash, NULL);
	
	/* FIXME: leak */
	process->command_queue = NULL;

	gtk_signal_emit (GTK_OBJECT (process), process_signals[RESET]);
}

static void
process_command (MPProcess *process, MIInfo *info, void **stack)
{
	Block *block;

	switch (info->operation) {
	case MI_MALLOC:
	case MI_FREE:
	case MI_REALLOC:
	case MI_EXIT:
		break;
	default:
		abort();
	}
		
	if (info->any.seqno != process->seqno) {
		queue_command (process, info, stack);
		return;
	}

	process->seqno++;

	block = NULL;
	
	switch (info->operation) {
	case MI_NEW:
	case MI_FORK:
	case MI_CLONE:
	case MI_EXEC:
		g_assert_not_reached ();

	case MI_EXIT:
		process_set_status (process, MP_PROCESS_EXITING);
		break;
		
	default: /* MALLOC / REALLOC / FREE */
		if (info->alloc.old_ptr != NULL) {
			block = g_hash_table_lookup (process->block_table, info->alloc.old_ptr);
			if (!block) {
				g_warning ("Block %p not found!\n", info->alloc.old_ptr);
				process_dump_stack (process, stderr, info->alloc.stack_size, stack);
			}
			else {
				g_hash_table_remove (process->block_table, info->alloc.old_ptr);
				block_unref (block);
				
				process->bytes_used -= block->size;
				process->n_allocations--;
			}
		}
		
		if (info->alloc.new_ptr) {
			block = g_new (Block, 1);
			block->refcount = 1;
			
			block->flags = 0;
			block->addr = info->alloc.new_ptr;
			block->size = info->alloc.size;
			block->stack_size = info->alloc.stack_size;
			block->stack = stack;
			
			process->n_allocations++;
			process->bytes_used += info->alloc.size;
			
			g_hash_table_insert (process->block_table, info->alloc.new_ptr, block);
		}
		else
			g_free (stack);
	}

	while (process->command_queue && unqueue_command (process, info, &stack))
		process_command (process, info, stack);
}

static gboolean 
input_func (GIOChannel  *source,
	    GIOCondition condition,
	    gpointer     data)
{
	MIInfo info;
	guint count;
	MPProcess *input_process = data;
	MPProcess *process = NULL;
  
	g_io_channel_read (source, (char *)&info, sizeof(info), &count);

	if (count == 0) {
		g_io_channel_unref (input_process->input_channel);
		input_process->input_channel = NULL;

		mp_server_remove_process (input_process->server, input_process);
		process_set_status (input_process, MP_PROCESS_DEFUNCT);
		
		return FALSE;
	} else {
		void **stack = NULL;

		if (info.operation == MI_MALLOC ||
		    info.operation == MI_REALLOC ||
		    info.operation == MI_FREE) {
			stack = g_new (void *, info.alloc.stack_size);
			g_io_channel_read (source, (char *)stack, sizeof(void *) * info.alloc.stack_size, &count);

		}

		process = input_process;
		while (process->clone_of)
			process = process->clone_of;
	
		process_command (process, &info, stack);

/*		if (info.any.pid != input_process->pid)
		g_warning ("Ow! Ow! Ow: %d %d %d!", info.any.pid, input_process->pid, g_io_channel_unix_get_fd (input_process->input_channel)); */

	}
  
	return TRUE;
}

void
process_stop_input (MPProcess *process)
{
	g_return_if_fail (process != NULL);
  
	if (process->input_tag) {
		g_source_remove (process->input_tag);
		process->input_tag = 0;
	}
}

void
process_start_input (MPProcess *process)
{
	g_return_if_fail (process != NULL);
  
	if (!process->input_tag && process->input_channel)
		process->input_tag = g_io_add_watch_full (process->input_channel,
							  G_PRIORITY_LOW,
							  G_IO_IN | G_IO_HUP,
							  input_func, process, NULL);
}

char *
process_find_exec (char **args)
{
	int i;
  
	if (g_file_exists(args[0])) {
		return g_strdup (args[0]);
	} else {
		char **paths;
		char *path = NULL;
		char *pathenv = getenv ("PATH");
		if (pathenv)
		{
			paths = g_strsplit (pathenv, ":", -1);
			for (i=0; paths[i]; i++) {
				path = g_concat_dir_and_file (paths[i], args[0]);
				if (g_file_exists (path))
					break;
				else {
					g_free (path);
					path = NULL;
				}
			}

			g_strfreev (paths);
		}

		return path;
	}
}

char **
process_parse_exec (const char *exec_string)
{
	return g_strsplit (exec_string, " ", -1);
}

GtkType
mp_process_get_type (void)
{
  static GtkType process_type = 0;

  if (!process_type)
    {
      static const GtkTypeInfo process_info =
      {
	"MPProcess",
	sizeof (MPProcess),
	sizeof (MPProcessClass),
	(GtkClassInitFunc) mp_process_class_init,
	(GtkObjectInitFunc) mp_process_init,
        /* reserved_1 */ NULL,
	/* reserved_2 */ NULL,
	(GtkClassInitFunc) NULL,
      };

      process_type = gtk_type_unique (GTK_TYPE_OBJECT, &process_info);
    }

  return process_type;
}

static void
mp_process_class_init (MPProcessClass *class)
{
	GtkObjectClass *object_class;

	object_class = GTK_OBJECT_CLASS (class);
	
	process_signals[STATUS_CHANGED] =
		gtk_signal_new ("status_changed",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (MPProcessClass, status_changed),
				gtk_marshal_NONE__NONE,
				GTK_TYPE_NONE, 0);

	process_signals[RESET] =
		gtk_signal_new ("reset",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (MPProcessClass, reset),
				gtk_marshal_NONE__NONE,
				GTK_TYPE_NONE, 0);

	gtk_object_class_add_signals (object_class, process_signals, LAST_SIGNAL);
	object_class->finalize = mp_process_finalize;
}

static void
mp_process_init (MPProcess *process)
{
	process->status = MP_PROCESS_INIT;
	process->pid = 0;
	process->clone_of = NULL;
  
	process->bytes_used = 0;
	process->n_allocations = 0;
	process->block_table = g_hash_table_new (g_direct_hash, NULL);

	process->program_name = NULL;
	process->input_channel = NULL;

	process->seqno = 0;

	process->command_queue = NULL;

	process->follow_fork = FALSE;
	process->follow_exec = FALSE;

	gtk_object_ref (GTK_OBJECT (process));
	gtk_object_sink (GTK_OBJECT (process));
}

static void 
mp_process_finalize (GtkObject *object)
{
	MPProcess *process = MP_PROCESS (object);

	process_exec_reset (process);
	
	g_free (process->program_name);
}


MPProcess *
process_new (MPServer *server)
{
	MPProcess *process;

	process = gtk_type_new (mp_process_get_type ());

	process->server = server;

	return process;
}

void
process_set_follow_fork (MPProcess *process,
			 gboolean   follow_fork)
{
	process->follow_fork = follow_fork;
}

void
process_set_follow_exec (MPProcess *process,
			 gboolean   follow_exec)
{
	process->follow_exec = follow_exec;
}

void
process_run (MPProcess *process, const char *path, char **args)
{
	process->program_name = g_strdup (path);
	read_inode (path);

	process->pid = mp_server_instrument (process->server, args);
	mp_server_add_process (process->server, process);

	process_set_status (process, MP_PROCESS_STARTING);
  
	process_start_input (process);
}

GList *
process_get_clones (MPProcess *process)
{
	return mp_server_get_process_clones (process->server, process);
}

void
process_set_status (MPProcess *process, MPProcessStatus status)
{
	if (process->status != status) {
		process->status = status;
		gtk_signal_emit (GTK_OBJECT (process), process_signals[STATUS_CHANGED]);
	}
}

char *
process_get_status_text (MPProcess *process)
{
	char *status = "";
	
	switch (process->status) {
	case MP_PROCESS_INIT:
		status = _("Initial");
		break;
	case MP_PROCESS_STARTING:
		status = _("Starting");
		break;
	case MP_PROCESS_RUNNING:
		status = _("Running");
		break;
	case MP_PROCESS_EXITING:
		status = _("Exiting");
		break;
	case MP_PROCESS_DEFUNCT:
		status = _("Defunct");
		break;
	case MP_PROCESS_DETACHED:
		status = _("Defunct");
		break;
	}

	return g_strdup (status);
}

char *
process_get_cmdline (MPProcess *process)
{
	char *fname;
	char *result;
	char *tmp;
	int n = 0;
	FILE *in = NULL;

	if (process->status == MP_PROCESS_DEFUNCT)
		return g_strdup ("");
	
	fname = g_strdup_printf ("/proc/%d/cmdline", process->pid);
	in = fopen (fname, "r");
	if (!in) {
		g_warning ("Can't open %s\n", fname);
		return g_strdup ("");
	}
	g_free (fname);

	getline (&tmp, &n, in);
	result = g_strdup (tmp);
	free (tmp);

	fclose (in);

	return result;
}

void
process_detach (MPProcess *process)
{
	if (process->status != MP_PROCESS_DEFUNCT) {
		int fd = g_io_channel_unix_get_fd (process->input_channel);

		if (process->status == MP_PROCESS_EXITING) {
			char response = 0;
			write (fd, &response, 1);
		} else {
			g_io_channel_close (process->input_channel);
			process_set_status (process, MP_PROCESS_DETACHED);
		}
	}
}

void
process_kill (MPProcess *process)
{
	if (process->status == MP_PROCESS_EXITING) {
		process_detach (process);
	} else if (process->status != MP_PROCESS_DEFUNCT &&
		   process->status != MP_PROCESS_INIT) {
		kill (process->pid, SIGTERM);
	}
}
