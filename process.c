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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
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
	MIInfo info;
	void **stack;
} Command;

typedef struct {
	dev_t device;
	ino_t inode;
	gchar *name;
} Inode;

GHashTable *inode_table = NULL;
char *lib_location = NULL;

#define SOCKET_TEMPLATE "memprof.XXXXXX"
static char *socket_path = NULL;

/* Socket to accept connections from new processes */
static int socket_fd;

static ProcessCreateFunc create_func;

#define PAGE_SIZE 4096

gint
queue_compare (gconstpointer a, gconstpointer b)
{
	const Command *commanda = a;
	const Command *commandb = b;

	return (commanda->info.any.seqno < commandb->info.any.seqno ? -1 :
		(commanda->info.any.seqno > commandb->info.any.seqno  ? 1 : 0));
}

void
queue_command (MPProcess *process, MIInfo *info, void **stack)
{
	Command *command = g_new (Command, 1);
	command->info = *info;
	command->stack = stack;

	process->command_queue = g_list_insert_sorted (process->command_queue, command, queue_compare);
}

gboolean
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
 * Code to keep track of current processes
 ************************************************************/

/* It would be faster to implement this as a flat array with a
 * binary search, but this is easier as a first pass
 */
GHashTable *pid_table = NULL;

static MPProcess *
process_table_find (pid_t pid)
{
	if (pid_table) {
		return g_hash_table_lookup (pid_table, GUINT_TO_POINTER (pid));
	} else {
		return NULL;
	}
}

static void
process_table_add (MPProcess *process)
{
	if (!pid_table)
		pid_table = g_hash_table_new (g_direct_hash, NULL);

	g_hash_table_insert (pid_table, GUINT_TO_POINTER (process->pid), process);
}

