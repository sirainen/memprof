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

static GtkWidget *
make_menu_item (const char *label, GtkSignalFunc cb)
{
	GtkWidget *menu_item = gtk_menu_item_new_with_label (label);
	gtk_signal_connect (GTK_OBJECT (menu_item), "activate", cb, NULL);
	gtk_widget_show (menu_item);
	
	return menu_item;
}

static ProcessWindow *
get_process_window (GtkWidget *menu_item)
{
	return gtk_object_get_data (GTK_OBJECT (menu_item->parent), "process-window");
}

static void
show_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = get_process_window (widget);

	process_window_show (pwin);
}

static void
hide_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = get_process_window (widget);

	process_window_hide (pwin);
}

static void
tree_detach_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = get_process_window (widget);
	MPProcess *process = process_window_get_process (pwin);

	process_detach (process);
}

static void
tree_kill_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = get_process_window (widget);

	process_window_maybe_kill (pwin);
}

static void
popup_menu (ProcessWindow *pwin, gint button, guint32 time)
{
	static GtkWidget *menu = NULL;
	static GtkWidget *show_item = NULL;
	static GtkWidget *hide_item = NULL;

	if (!menu) {
		menu = gtk_menu_new ();

		show_item = make_menu_item (_("Show"), GTK_SIGNAL_FUNC (show_cb));
		gtk_menu_append (GTK_MENU (menu), show_item);

		hide_item = make_menu_item (_("Hide"), GTK_SIGNAL_FUNC (hide_cb));
		gtk_menu_append (GTK_MENU (menu), hide_item);
		
		gtk_menu_append (GTK_MENU (menu),
				 make_menu_item (_("Kill"), GTK_SIGNAL_FUNC (tree_kill_cb)));

		gtk_menu_append (GTK_MENU (menu),
				 make_menu_item (_("Detach"), GTK_SIGNAL_FUNC (tree_detach_cb)));
	}

	if (process_window_visible (pwin)) {
		gtk_widget_set_sensitive (show_item, FALSE);
		gtk_widget_set_sensitive (hide_item, TRUE);
	} else {
		gtk_widget_set_sensitive (show_item, TRUE);
		gtk_widget_set_sensitive (hide_item, FALSE);
	}
	
	gtk_object_set_data (GTK_OBJECT (menu), "process-window", pwin);
	gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL, button, time);
}

static gboolean
button_press_cb (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	GtkCList *clist = GTK_CLIST (widget);
	ProcessWindow *pwin;
	int row, column;
	
	if (event->window == clist->clist_window) {
		if (!gtk_clist_get_selection_info (clist, event->x, event->y, &row, &column))
			return FALSE;

		pwin = gtk_clist_get_row_data (clist, row);
		
		if (event->button == 1 &&  event->type == GDK_2BUTTON_PRESS) {
			process_window_show (pwin);
			return TRUE;
			
		} else if (event->button == 3) {
			popup_menu (pwin, event->button, event->time);
			return TRUE;
		}
	}

	return FALSE;
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
				    GTK_SIGNAL_FUNC (button_press_cb), NULL);
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

static void
reparent_func (GtkCTree     *ctree,
	       GtkCTreeNode *node,
	       gpointer      data)
{
	GtkCTreeNode *parent = data;
	if (node != parent) {
		GtkCTreeNode *grandparent = GTK_CTREE_ROW (parent)->parent;
		gtk_ctree_move (ctree, node, grandparent, parent);
	}
}

void
tree_window_remove (ProcessWindow *window)
{
	GtkCTreeNode *node;
	MPProcess *process;
	
	ensure_tree_window ();

	process = process_window_get_process (window);
	node = find_node_by_process (process);

	gtk_ctree_post_recursive_to_depth (GTK_CTREE (tree_ctree), node,
					   GTK_CTREE_ROW (node)->level + 1,
					   reparent_func, node);

	gtk_ctree_remove_node (GTK_CTREE (tree_ctree), node);

	gtk_signal_disconnect_by_func (GTK_OBJECT (process), GTK_SIGNAL_FUNC (status_changed_cb), NULL);
}
