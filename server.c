/* -*- mode: C; c-file-style: "linux" -*- */

/* MemProf -- memory profiler and leak detector
 * Copyright (C) 1999-2000 Red Hat, Inc.
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

#include <errno.h>
#include <stdlib.h>
#include <sys/un.h>
#include <sys/socket.h>

#include <gtk/gtksignal.h>
#include <libgnome/libgnome.h>

#include "memintercept.h"
#include "server.h"

#define SOCKET_TEMPLATE "memprof.XXXXXX"

static void   mp_server_class_init  (MPServerClass *class);
static void   mp_server_init        (MPServer      *server);
static void   mp_server_finalize    (GtkObject     *object);

static char *   find_lib_location     (void);
static void     create_control_socket (MPServer     *server);
static gboolean control_func          (GIOChannel   *source,
				       GIOCondition  condition,
				       gpointer      data);
struct _MPServer
{
	GtkObject parent_instance;
	
	char *lib_location;
	char *socket_path;

	int socket_fd;

        /* It would be faster to implement this as a flat array with a
	 * binary search, but this is easier as a first pass
	 */
	GHashTable *pid_table;
	guint control_watch;
};

struct _MPServerClass {
	GtkObjectClass parent_class;

        void (*process_created) (MPServer *server, MPProcess *process);
};

/* Global list of socket paths so we can cleanup in an atexit() function
 */
static GSList *socket_paths;

enum {
	PROCESS_CREATED,
	LAST_SIGNAL
};
static guint server_signals[LAST_SIGNAL] = { 0 };

GtkType
mp_server_get_type (void)
{
	static GtkType server_type = 0;

	if (!server_type) {
		static const GtkTypeInfo server_info = {
			"MPServer",
			sizeof (MPServer),
			sizeof (MPServerClass),
			(GtkClassInitFunc) mp_server_class_init,
			(GtkObjectInitFunc) mp_server_init,
			/* reserved_1 */ NULL,
			/* reserved_2 */ NULL,
			(GtkClassInitFunc) NULL,
		};

		server_type = gtk_type_unique (GTK_TYPE_OBJECT, &server_info);
	}

	return server_type;
}

static void
mp_server_class_init (MPServerClass *class)
{
	GtkObjectClass *object_class;

	object_class = GTK_OBJECT_CLASS (class);

	object_class->finalize = mp_server_finalize;
	class->process_created = NULL;

	server_signals[PROCESS_CREATED] =
		gtk_signal_new ("process_created",
				GTK_RUN_LAST,
				object_class->type,
				GTK_SIGNAL_OFFSET (MPServerClass, process_created),
				gtk_marshal_NONE__POINTER,
				GTK_TYPE_NONE, 1,
				MP_TYPE_PROCESS);

	gtk_object_class_add_signals (object_class, server_signals, LAST_SIGNAL);
}

static void
mp_server_init (MPServer *server)
{
	GIOChannel *channel;

	server->pid_table = NULL;
	server->lib_location = find_lib_location ();
	create_control_socket (server);

	channel = g_io_channel_unix_new (server->socket_fd);
	server->control_watch = g_io_add_watch (channel, G_IO_IN | G_IO_HUP, control_func, server);
	g_io_channel_unref (channel);

	gtk_object_ref (GTK_OBJECT (server));
	gtk_object_sink (GTK_OBJECT (server));
}

static void
mp_server_finalize (GtkObject *object)
{
	MPServer *server = MP_SERVER (object);
	
	if (server->control_watch)
		g_source_remove (server->control_watch);
	if (server->pid_table)
		g_hash_table_destroy (server->pid_table);
	close (server->socket_fd);

	g_slist_remove (socket_paths, server->socket_path);
	g_free (server->socket_path);
}

MPServer *
mp_server_new (void)
{
	MPServer *server = gtk_type_new (mp_server_get_type ());

	return server;
}

int
mp_server_instrument (MPServer *server, char **args)
{
	int pid;

	pid = fork();
	if (pid < 0)
		show_error (ERROR_FATAL, "Cannot fork: %s\n", g_strerror (errno));

	if (pid == 0) {		/* Child  */
		gchar *envstr;
      
		envstr = g_strdup_printf ("%s=%s", "_MEMPROF_SOCKET", server->socket_path);
		putenv (envstr);

		envstr = g_strdup_printf ("%s=%s", "LD_PRELOAD", server->lib_location);
		putenv (envstr);

		execvp (args[0], args);

		g_warning ("Cannot run program: %s", g_strerror (errno));
		_exit(1);
	}

	return pid;
}


