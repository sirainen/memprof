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

#include <glib.h>
#include <gtk/gtkwidget.h>
#include <stdio.h>
#include <sys/types.h>
#include "bfd.h"

#ifndef __MEMPROF_H__
#define __MEMPROF_H__

typedef enum {
  BLOCK_MARKED  = 1 << 0,
  BLOCK_IS_ROOT = 1 << 1
} BlockFlags;

typedef struct {
  guint flags;
  void *addr;
  guint size;
  gint stack_size;
  void **stack;

  gint refcount;
} Block;

typedef struct {
  guint addr;
  guint size;
  gchar *name;
  bfd *abfd;
  GArray *symbols;
  long symcount;
  asymbol **syms;
  asection *section;
  gboolean do_offset : 1;
  gboolean prepared : 1;
} Map;

typedef struct {
  guint addr;
  guint size;
  gchar *name;
} Symbol;

typedef void (*SectionFunc) (void *addr, guint size, gpointer user_data);

typedef enum {
  ERROR_WARNING,
  ERROR_MODAL,
  ERROR_FATAL
} ErrorType;

void show_error	(GtkWidget *parent_window,
		 ErrorType error,
		 const gchar *format,
		 ...) G_GNUC_PRINTF (3, 4);

void     process_map_sections (Map           *map,
			       SectionFunc    func,
			       gpointer       user_data);
char *   demangle             (Map           *map,
			       const char    *name);
gboolean read_bfd             (Map           *map);
gboolean find_line            (Map           *map,
			       bfd_vma        addr,
			       const char   **filename,
			       char         **functionname,
			       unsigned int  *line);


/* Convert a device / inode pair into a file. Not needed for kernel-2.2 or greater.
 */
gchar *locate_inode (dev_t        device,
		     ino_t        inode);
void   read_inode   (const gchar *path);

#endif /* __MEMPROF_H__ */

