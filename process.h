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

#ifndef __PROCESS_H__
#define __PROCESS_H__

#include <stdio.h>
#include <glib.h>
#include <unistd.h>

#include "memprof.h"

typedef struct _MPProcess MPProcess;

typedef MPProcess *(*ProcessCreateFunc) (MPProcess *parent, pid_t pid);

struct _MPProcess
{
  pid_t pid;
  gchar *program_name;

  MPProcess *clone_of;
  guint seqno;	
	
  guint bytes_used;
  guint n_allocations;

  gint input_tag;
  GIOChannel *input_channel;

  GList *map_list;
  GList *bad_pages;
  GHashTable *block_table;

  GList *command_queue;

  gboolean follow_fork;
  gboolean follow_exec;
	
  ProcessCreateFunc create_func;
};

void        process_init            (ProcessCreateFunc   cfunc);
MPProcess * process_new             (void);
void        process_set_follow_fork (MPProcess          *process,
				     gboolean            follow_fork);
void        process_set_follow_exec (MPProcess          *process,
				     gboolean            follow_exec);
void        process_run             (MPProcess          *process,
				     const char         *path,
				     char              **args);
MPProcess * process_duplicate       (MPProcess          *process);
void        process_sections        (MPProcess          *process,
				     SectionFunc         func,
				     gpointer            user_data);
char **     process_parse_exec      (const char         *exec_string);
char *      process_find_exec       (char              **args);
GList *     process_get_clones      (MPProcess          *process);
void        process_start_input     (MPProcess          *process);
void        process_stop_input      (MPProcess          *process);
gboolean    process_find_line       (MPProcess          *process,
				     void               *address,
				     const char        **filename,
				     char              **functionname,
				     unsigned int       *line);
void        process_dump_stack      (MPProcess          *process,
				     FILE               *out,
				     gint                stack_size,
				     void              **stack);
Symbol *    process_locate_symbol   (MPProcess          *process,
				     guint               addr);

#endif /* __PROCESS_H__ */
