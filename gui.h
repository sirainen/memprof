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

#include "process.h"

typedef struct _ProcessWindow ProcessWindow;

void tree_window_show   (void);
void tree_window_add    (ProcessWindow *window);
void tree_window_remove (ProcessWindow *window);

MPProcess * process_window_get_process  (ProcessWindow *pwin);
gboolean    process_window_visible      (ProcessWindow *pwin);
MPProcess * process_window_get_process  (ProcessWindow *pwin);
void        process_window_show         (ProcessWindow *pwin);
void        process_window_hide         (ProcessWindow *pwin);

void        process_window_maybe_kill   (ProcessWindow *pwin);
void        process_window_maybe_detach (ProcessWindow *pwin);

gboolean hide_and_check_quit (GtkWidget *window);
void     check_quit          (void);