static char *
find_lib_location (void)
{
	const char **dirname;

	static const char *directories[] = {
		".libs",
		".",
		LIBDIR,
		NULL
	};

	char *lib_location;
	
	lib_location = NULL;
	for (dirname = directories; *dirname; dirname++) {
		char *path = g_concat_dir_and_file (*dirname, "libmemintercept.so");
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

	return lib_location;
}

static void
cleanup_socket (void)
{
	g_slist_foreach (socket_paths, (GFunc)unlink, NULL);
}

static void
create_control_socket (MPServer *server)
{
	static gboolean added_cleanup;
	struct sockaddr_un addr;
	int addrlen;

	memset (&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;

	server->socket_fd = socket (PF_UNIX, SOCK_STREAM, 0);
	if (server->socket_fd < 0)
		g_error ("socket: %s\n", g_strerror (errno));

 retry:
	if (server->socket_path)
		g_free (server->socket_path);

	server->socket_path = g_concat_dir_and_file (g_get_tmp_dir(), SOCKET_TEMPLATE);
	mktemp (server->socket_path);

	strncpy (addr.sun_path, server->socket_path, sizeof (addr.sun_path));
	addrlen = sizeof(addr.sun_family) + strlen (addr.sun_path);
	if (addrlen > sizeof (addr))
		addrlen = sizeof(addr);

	if (bind (server->socket_fd, &addr, addrlen) < 0) {
		if (errno == EADDRINUSE)
			goto retry;
		else
			g_error ("bind: %s\n", g_strerror (errno));
	}

	if (!added_cleanup) {
		g_atexit (cleanup_socket);
		added_cleanup = TRUE;
	}

	socket_paths = g_slist_prepend (socket_paths, server->socket_path);

	if (listen (server->socket_fd, 8) < 0)
		g_error ("listen: %s\n", g_strerror (errno));
}

static void
mp_server_process_created (MPServer *server, MPProcess *process)
{
	gtk_signal_emit (GTK_OBJECT (server), server_signals[PROCESS_CREATED], process);
}

/* Input func to receive new process connections */
static gboolean 
control_func (GIOChannel  *source,
	      GIOCondition condition,
	      gpointer     data)
{
	int newfd;
	MIInfo info;

	MPServer *server = data;
	
	MPProcess *process = NULL;
	MPProcess *parent_process;
	int count;
	char response = 0;

	newfd = accept (server->socket_fd, NULL, 0);
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
		parent_process = mp_server_find_process (server, info.fork.pid);
		if (parent_process && !parent_process->follow_fork) {
			goto out; /* Return negative response */
		}

		/* Fall through */
	case MI_NEW:
		process = mp_server_find_process (server, info.fork.new_pid);
		if (process) {
			if (process->follow_exec) {
				process = process_new (server);
				mp_server_add_process (server, process); /* Overwrites old process */
				mp_server_process_created (server, process);

			} else
				process_exec_reset (process);
		}

		if (!process) {
			parent_process = mp_server_find_process (server, info.fork.pid);
			if (!parent_process) {
				g_warning ("Unexpected connection from %d", info.fork.new_pid);
				goto out;
			}
			
			process = process_duplicate (parent_process);
			process->pid = info.fork.new_pid;
			mp_server_add_process (server, process);
			mp_server_process_created (server, process);
		}
		
		process_set_status (process, MP_PROCESS_RUNNING);
		
		break;
	case MI_CLONE:
		parent_process = mp_server_find_process (server, info.fork.pid);
		process = process_new (server);
		process_set_status (process, MP_PROCESS_RUNNING);
		
		process->clone_of = parent_process;
		process->pid = info.fork.new_pid;

		mp_server_add_process (server, process);

		while (parent_process->clone_of)
			parent_process = parent_process->clone_of;
			
		break;
		
	case MI_MALLOC:
	case MI_REALLOC:
	case MI_FREE:
	case MI_EXEC:
	case MI_EXIT:
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

/************************************************************
 * Code to keep track of current processes
 ************************************************************/

MPProcess *
mp_server_find_process (MPServer *server, pid_t pid)
{
	if (server->pid_table) {
		return g_hash_table_lookup (server->pid_table, GUINT_TO_POINTER (pid));
	} else {
		return NULL;
	}
}

void
mp_server_add_process (MPServer *server, MPProcess *process)
{
	if (!server->pid_table)
		server->pid_table = g_hash_table_new (g_direct_hash, NULL);

	g_hash_table_insert (server->pid_table, GUINT_TO_POINTER (process->pid), process);
}

void
mp_server_remove_process (MPServer *server, MPProcess *process)
{
	g_return_if_fail (server->pid_table != NULL);

	g_hash_table_remove (server->pid_table, GUINT_TO_POINTER (process->pid));
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
mp_server_get_process_clones (MPServer *server, MPProcess *process)
{
	CloneInfo ci;

	ci.parent = process;
	ci.result = NULL;
	
	g_hash_table_foreach (server->pid_table, (GHFunc)add_clone, &ci);

	return ci.result;
}
     
