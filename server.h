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

#include <gtk/gtkobject.h>
#include <process.h>

#define MP_TYPE_SERVER            (mp_server_get_type ())
#define MP_SERVER(obj)            (GTK_CHECK_CAST ((obj), MP_TYPE_SERVER, MPServer))
#define MP_SERVER_CLASS(klass)    (GTK_CHECK_CLASS_CAST ((klass), GTK_TYPE_SERVER, MPServerClass))
#define MP_IS_SERVER(obj)         (GTK_CHECK_TYPE ((obj), MP_TYPE_SERVER))
#define MP_IS_SERVER_CLASS(klass) (GTK_CHECK_CLASS_TYPE ((klass), MP_TYPE_SERVER))
#define MP_SERVER_GET_CLASS(obj)  (GTK_CHECK_GET_CLASS ((obj), MP_TYPE_SERVER, MPServerClass))

typedef struct _MPServerClass MPServerClass;

GtkType   mp_server_get_type   (void);
MPServer *mp_server_new        (void);
int       mp_server_instrument (MPServer  *server,
				char     **args);

MPProcess *mp_server_find_process       (MPServer  *server,
					 pid_t      pid);
void       mp_server_add_process        (MPServer  *server,
					 MPProcess *process);
void       mp_server_remove_process     (MPServer  *server,
					 MPProcess *process);
GList *    mp_server_get_process_clones (MPServer  *server,
					 MPProcess *process);
