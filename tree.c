/* -*- mode: C; c-file-style: "linux" -*- */

/* MemProf -- memory profiler and leak detector
 * Copyright (C) 2000 Red Hat, Inc.
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

#include <glade/glade.h>
#include <gnome.h>

#include "gui.h"

static GtkWidget *tree_window;
static GtkWidget *tree_ctree;

extern char *glade_file;

static void
button_press_cb (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	GtkCList *clist = GTK_CLIST (widget);
	ProcessWindow *pwin;
	int row, column;
	
	if (event->window == clist->clist_window &&
	    event->type == GDK_2BUTTON_PRESS &&
	    gtk_clist_get_selection_info (clist, event->x, event->y, &row, &column)) {

		pwin = gtk_clist_get_row_data (clist, row);
		process_window_show_hide (pwin);
	}
}

static void
ensure_tree_window (void)
{
	if (!tree_window) {
		GladeXML *xml = glade_xml_new (glade_file, "TreeWindow");
		
		tree_window = glade_xml_get_widget (xml, "TreeWindow");
		gtk_window_set_default_size (GTK_WINDOW (tree_window), 400, 300);
		gtk_signal_connect (GTK_OBJECT (tree_window), "delete_event",
				    GTK_SIGNAL_FUNC (hide_and_check_quit), NULL);
		
		tree_ctree = glade_xml_get_widget (xml, "TreeWindow-ctree");

		gtk_signal_connect (GTK_OBJECT (tree_ctree), "button_press_event",
				    button_press_cb, NULL);
	}
}

void
tree_window_show (void)
{
	ensure_tree_window ();
	
	gtk_widget_show (tree_window);
	gdk_window_show (tree_window->window); /* Raise */
}

static int
compare_process (ProcessWindow *window, MPProcess *process)
{
	return process_window_get_process (window) == process ? 0 : 1;
}

static GtkCTreeNode *
find_node_by_process (MPProcess *process)
{
	return gtk_ctree_find_by_row_data_custom (GTK_CTREE (tree_ctree), NULL, process,
						  (GCompareFunc)compare_process);
}

static void
update_node (GtkCTreeNode *node)
{
	char buffer[32];
	char *cmdline;
	char *status;
	
	ProcessWindow *pwin = gtk_ctree_node_get_row_data (GTK_CTREE (tree_ctree), node);
	MPProcess *process = process_window_get_process (pwin);

	sprintf(buffer, "%d", process->pid);
	gtk_ctree_node_set_text (GTK_CTREE (tree_ctree), node, 0, buffer);

	cmdline = process_get_cmdline (process);
	gtk_ctree_node_set_text (GTK_CTREE (tree_ctree), node, 1, cmdline);
	g_free (cmdline);
	
	status = process_get_status_text (process);
	gtk_ctree_node_set_text (GTK_CTREE (tree_ctree), node, 2, status);
	g_free (status);
}

static void
status_changed_cb (MPProcess *process)
{
	GtkCTreeNode *node = find_node_by_process (process);
	update_node (node);
}

void
tree_window_add (ProcessWindow *window)
{
	MPProcess *process;
	GtkCTreeNode *parent_node = NULL;
	GtkCTreeNode *node;
	static const char *text[3] = { NULL, NULL, NULL };
	
	ensure_tree_window ();

	process = process_window_get_process (window);
	if (process->parent) {
		parent_node = find_node_by_process (process->parent);

		if (parent_node) {
			gtk_ctree_set_node_info (GTK_CTREE (tree_ctree), parent_node,
						 "", 0, NULL, NULL, NULL, NULL,
						 FALSE, TRUE);
			update_node (parent_node);
		}
	}

	node = gtk_ctree_insert_node (GTK_CTREE (tree_ctree), parent_node, NULL,
				      (char **)text, 0, NULL, NULL, NULL, NULL,
				      TRUE, TRUE);
	gtk_ctree_node_set_row_data (GTK_CTREE (tree_ctree), node, window);
	update_node (node);

	gtk_signal_connect (GTK_OBJECT (process), "status_changed",
			    GTK_SIGNAL_FUNC (status_changed_cb), NULL);
}

void
tree_window_remove (ProcessWindow *window)
{
	ensure_tree_window ();
}