static void
process_table_remove (MPProcess *process)
{
	g_return_if_fail (pid_table != NULL);

	g_hash_table_remove (pid_table, GUINT_TO_POINTER (process->pid));
}

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
read_inode (const gchar *path)
{
	struct stat stbuf;

	g_return_if_fail (path != NULL);

	if (!inode_table)
		inode_table = g_hash_table_new (inode_hash, inode_compare);

	if (!stat (path, &stbuf)) {
		Inode *inode = g_new (Inode, 1);
		inode->device = stbuf.st_dev;
		inode->inode = stbuf.st_ino;
		if (!g_hash_table_lookup (inode_table, inode)) {
			inode->name = g_strdup (path);
			g_hash_table_insert (inode_table, inode, inode);
		} else
			g_free (inode);
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
      
		if (dir) {
			struct dirent *ent;
			while ((ent = readdir (dir))) {
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
		gpointer page_addr = (gpointer) (addr - addr % PAGE_SIZE);
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
instrument (MPProcess *process, char **args)
{
	int pid;

	/* pipe (fds); */

	pid = fork();
	if (pid < 0)
		show_error (ERROR_FATAL, "Cannot fork: %s\n", g_strerror (errno));

	if (pid == 0) {		/* Child  */
		gchar *envstr;
      
		envstr = g_strdup_printf ("%s=%s", "_MEMPROF_SOCKET", socket_path);
		putenv (envstr);

		envstr = g_strdup_printf ("%s=%s", "LD_PRELOAD", lib_location);
		putenv (envstr);

		execvp (args[0], args);

		g_warning ("Cannot run program: %s", g_strerror (errno));
		_exit(1);
	}

	process->pid = pid;
	process_table_add (process);
}

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
	MPProcess *new_process = process_new ();

	g_hash_table_foreach (process->block_table,
			      process_duplicate_block,
			      new_process->block_table);

	new_process->bytes_used = process->bytes_used;
	new_process->n_allocations = process->n_allocations;
	new_process->seqno = process->seqno;

	return new_process;
}

static void
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
}

/* Input func to receive new process connections */
static gboolean 
control_func (GIOChannel  *source,
	      GIOCondition condition,
	      gpointer     data)
{
	int newfd;
	MIInfo info;
	MPProcess *process = NULL;
	MPProcess *parent_process;
	int count;
	char response = 0;

	newfd = accept (socket_fd, NULL, 0);
	if (newfd < 0) {
		g_warning ("accept: %s\n", g_strerror (errno));
		goto out;
	}

	count = read (newfd, &info, sizeof(info));
	if (count < sizeof(info)) {
		g_warning ("short read from new process\n");
		goto out;
	}

	switch (info.operation) {
	case MI_FORK:
		parent_process = process_table_find (info.fork.pid);
		if (parent_process && !parent_process->follow_fork)
			goto out; /* Return negative response */

		/* Fall through */
	case MI_NEW:
		process = process_table_find (info.fork.new_pid);
		if (process) {
			if (process->follow_exec) {
				process = (*create_func) (NULL, info.fork.new_pid);
				process_table_add (process); /* Overwrites old process */
				
			} else
				process_exec_reset (process);
		}

		if (!process) {
			parent_process = process_table_find (info.fork.pid);
			process = (*create_func) (parent_process, info.fork.new_pid);
			process_table_add (process);
		}
		
		break;
	case MI_CLONE:
		parent_process = process_table_find (info.fork.pid);
		process = process_new ();
		
		process->clone_of = parent_process;
		process->pid = info.fork.new_pid;

		process_table_add (process);

		while (parent_process->clone_of)
			parent_process = parent_process->clone_of;
			
		break;
		
	case MI_MALLOC:
	case MI_REALLOC:
	case MI_FREE:
	case MI_EXEC:
		g_assert_not_reached ();
	}

	if (process) {
		process->input_channel = g_io_channel_unix_new (newfd);
		process_start_input (process);
		response = 1;
	}

 out:
	if (newfd >= 0) {
		write (newfd, &response, 1);
		if (!response)
			close (newfd);
	}

	return TRUE;
}

static void
process_command (MPProcess *process, MIInfo *info, void **stack)
{
	Block *block;

	switch (info->operation) {
	case MI_MALLOC:
	case MI_FREE:
	case MI_REALLOC:
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
		
		waitpid (input_process->pid, NULL, 0);

		process_table_remove (input_process);
		
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
		process->input_tag = g_io_add_watch_full (process->input_channel, G_PRIORITY_LOW, G_IO_IN, input_func, process, NULL);
}

void process_cleanup (void)
{
	if (socket_path)
		unlink (socket_path);
}

void
process_init (ProcessCreateFunc cfunc)
{
	const char **dirname;
	char *path;
	GIOChannel *channel;
	struct sockaddr_un addr;
	int addrlen;

	static const char *directories[] = {
		".libs",
		".",
		LIBDIR,
		NULL
	};

	create_func = cfunc;
  
	read_inodes ();

	for (dirname = directories; *dirname; dirname++) {
		path = g_concat_dir_and_file (*dirname, "libmemintercept.so");
		if (!access (path, R_OK)) {
			lib_location = path;
			break;
		}
		g_free (path);
	}

	if (!lib_location)
		show_error (ERROR_FATAL, _("Cannot find libmemintercept.so"));

	/* Make lib_location absolute */

	if (lib_location[0] != '/') {
		char *wd = g_get_current_dir();
		char *newloc = g_strconcat (wd, "/", lib_location, NULL);
		g_free (lib_location);
		lib_location = newloc;
		g_free (wd);
	}

	/* Create a control socket */

	memset (&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;

	socket_fd = socket (PF_UNIX, SOCK_STREAM, 0);
	if (socket_fd < 0)
		g_error ("socket: %s\n", g_strerror (errno));

 retry:
	if (socket_path)
		g_free (socket_path);

	socket_path = g_concat_dir_and_file (g_get_tmp_dir(), SOCKET_TEMPLATE);
	mktemp (socket_path);

	strncpy (addr.sun_path, socket_path, sizeof (addr.sun_path));
	addrlen = sizeof(addr.sun_family) + strlen (addr.sun_path);
	if (addrlen > sizeof (addr))
		addrlen = sizeof(addr);

	if (bind (socket_fd, &addr, addrlen) < 0) {
		if (errno == EADDRINUSE)
			goto retry;
		else
			g_error ("bind: %s\n", g_strerror (errno));
	}

	g_atexit (process_cleanup);

	if (listen (socket_fd, 8) < 0)
		g_error ("listen: %s\n", g_strerror (errno));

	channel = g_io_channel_unix_new (socket_fd);
	g_io_add_watch (channel, G_IO_IN | G_IO_HUP, control_func, NULL);
	g_io_channel_unref (channel);
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

MPProcess *
process_new (void)
{
	MPProcess *process;
	
	process = g_new0 (MPProcess, 1);

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

	instrument (process, args);
  
	process_start_input (process);
}

typedef struct {
	MPProcess *parent;
	GList *result;
} CloneInfo;

static void
add_clone (gpointer key, MPProcess *process, CloneInfo *ci)
{
	MPProcess *parent = process;
	while (parent->clone_of)
		parent = parent->clone_of;

	if (parent == ci->parent)
		ci->result = g_list_prepend (ci->result, process);
}

GList *
process_get_clones (MPProcess *process)
{
	GList *result = NULL;
	CloneInfo ci;

	ci.parent = process;
	ci.result = NULL;
	
	g_hash_table_foreach (pid_table, (GHFunc)add_clone, &ci);

	return ci.result;
}
     
