/* -*- mode: C; c-file-style: "linux" -*- */

/* MemProf -- memory profiler and leak detector
 * Copyright 1999, 2000, 2001, Red Hat, Inc.
 * Copyright 2002, Kristian Rietveld
 * Copyright 2002, Soeren Sandmann (sandmann@daimi.au.dk)
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

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

#include <glade/glade.h>

#include <gconf/gconf-client.h>

#include <regex.h>

#include "leakdetect.h"
#include "gui.h"
#include "memprof.h"
#include "process.h"
#include "profile.h"
#include "server.h"
#include <gnome.h>

struct _ProcessWindow {
	MPProcess *process;
	Profile *profile;
	GSList *leaks;

	GtkWidget *main_window;
	GtkWidget *main_notebook;
	GtkWidget *n_allocations_label;
	GtkWidget *profile_status_label;
	GtkWidget *bytes_per_label;
	GtkWidget *total_bytes_label;

	GtkWidget *profile_func_tree_view;
	GtkTreeViewColumn *profile_func_column0;
	GtkTreeViewColumn *profile_func_column1;
	GtkTreeViewColumn *profile_func_column2;

	GtkWidget *profile_caller_tree_view;
	GtkTreeViewColumn *profile_caller_column0;
	GtkTreeViewColumn *profile_caller_column1;
	GtkTreeViewColumn *profile_caller_column2;
	
	GtkWidget *profile_descendants_tree_view;
	GtkTreeViewColumn *profile_descendants_column0;
	GtkTreeViewColumn *profile_descendants_column1;
	GtkTreeViewColumn *profile_descendants_column2;
	GtkTreeViewColumn *profile_descendants_column3;

	GtkWidget *leak_block_clist;
	GtkWidget *leak_stack_clist;

	GtkWidget *usage_max_label;
	GtkWidget *usage_canvas;

	guint usage_max;
	guint usage_high;
	guint usage_leaked;

	GnomeCanvasItem *usage_frame;
	GnomeCanvasItem *usage_current_bar;
	GnomeCanvasItem *usage_high_bar;
	GnomeCanvasItem *usage_leak_bar;

	guint status_update_timeout;

	gboolean follow_fork : 1;
	gboolean follow_exec : 1;
};

char *glade_file;

MPServer *global_server;

static ProcessWindow *process_window_new (void);
static void process_window_destroy (ProcessWindow *pwin);
static void process_window_reset   (ProcessWindow *pwin);

static GSList *skip_funcs = NULL;
static GSList *skip_regexes = NULL;

static gboolean default_follow_fork = FALSE;
static gboolean default_follow_exec = FALSE;

char *stack_command = NULL;

GSList *process_windows = NULL;

static GladeXML *pref_dialog_xml = NULL; /* We save this around, so we can prevent multiple property boxes from being opened at once */

static MPProfileType profile_type;


/* Helper function to print a clear error message if there
 * is a problem looking up a particular widget
 */
static GtkWidget *
get_widget (GladeXML   *glade,
	    const char *name)
{
  GtkWidget *widget = glade_xml_get_widget (glade, name);
  if (!widget)
    g_error ("Cannot lookup %s\n", name);

  return widget;
}


/************************************************************
 * Status Page 
 ************************************************************/

static void
update_bars (ProcessWindow *pwin)
{
	gint bytes_used;

	if (pwin->process)
		bytes_used = pwin->process->bytes_used;
	else
		bytes_used = 0;
	
	gnome_canvas_item_set (pwin->usage_current_bar,
			       "x2", ((double)pwin->usage_canvas->allocation.width *
				      (double)bytes_used / pwin->usage_max),
			       NULL);
	gnome_canvas_item_set (pwin->usage_high_bar,
			       "x2", ((double)pwin->usage_canvas->allocation.width *
				      (double)pwin->usage_high / pwin->usage_max),
			       NULL);
	gnome_canvas_item_set (pwin->usage_leak_bar,
			       "x2", ((double)pwin->usage_canvas->allocation.width *
				      (double)pwin->usage_leaked / pwin->usage_max),
			       NULL);
}

static gboolean
update_status (gpointer data)
{
	char *tmp;
	ProcessWindow *pwin = (ProcessWindow *)data;

	tmp = g_strdup_printf ("%d", pwin->process->bytes_used);
	gtk_label_set_text (GTK_LABEL (pwin->total_bytes_label), tmp);
	g_free (tmp);

	tmp = g_strdup_printf ("%d", pwin->process->n_allocations);
	gtk_label_set_text (GTK_LABEL (pwin->n_allocations_label), tmp);
	g_free (tmp);

	tmp = g_strdup_printf ("%d samples", pwin->process->bytes_used);
	gtk_label_set_text (GTK_LABEL (pwin->profile_status_label), tmp);
	g_free (tmp);

	if (pwin->process->n_allocations == 0)
		tmp = g_strdup("-");
	else
		tmp = g_strdup_printf ("%.2f",
				       (double)pwin->process->bytes_used /
					       pwin->process->n_allocations);
	gtk_label_set_text (GTK_LABEL (pwin->bytes_per_label), tmp);
	g_free (tmp);

	if (pwin->process->bytes_used > pwin->usage_max) {
		while ((pwin->process->bytes_used > pwin->usage_max))
			pwin->usage_max *= 2;
	
		tmp = g_strdup_printf ("%dk", pwin->usage_max / 1024);
		gtk_label_set_text (GTK_LABEL (pwin->usage_max_label), tmp);
		g_free (tmp);
	}

	pwin->usage_high = MAX (pwin->process->bytes_used, pwin->usage_high);

	update_bars (pwin);
	
	return TRUE;
}

static void
usage_canvas_size_allocate (GtkWidget     *widget,
			    GtkAllocation *allocation,
			    ProcessWindow *pwin)
{
	gnome_canvas_set_scroll_region (GNOME_CANVAS (widget),
					0, 0,
					allocation->width, allocation->height);
		
	gnome_canvas_item_set (pwin->usage_frame,
			       "x2", (double)allocation->width - 1,
			       "y2", (double)allocation->height - 1,
			       NULL);
	gnome_canvas_item_set (pwin->usage_current_bar,
			       "y2", (double)allocation->height - 1,
			       NULL);
	gnome_canvas_item_set (pwin->usage_high_bar,
			       "y2", (double)allocation->height - 1,
			       NULL);
	gnome_canvas_item_set (pwin->usage_leak_bar,
			       "y2", (double)allocation->height - 1,
			       NULL);
}
					    


/************************************************************
 * GUI for profiles
 ************************************************************/

static gboolean
get_sort_info (GtkTreeView *view, int *sort_column, GtkSortType *sort_type)
{
	GtkTreeModel *model = gtk_tree_view_get_model (view);

	if (model && GTK_IS_TREE_SORTABLE (model))
		return gtk_tree_sortable_get_sort_column_id (
			GTK_TREE_SORTABLE (model), sort_column, sort_type);
	
	return FALSE;
}

static void
profile_func_list_goto_symbol (ProcessWindow *pwin, Symbol *symbol)
{
	GtkTreeModel *function_list;
	GtkTreeIter iter;
	gboolean found = FALSE;

	function_list = gtk_tree_view_get_model (
		GTK_TREE_VIEW (pwin->profile_func_tree_view));

	if (gtk_tree_model_get_iter_first (function_list, &iter)) {
		do {
			ProfileFunc *func;

			gtk_tree_model_get (function_list, &iter,
					    3, &func,
					    -1);

			if (func->node->symbol == symbol) {
				found = TRUE;
				break;
			}
		} while (gtk_tree_model_iter_next (function_list, &iter));
	}
	
	if (found) {
		GtkTreePath *path =
			gtk_tree_model_get_path (function_list, &iter);

		gtk_tree_view_set_cursor (
			GTK_TREE_VIEW (pwin->profile_func_tree_view), path, 0, FALSE);
	}
	gtk_widget_grab_focus (GTK_WIDGET (pwin->profile_func_tree_view));
}

static void
profile_descendants_row_activated (GtkTreeView *treeview,
				   GtkTreePath *path,
				   GtkTreeViewColumn *column,
				   gpointer data)
{
	GtkTreeModel *descendants_store;
	ProcessWindow *pwin = data;
	GtkTreeIter iter;
	Symbol *desc_symbol;

	descendants_store = gtk_tree_view_get_model (treeview);
	if (!gtk_tree_model_get_iter (descendants_store, &iter, path))
		return;

	gtk_tree_model_get (descendants_store, &iter, 4, &desc_symbol, -1);

	profile_func_list_goto_symbol (pwin, desc_symbol);
}

static void
profile_caller_row_activated (GtkTreeView *treeview,
			      GtkTreePath *path,
			      GtkTreeViewColumn *column,
			      gpointer data)
{
	GtkTreeModel *caller_list;
	ProcessWindow *pwin = data;
	GtkTreeIter iter;
	Symbol *caller_symbol;

	caller_list = gtk_tree_view_get_model (treeview);
	if (!gtk_tree_model_get_iter (caller_list, &iter, path))
		return;

	gtk_tree_model_get (caller_list, &iter, 3, &caller_symbol, -1);

	if (caller_symbol != GINT_TO_POINTER (-1))
		profile_func_list_goto_symbol (pwin, caller_symbol);
}

static void
add_node (GtkTreeStore *store, int n_samples,
	  const GtkTreeIter *parent, ProfileDescendantTreeNode *node)
{
	GtkTreeIter iter;
	gchar *name;
	int i;

	gtk_tree_store_insert (store, &iter, (GtkTreeIter *)parent, 0);

	if (node->symbol)
		name = node->symbol->name;
	else
		name = "???";

	gtk_tree_store_set (store, &iter,
			    0, name,
			    1, 100 * (double)node->self/n_samples,
			    2, 100 * (double)node->non_recursion/n_samples,
			    3, 100 * (double)node->total/n_samples,
			    4, node->symbol,
			    -1);

	for (i = 0; i < node->children->len; ++i)
		add_node (store, n_samples, &iter, node->children->pdata[i]);
}

static void
profile_selection_changed (GtkTreeSelection *selection, ProcessWindow *pwin)
{
	GtkListStore *store;
	GtkTreeIter selected;
	ProfileFunc *func;
	GtkTreeStore *tree_store;
	GtkListStore *list_store;
	GPtrArray *caller_list;
	ProfileDescendantTree *descendant_tree;
	int i;
	int n_samples = pwin->profile->n_bytes;
	int old_sort_column;
	GtkSortType old_sort_type;
	gboolean was_sorted;

	gtk_tree_selection_get_selected (selection, (GtkTreeModel **)&store, &selected);

	gtk_tree_model_get (GTK_TREE_MODEL (store), &selected,
			    3, &func,
			    -1);

	was_sorted = get_sort_info (GTK_TREE_VIEW (pwin->profile_descendants_tree_view),
				    &old_sort_column, &old_sort_type);
	
	/* fill descendants tree */
	tree_store = gtk_tree_store_new (
		5, G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_POINTER);

	descendant_tree = profile_func_create_descendant_tree (func);

	add_node (tree_store, n_samples, NULL, descendant_tree->roots->pdata[0]);
	
	gtk_widget_set_sensitive (GTK_WIDGET (pwin->profile_descendants_tree_view), TRUE);

	gtk_tree_view_set_model (GTK_TREE_VIEW (pwin->profile_descendants_tree_view),
				 GTK_TREE_MODEL (tree_store));

	if (was_sorted)
		gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (tree_store),
						      old_sort_column, old_sort_type);
	else
		gtk_tree_sortable_set_sort_column_id (
			GTK_TREE_SORTABLE (tree_store), 2, GTK_SORT_DESCENDING);

	gtk_tree_sortable_sort_column_changed (GTK_TREE_SORTABLE (tree_store));

	g_object_unref (G_OBJECT (tree_store));

	gtk_tree_view_column_set_sort_column_id (pwin->profile_descendants_column0, 0);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_descendants_column1, 1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_descendants_column2, 2);

	profile_descendant_tree_free (descendant_tree);

	/* fill caller tree */
	was_sorted = get_sort_info (GTK_TREE_VIEW (pwin->profile_caller_tree_view),
				    &old_sort_column, &old_sort_type);

	list_store = gtk_list_store_new (4, G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_POINTER);

 	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column0, -1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column1, -1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column2, -1);

	caller_list = profile_func_create_caller_list (func);

	for (i = 0; i < caller_list->len; ++i) {
		GtkTreeIter iter;
		gchar *name;
		ProfileFunc *caller = caller_list->pdata[i];

		if (caller->node) {
			if (caller->node->symbol)
				name = caller->node->symbol->name;
			else
				name = "???";
		}
		else
			name = "<spontaneous>";
			
		gtk_list_store_append (list_store, &iter);
		gtk_list_store_set (list_store, &iter,
				    0, name,
				    1, 100 * (double)caller->self / n_samples,
				    2, 100 * (double)caller->total / n_samples,
				    3, caller->node? caller->node->symbol : GINT_TO_POINTER (-1),
				    -1);
	}
	profile_caller_list_free (caller_list);

 	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column0, 0);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column1, 1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column2, 2);

	gtk_tree_view_set_model (GTK_TREE_VIEW (pwin->profile_caller_tree_view),
				 GTK_TREE_MODEL (list_store));

	if (was_sorted)
		gtk_tree_sortable_set_sort_column_id (
			GTK_TREE_SORTABLE (list_store), old_sort_column, old_sort_type);
	else
		gtk_tree_sortable_set_sort_column_id (
			GTK_TREE_SORTABLE (list_store), 2, GTK_SORT_DESCENDING);

	gtk_tree_sortable_sort_column_changed (GTK_TREE_SORTABLE (list_store));

	g_object_unref (G_OBJECT (list_store));
	
	gtk_widget_set_sensitive (GTK_WIDGET (pwin->profile_caller_tree_view), TRUE);
}

static void
profile_fill (ProcessWindow *pwin)
{
	GtkListStore *store;
	
	int i;
	int n_samples = pwin->profile->n_bytes;

	int old_sort_column;
	GtkSortType old_sort_type;
	gboolean was_sorted;

	was_sorted =
		get_sort_info (GTK_TREE_VIEW (pwin->profile_func_tree_view),
			       &old_sort_column, &old_sort_type);
	
	gtk_tree_view_set_model (GTK_TREE_VIEW (pwin->profile_func_tree_view), NULL);
 	store = gtk_list_store_new (4, G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_POINTER);

	gtk_tree_view_set_model (
		GTK_TREE_VIEW (pwin->profile_func_tree_view),
		GTK_TREE_MODEL (store));

	/* inserting in a ListStore is O(n) when sorting ... */
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column0, -1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column1, -1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column2, -1);

	for (i = 0; i < pwin->profile->functions->len; ++i) {
		GtkTreeIter iter;
		gchar *name;
		
		ProfileFunc *func = pwin->profile->functions->pdata[i];

		gtk_list_store_append (store, &iter);

		g_assert (func);

		if (func->node->symbol)
			name = func->node->symbol->name;
		else
			name = "???";
		
		gtk_list_store_set (store, &iter,
				    0, name,
				    1, 100 * (double)func->self / n_samples,
				    2, 100 * (double)func->total / n_samples,
				    3, func,
				    -1);
	}
	
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column0, 0);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column1, 1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column2, 2);

	if (was_sorted) {
		gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store), old_sort_column,
						      old_sort_type);
	}
	else {
		gtk_tree_sortable_set_sort_column_id (
			GTK_TREE_SORTABLE (store), 2, GTK_SORT_DESCENDING);
	}
	
	gtk_tree_sortable_sort_column_changed (GTK_TREE_SORTABLE (store));

	gtk_widget_set_sensitive (GTK_WIDGET (pwin->profile_func_tree_view), TRUE);

	g_object_unref (G_OBJECT (store));
}


/************************************************************
 * GUI for leak detection
 ************************************************************/

static void
leak_block_select_row (GtkWidget     *widget,
		       gint           row,
		       gint           column,
		       GdkEvent      *event,
		       ProcessWindow *pwin)
{
	Block *block = gtk_clist_get_row_data (GTK_CLIST (widget), row);
	StackElement *stack;

	if (block == NULL)
		return;

	gtk_clist_clear (GTK_CLIST (pwin->leak_stack_clist));
	for (stack = block->stack; !STACK_ELEMENT_IS_ROOT (stack); stack = stack->parent) {
		char *data[3];
		char buf[32];
		const char *filename;
		char *functionname;
		unsigned int line;
		
		if (!process_find_line (pwin->process, stack->address,
					&filename, &functionname, &line)) {
			/* 0x3f == '?' -- suppress trigraph warnings */
			functionname = g_strdup ("(\x3f\x3f\x3f)");
			filename = "(\x3f\x3f\x3f)";
			line = 0;
		}
		
		data[0] = functionname;
		
		g_snprintf(buf, 32, "%d", line);
		data[1] = buf;
		
		data[2] = (char *)filename;

		gtk_clist_append (GTK_CLIST (pwin->leak_stack_clist), data);
		free (data[0]);
	}
		
}

static void
leaks_fill (ProcessWindow *pwin, GtkCList *clist)
{
	gint row;
	GSList *tmp_list;

	gtk_clist_clear (clist);
	tmp_list = pwin->leaks;
	while (tmp_list) {
		char *data[3];
		char buf[32];
		const char *filename;
		char *functionname = NULL;
		unsigned int line;
		
		Block *block = tmp_list->data;
		StackElement *stack;

		for (stack = block->stack; !STACK_ELEMENT_IS_ROOT (stack); stack = stack->parent) {
			if (process_find_line (pwin->process, stack->address,
					       &filename, &functionname, &line)) {
				GSList *tmp_list;

				if (!functionname)
					continue;

				for (tmp_list = skip_funcs; tmp_list != NULL; tmp_list = tmp_list->next) {
					if (!strcmp (functionname, tmp_list->data)) {
						free (functionname);
						functionname = NULL;
						break;
					}
				}

				if (!functionname)
					continue;

				for (tmp_list = skip_regexes; tmp_list != NULL; tmp_list = tmp_list->next) {
					regex_t regex;

					regcomp (&regex, tmp_list->data, 0);

					if (!regexec (&regex, functionname, 0, NULL, 0)) {
						free (functionname);
						functionname = NULL;
						regfree (&regex);
						break;
					}

					regfree (&regex);
				}
			}
			
			if (functionname)
				break;
		}
		if (!functionname)
			functionname = g_strdup ("(\x3f\x3f\x3f)");
		
		g_snprintf(buf, 32, "%p", block->addr);
		data[0] = g_strdup (buf);
		
		g_snprintf(buf, 32, "%d", block->size);
		data[1] = g_strdup (buf);

		data[2] = functionname;
		
		row = gtk_clist_append (clist, data);
		gtk_clist_set_row_data (clist, row, block);

		g_free (data[0]);
		g_free (data[1]);
		free (data[2]);
		
		tmp_list = tmp_list->next;
	}

	if (clist->rows > 0)
		leak_block_select_row (GTK_WIDGET (clist), 0, 0, NULL, pwin);
}

static void
leak_stack_run_command (ProcessWindow *pwin, Block *block, int frame)
{
	const char *filename;
	char *functionname;
	unsigned int line;

	StackElement *stack = block->stack;
	while (frame--)
		stack = stack->parent;

	if (process_find_line (pwin->process, stack->address,
			       &filename, &functionname, &line)) {
		
		GString *command = g_string_new (NULL);
		char *p = stack_command;
		char buf[32];
		char *args[3];

		while (*p) {
			if (*p == '%') {
				switch (*++p) {
				case 'f':
					g_string_append (command, filename);
					break;
				case 'l':
					snprintf(buf, 32, "%d", line);
					g_string_append (command, buf);
					break;
				case '%':
					g_string_append_c (command, '%');
					break;
				default:
					g_string_append_c (command, '%');
					g_string_append_c (command, *p);
				}
			} else
					g_string_append_c (command, *p);
			p++;
		}
		
		free (functionname);

		args[0] = "/bin/sh";
		args[1] = "-c";
		args[2] = command->str;

		if (gnome_execute_async (NULL, 3, args) == -1) {
			show_error (pwin->main_window,
				    ERROR_MODAL, _("Executation of \"%s\" failed"),
				    command->str);
		}

		g_string_free (command, FALSE);
	}
}

static gboolean
leak_stack_button_press (GtkWidget      *widget,
			 GdkEventButton *event,
			 ProcessWindow  *pwin)
{
	if (event->window == GTK_CLIST (widget)->clist_window &&
	    event->type == GDK_2BUTTON_PRESS) {
		int my_row;

		if (!gtk_clist_get_selection_info (GTK_CLIST (widget),
						   event->x, event->y,
						   &my_row, NULL))
			return FALSE;

		if (my_row != -1) {
			Block *block;
			gint block_row;

			g_return_val_if_fail (GTK_CLIST (pwin->leak_block_clist)->selection, FALSE);

			block_row = GPOINTER_TO_INT (GTK_CLIST (pwin->leak_block_clist)->selection->data);
			block = gtk_clist_get_row_data (GTK_CLIST (pwin->leak_block_clist), block_row);

			leak_stack_run_command (pwin, block, my_row);
		}

		g_signal_stop_emission_by_name (G_OBJECT (widget), "button_press_event");
		return TRUE;
	}
	
	return FALSE;
}



/************************************************************
 * File Selection handling
 ************************************************************/

static void
filename_ok_clicked (GtkWidget *button, gchar **name)
{
	GtkWidget *fs = gtk_widget_get_ancestor (button,
						 gtk_file_selection_get_type());
  
	*name = g_strdup (gtk_file_selection_get_filename (GTK_FILE_SELECTION (fs)));
	gtk_widget_destroy (fs);
}

static gchar *
get_filename (const gchar *title, 
	      const gchar *prompt_text, 
	      const gchar *suggested_name)
{
	GtkWidget *fs;
	gchar *filename = NULL;
	
	fs = gtk_file_selection_new (title);
	gtk_file_selection_set_filename (GTK_FILE_SELECTION (fs), 
					 suggested_name);
	
	gtk_label_set_text (GTK_LABEL (GTK_FILE_SELECTION (fs)->selection_text),
			    prompt_text);
	gtk_signal_connect (GTK_OBJECT (GTK_FILE_SELECTION (fs)->ok_button),
			    "clicked",
			    GTK_SIGNAL_FUNC (filename_ok_clicked), &filename);
	gtk_signal_connect_object (GTK_OBJECT (GTK_FILE_SELECTION (fs)->cancel_button),
				   "clicked",
				   GTK_SIGNAL_FUNC (gtk_widget_destroy), 
				   GTK_OBJECT (fs));
	gtk_signal_connect (GTK_OBJECT (fs),
			    "destroy",
			    GTK_SIGNAL_FUNC (gtk_main_quit), NULL);
	
	gtk_widget_show (fs);
	gtk_main();
	
	return filename;
}

/* Really ugly utility function to retrieve the ProcessWindow from
 * either a menu_item or toolbar item.
 */
ProcessWindow *
pwin_from_widget (GtkWidget *widget)
{
	GtkWidget *app;

	if (GTK_IS_MENU_ITEM (widget)) {
		GtkWidget *menu_shell = widget->parent;

		while (menu_shell && !GTK_IS_MENU_BAR (menu_shell)) {
			menu_shell = gtk_menu_get_attach_widget (GTK_MENU (menu_shell))->parent;
		}
		g_assert (menu_shell != NULL);

		app = gtk_widget_get_toplevel (menu_shell);
	} else
		app = gtk_widget_get_toplevel (widget);
		
	return gtk_object_get_data (GTK_OBJECT (app), "process-window");
}

void
close_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = pwin_from_widget (widget);

	hide_and_check_quit (pwin->main_window);
}

void
exit_cb (GtkWidget *widget)
{
	gtk_main_quit ();
}

static void
reset_cb (MPProcess *process, ProcessWindow *pwin)
{
	process_window_reset  (pwin);
}

static void
status_changed_cb (MPProcess *process, ProcessWindow *pwin)
{
	if (process->status == MP_PROCESS_DEFUNCT ||
	    process->status == MP_PROCESS_DETACHED) {

		if (g_slist_length (process_windows) > 1) {
			tree_window_remove (pwin);
			process_window_destroy (pwin);
		} else { 
			tree_window_remove (pwin);
		
			if (pwin->process) {
				g_signal_handlers_disconnect_by_func (G_OBJECT (pwin->process), G_CALLBACK (status_changed_cb), pwin);
				g_signal_handlers_disconnect_by_func (G_OBJECT (pwin->process), G_CALLBACK (reset_cb), pwin);
				g_object_unref (G_OBJECT (pwin->process));
				pwin->process = NULL;
			}
			
			if (pwin->status_update_timeout) {
				g_source_remove (pwin->status_update_timeout);
				pwin->status_update_timeout = 0;
			}
			
			process_window_reset (pwin);
		}
		
	} else {
		char *status  = process_get_status_text (process);
		char *cmdline = process_get_cmdline (process);
		char *title = g_strdup_printf ("%s - %s (%d) - %s", _("MemProf"), cmdline, process->pid, status);
		gtk_window_set_title (GTK_WINDOW (pwin->main_window), title);
		
		g_free (title);
		g_free (status);
		g_free (cmdline);
	}
}

static void 
process_window_reset (ProcessWindow *pwin)
{
	if (pwin->profile) {
		profile_free (pwin->profile);
		pwin->profile = NULL;

		gtk_tree_view_set_model (
			GTK_TREE_VIEW (pwin->profile_func_tree_view), NULL);
		gtk_tree_view_set_model (
			GTK_TREE_VIEW (pwin->profile_caller_tree_view), NULL);
		gtk_tree_view_set_model (
			GTK_TREE_VIEW (pwin->profile_descendants_tree_view), NULL);

		gtk_widget_set_sensitive (GTK_WIDGET (pwin->profile_func_tree_view), FALSE);
		gtk_widget_set_sensitive (GTK_WIDGET (pwin->profile_caller_tree_view), FALSE);
		gtk_widget_set_sensitive (GTK_WIDGET (pwin->profile_descendants_tree_view), FALSE);
	}

	if (pwin->leaks) {
		g_slist_free (pwin->leaks);
		pwin->leaks = NULL;
		gtk_clist_clear (GTK_CLIST (pwin->leak_block_clist));
		gtk_clist_clear (GTK_CLIST (pwin->leak_stack_clist));
	}
	
	pwin->usage_max = 32*1024;
	pwin->usage_high = 0;
	pwin->usage_leaked = 0;
	
	gtk_window_set_title (GTK_WINDOW (pwin->main_window), "MemProf");
	update_bars (pwin);
}

static void
init_process (ProcessWindow *pwin, MPProcess *process)
{
	pwin->process = process;

	process_set_follow_fork (pwin->process, pwin->follow_fork);
	process_set_follow_exec (pwin->process, pwin->follow_exec);
	
	pwin->status_update_timeout =
		g_timeout_add (100,
			       update_status,
			       pwin);

	g_signal_connect (G_OBJECT (process), "status_changed",
			G_CALLBACK (status_changed_cb), pwin);
	g_signal_connect (G_OBJECT (process), "reset",
			G_CALLBACK (reset_cb), pwin);

	tree_window_add (pwin);
}

static void
process_created_cb (MPServer *server, MPProcess *process)
{
	ProcessWindow *pwin = process_window_new ();
	
	init_process (pwin, process);

	tree_window_show ();
}

static gboolean
run_file (ProcessWindow *pwin, char **args)
{
	gboolean result;
	char *path;
	
	g_return_val_if_fail (args != NULL, FALSE);
	g_return_val_if_fail (args[0] != NULL, FALSE);

	path = process_find_exec (args);
	
	if (path) {
		MPProcess *process = process_new (global_server);
		process_run (process, path, args);

		if (pwin->process) {
			pwin = process_window_new ();
			tree_window_show ();
		}

		init_process (pwin, process);
		
		gtk_widget_show (pwin->main_window);

		result = TRUE;
		
	} else {
		show_error (pwin->main_window,
			    ERROR_MODAL,
			    _("Cannot find executable for \"%s\""),
			    args[0]);
		result = FALSE;
	}

	g_free (path);
	return result;
}


void
run_cb (GtkWidget *widget)
{
       GladeXML *xml;
       GtkWidget *run_dialog;
       GtkWidget *entry;

       ProcessWindow *pwin = pwin_from_widget (widget);
       
       xml = glade_xml_new (glade_file, "RunDialog", NULL);
       run_dialog = get_widget (xml, "RunDialog");
       entry = get_widget (xml, "RunDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (get_widget (xml, "RunDialog-run")));

       g_object_unref (G_OBJECT (xml));

       while (1) {
	       gtk_window_set_transient_for (GTK_WINDOW (run_dialog),
			                     GTK_WINDOW (pwin->main_window));
	       if (gtk_dialog_run (GTK_DIALOG (run_dialog)) == 1) {
		       gchar **args;
		       char *text;
		       gboolean result;

		       text = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
		       args = process_parse_exec (text);

		       result = run_file (pwin, args);

		       g_strfreev (args);
		       g_free (text);

		       if (result)
			       break;
	       } else {
		       break;
	       }
       }

       gtk_widget_destroy (run_dialog);
}

void
kill_cb (GtkWidget *widget)
{
       ProcessWindow *pwin = pwin_from_widget (widget);

       if (pwin->process)
	       process_window_maybe_kill (pwin);
}

void
detach_cb (GtkWidget *widget)
{
       ProcessWindow *pwin = pwin_from_widget (widget);

       if (pwin->process)
	       process_window_maybe_detach (pwin);
}

void
process_tree_cb (GtkWidget *widget)
{
	tree_window_show ();
}

void
save_leak_cb (GtkWidget *widget)
{
	static gchar *suggestion = NULL;
	gchar *filename;
	
	ProcessWindow *pwin = pwin_from_widget (widget);
       
	if (pwin->leaks) {
		filename = get_filename ("Save Leak Report", "Output file",
					 suggestion ? suggestion : "memprof.leak");
		if (filename) {
			g_free (suggestion);
			suggestion = filename;
			
			leaks_print (pwin->process, pwin->leaks, filename);
		}
	}
}

void
save_profile_cb (GtkWidget *widget)
{
	show_error (pwin_from_widget (widget)->main_window,
		    ERROR_WARNING, _("Saving is disabled at the moment"));
	
#if 0
	static gchar *suggestion = NULL;
	gchar *filename;

	ProcessWindow *pwin = pwin_from_widget (widget);
       
	if (pwin->profile) {
		filename = get_filename ("Save Profile", "Output file",
					 suggestion ? suggestion : "memprof.out");
		if (filename) {
			g_free (suggestion);
			suggestion = filename;
			
			profile_write (pwin->profile, filename);
		}
	}
#endif
}

void
save_current_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = pwin_from_widget (widget);
       
	switch (gtk_notebook_get_current_page (GTK_NOTEBOOK (pwin->main_notebook))) {
	case 0:
		save_profile_cb (widget);
		break;
	case 1:
		save_leak_cb (widget);
		break;
	}
}

void
generate_leak_cb (GtkWidget *widget)
{
	GSList *tmp_list;
	ProcessWindow *pwin = pwin_from_widget (widget);
       
	
	if (pwin->process) {
		process_stop_input (pwin->process);

		if (pwin->leaks)
			g_slist_free (pwin->leaks);
		pwin->leaks = leaks_find (pwin->process);

		pwin->usage_leaked = 0;
		tmp_list = pwin->leaks;
		while (tmp_list) {
			pwin->usage_leaked += ((Block *)tmp_list->data)->size;
			tmp_list = tmp_list->next;
		}
		
		leaks_fill (pwin, GTK_CLIST (pwin->leak_block_clist));
		gtk_notebook_set_page (GTK_NOTEBOOK (pwin->main_notebook), 1);

		process_start_input (pwin->process);
	}
}

void
generate_profile_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = pwin_from_widget (widget);
       
	if (pwin->process) {
		process_stop_input (pwin->process);

		if (pwin->profile) {
			profile_free (pwin->profile);
			pwin->profile = NULL;
		}

		pwin->profile = profile_create (pwin->process, skip_funcs);
		process_start_input (pwin->process);
		profile_fill (pwin);

		gtk_notebook_set_page (GTK_NOTEBOOK (pwin->main_notebook), 0);
	}
}

void
reset_profile_cb (GtkWidget *widget)
{
	ProcessWindow *pwin = pwin_from_widget (widget);

	process_window_reset (pwin);

	if (pwin->process)
		process_clear_input (pwin->process);
}

static void
string_view_init (GtkTreeView *tree_view)
{
	GtkListStore *store = gtk_list_store_new (1, G_TYPE_STRING);
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	gtk_tree_view_set_model (tree_view, GTK_TREE_MODEL (store));

	column = gtk_tree_view_column_new ();

	renderer = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_set_attributes (column, renderer, "text", 0, NULL);

	gtk_tree_view_append_column (tree_view, column);
}

static void
string_view_fill (GtkTreeView *tree_view,
		  GSList      *strings)
{
	GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
	GtkListStore *store = GTK_LIST_STORE (model);
	GSList *tmp_list;

	gtk_list_store_clear (store);
	
	for (tmp_list = strings; tmp_list != NULL; tmp_list = tmp_list->next) {
		GtkTreeIter iter;
		gtk_list_store_append (store, &iter);
		gtk_list_store_set (store, &iter, 0, tmp_list->data, -1);
	}
}

static void
skip_fill (void)
{
	if (pref_dialog_xml) {
		GtkWidget *tree_view = get_widget (pref_dialog_xml, "skip-tree-view");
		string_view_fill (GTK_TREE_VIEW (tree_view), skip_funcs);
	}
}

static void
skip_add_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GladeXML *xml;
       GtkWidget *dialog;
       GtkWidget *entry;
       char *text;

       GtkWidget *skip_tree_view = get_widget (preferences_xml, "skip-tree-view");
       GtkTreeModel *skip_model = gtk_tree_view_get_model (GTK_TREE_VIEW (skip_tree_view));
       GtkWidget *property_box = get_widget (preferences_xml, "Preferences");

       xml = glade_xml_new (glade_file, "SkipAddDialog", NULL);
       dialog = get_widget (xml, "SkipAddDialog");
       entry = get_widget (xml, "SkipAddDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (get_widget (xml, "SkipAddDialog-add")));

       g_object_unref (G_OBJECT (xml));

       while (1) {
               gtk_window_set_transient_for (GTK_WINDOW (dialog),
				             GTK_WINDOW (property_box));

	       if (gtk_dialog_run (GTK_DIALOG (dialog)) == 1) {
		       text = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);

		       if (strchr (text, ' ')) {
			       GtkWidget *error = gnome_error_dialog_parented ("Function names cannot contain spaces",									       GTK_WINDOW (dialog));
			       gnome_dialog_run (GNOME_DIALOG (error));
			       g_free (text);
		       } else if (strlen (text) == 0) {
			       GtkWidget *error = gnome_error_dialog_parented ("Function name cannot be blank",
									       GTK_WINDOW (dialog));
			       gnome_dialog_run (GNOME_DIALOG (error));
			       g_free (text);
		       } else {
			       GtkTreeIter iter;
			       gtk_list_store_append (GTK_LIST_STORE (skip_model), &iter);
			       gtk_list_store_set (GTK_LIST_STORE (skip_model), &iter,
						   0, text,
						   -1);

			       skip_funcs = g_slist_append (skip_funcs, g_strdup (text));
			       gconf_client_set_list (gconf_client_get_default (),
						      "/apps/memprof/skip_funcs",
						      GCONF_VALUE_STRING, skip_funcs, NULL);

			       g_free (text);

			       break;
		       }
	       } else {
		       break;
	       }
       }

       gtk_widget_destroy (dialog);
}

static int
iter_get_index (GtkTreeModel *model,
	       GtkTreeIter  *iter)
{
	GtkTreePath *path = gtk_tree_model_get_path (model,iter);
	int result;
	
	g_assert (path);
	g_assert (gtk_tree_path_get_depth (path) == 1);
	result = gtk_tree_path_get_indices (path)[0];
	gtk_tree_path_free (path);

	return result;

}

static void
skip_delete_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GtkWidget *skip_tree_view = get_widget (preferences_xml, "skip-tree-view");
       GtkTreeSelection *skip_selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (skip_tree_view));
       GtkTreeModel *skip_model;
       GtkTreeIter iter;

	if (gtk_tree_selection_get_selected (skip_selection, &skip_model, &iter)) {
		int selected_row = iter_get_index (skip_model, &iter);
		GSList *tmp_list;
		
		gtk_list_store_remove (GTK_LIST_STORE (skip_model), &iter);
		
		tmp_list = g_slist_nth (skip_funcs, selected_row);
		g_free (tmp_list->data);
		skip_funcs = g_slist_delete_link (skip_funcs, tmp_list);

		gconf_client_set_list (gconf_client_get_default (),
				       "/apps/memprof/skip_funcs",
				       GCONF_VALUE_STRING, skip_funcs, NULL);
	}
}

static void
skip_defaults_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GConfClient *c = gconf_client_get_default ();

       g_slist_free (skip_funcs);
       skip_funcs = NULL;

       /* we unset this here, notification will update it */
       gconf_client_unset (c, "/apps/memprof/skip_funcs", NULL);
}

static void
skip_regexes_fill (void)
{
	if (pref_dialog_xml) {
		GtkWidget *tree_view = get_widget (pref_dialog_xml, "skip-regexes-tree-view");
		string_view_fill (GTK_TREE_VIEW (tree_view), skip_regexes);
	}
}

static void
skip_regexes_add_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GladeXML *xml;
       GtkWidget *dialog;
       GtkWidget *entry;
       char *text;

       GtkWidget *regexes_tree_view = get_widget (preferences_xml, "skip-tree-view");
       GtkTreeModel *regexes_model = gtk_tree_view_get_model (GTK_TREE_VIEW (regexes_tree_view));
       GtkWidget *property_box = get_widget (preferences_xml, "Preferences");

       xml = glade_xml_new (glade_file, "SkipRegexesAddDialog", NULL);
       dialog = get_widget (xml, "SkipRegexesAddDialog");
       entry = get_widget (xml, "SkipRegexesAddDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (get_widget (xml, "SkipRegexesAddDialog-add")));

       g_object_unref (G_OBJECT (xml));

       while (1) {
               gtk_window_set_transient_for (GTK_WINDOW (dialog),
                                             GTK_WINDOW (property_box));

	       if (gtk_dialog_run (GTK_DIALOG (dialog)) == 1) {
		       text = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
		       
		       if (strchr (text, ' ')) {
			       GtkWidget *error = gnome_error_dialog_parented ("Function names cannot contain spaces",									       GTK_WINDOW (dialog));
			       gnome_dialog_run (GNOME_DIALOG (error));
			       g_free (text);
		       } else if (strlen (text) == 0) {
			       GtkWidget *error = gnome_error_dialog_parented ("Function name cannot be blank",
									       GTK_WINDOW (dialog));
			       gnome_dialog_run (GNOME_DIALOG (error));
			       g_free (text);
		       } else {
			       GtkTreeIter iter;
			       gtk_list_store_append (GTK_LIST_STORE (regexes_model), &iter);
			       gtk_list_store_set (GTK_LIST_STORE (regexes_model), &iter,
						   0, text,
						   -1);

			       skip_regexes = g_slist_append (skip_regexes,
							      g_strdup (text));
			       gconf_client_set_list (gconf_client_get_default (),
						      "/apps/memprof/skip_regexes",
						      GCONF_VALUE_STRING, skip_regexes, NULL);

			       g_free (text);

			       break;
		       }
	       } else {
		       break;
	       }
       }

       gtk_widget_destroy (dialog);
}

static void
skip_regexes_delete_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GtkWidget *regexes_tree_view = get_widget (preferences_xml, "skip-regexes-tree-view");
       GtkTreeSelection *regexes_selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (regexes_tree_view));
       GtkTreeModel *regexes_model;
       GtkTreeIter iter;

	if (gtk_tree_selection_get_selected (regexes_selection, &regexes_model, &iter)) {
		int selected_row = iter_get_index (regexes_model, &iter);
		GSList *tmp_list;
		
		gtk_list_store_remove (GTK_LIST_STORE (regexes_model), &iter);
		
		tmp_list = g_slist_nth (skip_regexes, selected_row);
		g_free (tmp_list->data);
		skip_regexes = g_slist_delete_link (skip_regexes, tmp_list);

		gconf_client_set_list (gconf_client_get_default (),
				       "/apps/memprof/skip_regexes",
				       GCONF_VALUE_STRING, skip_regexes, NULL);
	}
}

static void
skip_regexes_defaults_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GConfClient *c = gconf_client_get_default ();

       g_slist_free (skip_regexes);
       skip_regexes = NULL;

       /* we unset this there, notification will update it */
       gconf_client_unset (c, "/apps/memprof/skip_regexes", NULL);
}

static void
stack_command_entry_update (GtkWidget *entry,
			    gpointer data)
{
	const gchar *new_value;

	new_value = gtk_entry_get_text (GTK_ENTRY (entry));

	if (new_value)
		gconf_client_set_string (gconf_client_get_default (),
					 "/apps/memprof/stack_command",
					 new_value, NULL);
}

static void
stack_command_entry_focus_out (GtkWidget *entry,
			       GdkEventFocus *ev,
			       gpointer data)
{
	stack_command_entry_update (entry, NULL);
}

void
preferences_destroy_cb (GtkWidget *widget,
			GObject **xml)
{
	g_assert( xml != NULL );

	g_object_unref(*xml);
	/* Mark the property box as destroyed so we don't try to touch it again */
	*xml = NULL;
}

void
preferences_response_cb (GtkWidget *w, int response, gpointer data)
{
	if (response == GTK_RESPONSE_CLOSE)
		gtk_widget_destroy (w);
}

void
preferences_cb (GtkWidget *widget)
{
       static GtkWidget *property_box = NULL;
       GtkWidget *skip_tree_view, *regexes_tree_view;
       GtkWidget *stack_command_entry;
       GtkWidget *button;
       GladeXML *xml;

       /* Check for an open property box */
       if (pref_dialog_xml != NULL)
       {
		gdk_window_show( GTK_WIDGET(property_box)->window );
		gdk_window_raise( GTK_WIDGET(property_box)->window );
		return;
       }

       pref_dialog_xml = glade_xml_new (glade_file, "Preferences", NULL);
       xml = pref_dialog_xml;
       
       stack_command_entry = get_widget (xml, "stack-command-entry");

       property_box = gtk_dialog_new_with_buttons ("Preferences",
						   NULL,
						   GTK_DIALOG_DESTROY_WITH_PARENT,
						   GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
						   NULL);
       gtk_widget_reparent (get_widget (xml, "notebook2"),
                            GTK_DIALOG (property_box)->vbox);

       skip_tree_view = get_widget (xml, "skip-tree-view");
       string_view_init (GTK_TREE_VIEW (skip_tree_view));
       skip_fill ();

       regexes_tree_view = get_widget (xml, "skip-regexes-tree-view");
       string_view_init (GTK_TREE_VIEW (regexes_tree_view));
       skip_regexes_fill ();

       gtk_entry_set_text (GTK_ENTRY (stack_command_entry), stack_command);
       g_signal_connect (G_OBJECT (stack_command_entry), "activate",
			 G_CALLBACK (stack_command_entry_update), NULL);
       g_signal_connect (G_OBJECT (stack_command_entry), "focus_out_event",
			 G_CALLBACK (stack_command_entry_focus_out), NULL);

       button = get_widget (xml, "skip-add-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_add_cb), xml);
       button = get_widget (xml, "skip-delete-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_delete_cb), xml);
       button = get_widget (xml, "skip-defaults-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_defaults_cb), xml);

       button = get_widget (xml, "skip-regexes-add-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_regexes_add_cb), xml);
       button = get_widget (xml, "skip-regexes-delete-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_regexes_delete_cb), xml);
       button = get_widget (xml, "skip-regexes-defaults-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_regexes_defaults_cb), xml);

       glade_xml_signal_autoconnect (xml);

       g_signal_connect (G_OBJECT (property_box), "destroy",
			 G_CALLBACK (preferences_destroy_cb), &(pref_dialog_xml));
       g_signal_connect (G_OBJECT (property_box), "response",
			 G_CALLBACK (preferences_response_cb), NULL);

       gtk_widget_show_all (property_box);
}

static void
preferences_list_notify (GConfClient *client,
			 guint cnx_id,
			 GConfEntry *entry,
			 gpointer user_data)
{
	const gchar *key = gconf_entry_get_key (entry);
	
	if (strcmp (key, "/apps/memprof/skip_funcs") == 0) {
		if (skip_funcs)
			g_slist_free (skip_funcs);
		skip_funcs = gconf_client_get_list (client, key,
						    GCONF_VALUE_STRING, NULL);
		skip_fill ();
	} else if (strcmp (key, "/apps/memprof/skip_regexes") == 0) {
		if (skip_regexes)
			g_slist_free (skip_regexes);
		skip_regexes = gconf_client_get_list (client, key,
						      GCONF_VALUE_STRING, NULL);
		skip_regexes_fill ();
	}
}

static void
preferences_stack_command_notify (GConfClient *client,
				  guint cnx_id,
				  GConfEntry *entry,
				  gpointer user_data)
{
	if (stack_command)
		g_free (stack_command);
	stack_command = gconf_client_get_string (client,
						 "/apps/memprof/stack_command",
						 NULL);

	if (pref_dialog_xml && stack_command) {
		GtkWidget *e;

		e = get_widget (pref_dialog_xml, "stack-command-entry");
		gtk_entry_set_text (GTK_ENTRY (e), stack_command);
	}
}

void
follow_fork_cb (GtkWidget *widget)
{
       ProcessWindow *pwin = pwin_from_widget (widget);
       pwin->follow_fork = GTK_CHECK_MENU_ITEM (widget)->active;
       if (pwin->process)
	       process_set_follow_fork (pwin->process, pwin->follow_fork);
}

void
follow_exec_cb (GtkWidget *widget)
{
       ProcessWindow *pwin = pwin_from_widget (widget);
       pwin->follow_exec = GTK_CHECK_MENU_ITEM (widget)->active;
       if (pwin->process)
	       process_set_follow_exec (pwin->process, pwin->follow_exec);
}

void
about_cb (GtkWidget *widget)
{
       GladeXML *xml;
       GtkWidget *dialog;

       ProcessWindow *pwin = pwin_from_widget (widget);
	
       xml = glade_xml_new (glade_file, "About", NULL);
       dialog = get_widget (xml, "About");
       g_object_unref (G_OBJECT (xml));

       gtk_window_set_transient_for (GTK_WINDOW (dialog),
                                     GTK_WINDOW (pwin->main_window));
       g_object_set (G_OBJECT (dialog), "version", VERSION, NULL);

       gtk_widget_show (dialog);
}

static void
show_error_response (GtkDialog *dialog,
		     gint       response_id,
		     gpointer   user_data)
{
	if (response_id == GTK_RESPONSE_OK)
		gtk_widget_destroy (GTK_WIDGET (dialog));
}

void
show_error (GtkWidget *parent_window,
	    ErrorType error,
	    const gchar *format,
	    ...)
{
	va_list args;
	char *message;
	GtkWidget *dialog;

	va_start (args, format);
	vasprintf (&message, format, args);
	va_end (args);

	dialog = gtk_message_dialog_new (parent_window ? GTK_WINDOW (parent_window) : NULL,
					 GTK_DIALOG_DESTROY_WITH_PARENT,
					 (error == ERROR_FATAL) ?
					   GTK_MESSAGE_ERROR :
					   GTK_MESSAGE_WARNING,
					 GTK_BUTTONS_OK, message);
	free (message);

	gtk_window_set_title (GTK_WINDOW (dialog),
			      (error == ERROR_FATAL) ?
			      _("MemProf Error") : _("MemProf Warning"));

	if (error == ERROR_WARNING) {
		gtk_widget_show (dialog);
		g_signal_connect (dialog, "response",
				  G_CALLBACK (show_error_response), NULL);
	} else {
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);
	}

	if (error == ERROR_FATAL)
		exit(1);
}

static void
set_white_bg (GtkWidget *widget)
{
	GdkColor white = { 0, 0xffff, 0xffff, 0xffff };
	gtk_widget_modify_bg (widget, GTK_STATE_NORMAL, &white);
}

static void
process_window_free (ProcessWindow *pwin)
{
	/* FIXME: we leak the process structure */
	
	if (pwin->leaks)
		g_slist_free (pwin->leaks);

	if (pwin->profile)
		profile_free (pwin->profile);

	process_windows = g_slist_remove (process_windows, pwin);
	if (!process_windows)
		gtk_main_quit ();
	
	g_free (pwin);
}


static void
process_window_destroy (ProcessWindow *pwin)
{
	if (pwin->status_update_timeout)
		g_source_remove (pwin->status_update_timeout);

	gtk_widget_destroy (pwin->main_window);
	check_quit ();
}

typedef struct {
	gint column;
	ProcessWindow *pwin;
} ColumnInfo;

static void
double_to_text (GtkTreeViewColumn *tree_column,
		GtkCellRenderer *cell, GtkTreeModel *tree_model,
		GtkTreeIter *iter, gpointer data)
{
	gdouble d;
	gchar *text;
	ColumnInfo *info = data;

	gtk_tree_model_get (tree_model, iter, info->column, &d, -1);

	if (profile_type == MP_PROFILE_MEMORY)
		text = g_strdup_printf ("%d", (int)(d * info->pwin->profile->n_bytes));
	else
		text = g_strdup_printf ("%.2f", d);

        g_object_set (cell, "text", text, NULL);
        g_free (text);
}

static GtkTreeViewColumn *
add_two_digit_column (GtkTreeView *view, const gchar *title, gint model_column, ProcessWindow *pwin)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	ColumnInfo *column_info = g_new (ColumnInfo, 1);

	renderer = gtk_cell_renderer_text_new ();

	column = gtk_tree_view_column_new ();
	gtk_tree_view_column_set_title  (column, title);
	gtk_tree_view_column_pack_start (column, renderer, TRUE);
	gtk_tree_view_column_set_resizable (column, TRUE);

	column_info->column = model_column;
	column_info->pwin = pwin;
	
	gtk_tree_view_column_set_cell_data_func (
		column, renderer, double_to_text, column_info, g_free);

	gtk_tree_view_append_column (view, column);

	return column;
}

static GtkTreeViewColumn *
add_plain_text_column (GtkTreeView *view, const gchar *title, gint model_column)
{
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;

	renderer = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (
		title, renderer, "text", model_column, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (view, column);

	return column;
}

static void
setup_profile_tree_view (ProcessWindow *pwin, GtkTreeView *tree_view)
{
	GtkTreeSelection *selection;
	
	pwin->profile_func_column0 = add_plain_text_column (tree_view, "Name", 0);
	pwin->profile_func_column1 = add_two_digit_column (tree_view, "Self", 1, pwin);
	pwin->profile_func_column2 = add_two_digit_column (tree_view, "Total", 2, pwin);

	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column0, 0);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column1, 1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_func_column2, 2);

	selection = gtk_tree_view_get_selection (tree_view);
	g_signal_connect (selection, "changed", G_CALLBACK (profile_selection_changed), pwin);

	gtk_widget_set_sensitive (GTK_WIDGET (tree_view), FALSE);
}

static void
setup_profile_descendants_tree_view (ProcessWindow *pwin, GtkTreeView *tree_view)
{
	pwin->profile_descendants_column0 = add_plain_text_column (tree_view, "Name", 0);
	pwin->profile_descendants_column1 = add_two_digit_column (tree_view, "Self", 1, pwin);
	pwin->profile_descendants_column2 = add_two_digit_column (tree_view, "Cummulative", 2, pwin);

	gtk_tree_view_column_set_sort_column_id (pwin->profile_descendants_column0, 0);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_descendants_column1, 1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_descendants_column2, 2);

	gtk_widget_set_sensitive (GTK_WIDGET (pwin->profile_descendants_tree_view), FALSE);

	g_signal_connect (G_OBJECT (tree_view), "row-activated",
			  G_CALLBACK (profile_descendants_row_activated), pwin);

	gtk_widget_set_sensitive (GTK_WIDGET (tree_view), FALSE);
}

static void
setup_profile_caller_tree_view (ProcessWindow *pwin, GtkTreeView *tree_view)
{
	pwin->profile_caller_column0 = add_plain_text_column (tree_view, "Name", 0);
	pwin->profile_caller_column1 = add_two_digit_column (tree_view, "Self", 1, pwin);
	pwin->profile_caller_column2 = add_two_digit_column (tree_view, "Total", 2, pwin);
	
	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column0, 0);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column1, 1);
	gtk_tree_view_column_set_sort_column_id (pwin->profile_caller_column2, 2);

	g_signal_connect (G_OBJECT (tree_view), "row-activated",
			  G_CALLBACK (profile_caller_row_activated), pwin);

	gtk_widget_set_sensitive (GTK_WIDGET (tree_view), FALSE);
}

static ProcessWindow *
process_window_new (void)
{
	gchar *fullfilename;
	GladeXML *xml;
	GtkWidget *vpaned;
	GtkWidget *hpaned;
	ProcessWindow *pwin;
	GtkWidget *menuitem;
	
	pwin = g_new0 (ProcessWindow, 1);
	process_windows = g_slist_prepend (process_windows, pwin);
	
	pwin->process = NULL;
	pwin->profile = NULL;
	pwin->leaks = NULL;
	
	pwin->usage_max = 32*1024;
	pwin->usage_high = 0;
	pwin->usage_leaked = 0;
	
	xml = glade_xml_new (glade_file, "MainWindow", NULL);
	
	pwin->main_window = get_widget (xml, "MainWindow");
	fullfilename = gnome_program_locate_file (NULL, GNOME_FILE_DOMAIN_PIXMAP, "memprof.png", FALSE, NULL);
	gnome_window_icon_set_from_file (GTK_WINDOW (pwin->main_window),
					 fullfilename);
	g_free (fullfilename);
	
	gtk_signal_connect (GTK_OBJECT (pwin->main_window), "delete_event",
			    GTK_SIGNAL_FUNC (hide_and_check_quit), pwin);
	
	
	gtk_window_set_default_size (GTK_WINDOW (pwin->main_window), 500, 400);
	
	gtk_object_set_data_full (GTK_OBJECT (pwin->main_window),
				  "process-window",
				  pwin, (GDestroyNotify)process_window_free);
	
	pwin->main_notebook = get_widget (xml, "main-notebook");
	
	pwin->follow_fork = default_follow_fork;
	pwin->follow_exec = default_follow_exec;
	
	menuitem = get_widget (xml, "follow-fork");
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (menuitem), pwin->follow_fork);
	menuitem = get_widget (xml, "follow-exec");
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (menuitem), pwin->follow_exec);
	
	pwin->n_allocations_label = get_widget (xml, "n-allocations-label");
	pwin->bytes_per_label = get_widget (xml, "bytes-per-label");
	pwin->total_bytes_label = get_widget (xml, "total-bytes-label");

	pwin->profile_status_label = get_widget (xml, "profile-status-label");
	
	/* setup profile tree views */
	pwin->profile_func_tree_view = get_widget (xml, "profile-func-tree-view");
	pwin->profile_descendants_tree_view =
		get_widget (xml, "profile-descendants-tree-view");
	pwin->profile_caller_tree_view = get_widget (xml, "profile-caller-tree-view");

	setup_profile_tree_view (pwin, GTK_TREE_VIEW (pwin->profile_func_tree_view));
	setup_profile_descendants_tree_view (pwin, GTK_TREE_VIEW (pwin->profile_descendants_tree_view));
	setup_profile_caller_tree_view (pwin, GTK_TREE_VIEW (pwin->profile_caller_tree_view));

	/* leak clists */
	pwin->leak_block_clist = get_widget (xml, "leak-block-clist");
	pwin->leak_stack_clist =  get_widget (xml, "leak-stack-clist");
	
	gtk_clist_set_column_width (GTK_CLIST (pwin->leak_stack_clist), 0, 250);
	gtk_clist_set_column_width (GTK_CLIST (pwin->leak_stack_clist), 2, 500);
	
	gtk_signal_connect (GTK_OBJECT (pwin->leak_block_clist), "select_row",
			    GTK_SIGNAL_FUNC (leak_block_select_row), pwin);
	gtk_signal_connect (GTK_OBJECT (pwin->leak_stack_clist), "button_press_event",
			    GTK_SIGNAL_FUNC (leak_stack_button_press), pwin);
	
	pwin->usage_max_label = get_widget (xml, "usage-max-label");
	pwin->usage_canvas = get_widget (xml, "usage-canvas");
	
	set_white_bg (pwin->usage_canvas);
	gtk_signal_connect (GTK_OBJECT (pwin->usage_canvas), "size_allocate",
			    GTK_SIGNAL_FUNC (usage_canvas_size_allocate), pwin);
	
	pwin->usage_high_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (pwin->usage_canvas)),
						      gnome_canvas_rect_get_type (),
						      "x1", 0.0,
						      "y1", 0.0,
						      "outline_color", "black", 
						      "fill_color", "blue",
						      NULL);
	pwin->usage_current_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (pwin->usage_canvas)),
							 gnome_canvas_rect_get_type (),
							 "x1", 0.0,
							 "y1", 0.0,
							 "outline_color", "black", 
							 "fill_color", "yellow",
							 NULL);
	pwin->usage_leak_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (pwin->usage_canvas)),
						      gnome_canvas_rect_get_type (),
						      "x1", 0.0,
						      "y1", 0.0,
						      "outline_color", "black", 
						      "fill_color", "red",
						      NULL);
	pwin->usage_frame = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (pwin->usage_canvas)),
						   gnome_canvas_rect_get_type (),
						   "x1", 0.0,
						   "y1", 0.0,
						   "outline_color", "black", 
						   "fill_color", NULL,
						   NULL);
	
	vpaned = get_widget (xml, "profile-vpaned");
	gtk_paned_set_position (GTK_PANED (vpaned), 150);

	hpaned = get_widget (xml, "profile-hpaned");
	gtk_paned_set_position (GTK_PANED (hpaned), 150);
	
	vpaned = get_widget (xml, "leaks-vpaned");
	gtk_paned_set_position (GTK_PANED (vpaned), 150);
	
	/* If profiling time, not memory, hide all GUI related to leak
	 * detection.
	 */
	if (profile_type != MP_PROFILE_MEMORY) {
		gtk_widget_hide (get_widget (xml, "leaks-vpaned"));
		gtk_widget_hide (get_widget (xml, "toolbar-leaks-button"));
		gtk_widget_hide (get_widget (xml, "save-leak-info"));
		gtk_widget_hide (get_widget (xml, "generate-leak-report"));
		gtk_widget_hide (get_widget (xml, "allocation-bar"));
		gtk_notebook_set_show_tabs (
			GTK_NOTEBOOK (get_widget (xml, "main-notebook")), FALSE);
	}
	else {
		gtk_widget_hide (get_widget (xml, "profile-status-label"));
		gtk_widget_hide (get_widget (xml, "reset-profile-button"));
	}
	
	glade_xml_signal_autoconnect (xml);
	g_object_unref (G_OBJECT (xml));
	
	return pwin;
}


MPProcess *
process_window_get_process (ProcessWindow *pwin)
{
	return pwin->process;
}

gboolean
process_window_visible (ProcessWindow *pwin)
{
	return GTK_WIDGET_VISIBLE (pwin->main_window);
}

void
process_window_show (ProcessWindow *pwin)
{
	if (!process_window_visible (pwin))
		gtk_widget_show (pwin->main_window);
	else
		gdk_window_show (pwin->main_window->window);
}

void
process_window_hide (ProcessWindow *pwin)
{
	if (process_window_visible (pwin)) 
		hide_and_check_quit (pwin->main_window);
}

void
check_quit (void)
{
	GList *toplevels, *tmplist;
	
	tmplist = toplevels = gtk_window_list_toplevels ();
	while (tmplist) {
		if (GTK_WIDGET_VISIBLE (toplevels->data))
			return;
		tmplist = tmplist->next;
	}

	g_list_free (toplevels);

	gtk_main_quit ();
}

gboolean
hide_and_check_quit (GtkWidget *window)
{
	gtk_widget_hide (window);
	check_quit ();

	return TRUE;
}


void
process_window_maybe_detach (ProcessWindow *pwin)
{
	GtkWidget *dialog;
	const char *message;
	gint response;

	if (pwin->process->status == MP_PROCESS_EXITING)
		message = _("Really detach from finished process?");
	else
		message = _("Really detach from running process?");

	dialog = gtk_message_dialog_new (GTK_WINDOW (pwin->main_window),
					 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION,
					 GTK_BUTTONS_YES_NO,
					 message);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_YES);

	response = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);

	if (response == GTK_RESPONSE_YES)
		process_detach (pwin->process);
}


void
process_window_maybe_kill (ProcessWindow *pwin)
{
	if (pwin->process->status == MP_PROCESS_EXITING)
		process_window_maybe_detach (pwin);
	else {
		GtkWidget *dialog;
		gint response;

		dialog = gtk_message_dialog_new (GTK_WINDOW (pwin->main_window),
						 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
						 GTK_MESSAGE_QUESTION,
						 GTK_BUTTONS_YES_NO,
						 _("Really kill running process?"));
		gtk_dialog_set_default_response (GTK_DIALOG (dialog),
						 GTK_RESPONSE_YES);

		response = gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);

		if (response == GTK_RESPONSE_YES)
			process_kill (pwin->process);
	}
}

void
sigchld_handler (int signum)
{
	int old_errno = errno;
	
	while (1) {
		int pid = waitpid (WAIT_ANY, NULL, WNOHANG);
		if (pid < 0 && errno != ECHILD)
			g_error ("waitpid: %s\n", g_strerror (errno));
		else if (pid <= 0)
			break;
	}

	errno = old_errno;
}

int
main(int argc, char **argv)
{
       static char *profile_type_string = NULL;
       static char *profile_rate_string = NULL;
       static int profile_interval = 1000;

       static const struct poptOption memprof_popt_options [] = {
	       { "follow-fork", '\0', POPT_ARG_NONE, &default_follow_fork, 0,
		 N_("Create new windows for forked processes"), NULL },
	       { "follow-exec", '\0', POPT_ARG_NONE, &default_follow_exec, 0,
		 N_("Retain windows for processes after exec()"), NULL },
	       { "profile", '\0', POPT_ARG_STRING, &profile_type_string, 0,
		 N_("Type of profiling information to collect"), "memory/cycles/time" },
	       { "rate", '\0', POPT_ARG_STRING, &profile_rate_string, 0,
		 N_("Number of samples/sec for time profile (1k=1000)"), NULL },
	       { NULL, '\0', 0, NULL, 0 },
       };
       poptContext ctx;

       GnomeProgram *program;
       const char **startup_args;
       ProcessWindow *initial_window;
	

       /* Set up a handler for SIGCHLD to avoid zombie children
	*/
       signal (SIGCHLD, sigchld_handler);

       bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
       textdomain (GETTEXT_PACKAGE);

#ifdef HAVE_BIND_TEXTDOMAIN_CODESET
       bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
#endif

       program = gnome_program_init (PACKAGE, VERSION,
				     LIBGNOMEUI_MODULE,
				     argc, argv,
				     GNOME_PARAM_POPT_TABLE, memprof_popt_options,
				     NULL);

       /* If the user didn't specify the profile type explicitely,
	* we guess from the executable name.
	*/
       if (!profile_type_string) {
	       char *basename;
	       basename = g_path_get_basename (argv[0]);
	       
	       if (strcmp (basename, "speedprof") == 0)
		       profile_type_string = "cycles";
	       else
		       profile_type_string = "memory";

	       g_free (basename);
       }

       if (strcmp (profile_type_string, "memory") == 0)
	       profile_type = MP_PROFILE_MEMORY;
       else if (strcmp (profile_type_string, "cycles") == 0)
	       profile_type = MP_PROFILE_CYCLES;
       else if (strcmp (profile_type_string, "time") == 0)
	       profile_type = MP_PROFILE_TIME;
       else {
	       g_printerr (_("Argument of --profile must be one of 'memory', 'cycles, or 'time'\n"));
	       exit (1);
       }
	       
       if (profile_rate_string) {
	       int multiplier = 1;
	       double rate;
	       int len = strlen (profile_rate_string);
	       char suffix[2] = { '\0', '\0' };
	       char *end;
	       
	       if (len > 0 &&
		   (profile_rate_string[len - 1] == 'k' ||
		    profile_rate_string[len - 1] == 'K')) {
		       suffix[0] = profile_rate_string[len - 1];
		       multiplier = 1000;
		       profile_rate_string[len - 1] = '\0';
	       }

	       rate = strtod (profile_rate_string, &end);
	       if (len == 0 || *end != '\0' ||
		   rate * multiplier <= 1 || rate * multiplier > 1000000) {
		       g_printerr ("Invalid rate: %s%s\n",
				   profile_rate_string, suffix);
		       exit (1);
	       }

	       profile_interval = (int) (0.5 + 1000000 / (rate * multiplier));
       }

       g_object_get (G_OBJECT (program),
		     GNOME_PARAM_POPT_CONTEXT, &ctx,
		     NULL);

       glade_gnome_init ();

       glade_file = "./memprof.glade";
       if (!g_file_exists (glade_file)) {
	       glade_file = g_concat_dir_and_file (DATADIR, "memprof.glade");
       }
       if (!g_file_exists (glade_file)) {
	       show_error (NULL, ERROR_FATAL, _("Cannot find memprof.glade"));
       }

       global_server = mp_server_new ();
       mp_server_set_profile_type (global_server, profile_type);
       mp_server_set_interval (global_server, profile_interval);
       
       g_signal_connect (G_OBJECT (global_server), "process_created",
		       G_CALLBACK (process_created_cb), NULL);
       
       initial_window = process_window_new ();

       gconf_client_add_dir (gconf_client_get_default (),
		             "/apps/memprof",
			     GCONF_CLIENT_PRELOAD_NONE, NULL);
       skip_funcs = gconf_client_get_list (gconf_client_get_default (),
					   "/apps/memprof/skip_funcs",
					   GCONF_VALUE_STRING, NULL);
       skip_regexes = gconf_client_get_list (gconf_client_get_default (),
					     "/apps/memprof/skip_regexes",
					     GCONF_VALUE_STRING, NULL);
       stack_command = gconf_client_get_string (gconf_client_get_default (),
						"/apps/memprof/stack_command",
						NULL);
       gconf_client_notify_add (gconf_client_get_default (),
			        "/apps/memprof/stack_command",
			        preferences_stack_command_notify,
			        NULL, NULL, NULL);
       gconf_client_notify_add (gconf_client_get_default (),
				"/apps/memprof/skip_funcs",
				preferences_list_notify,
				NULL, NULL, NULL);
       gconf_client_notify_add (gconf_client_get_default (),
				"/apps/memprof/skip_regexes",
				preferences_list_notify,
				NULL, NULL, NULL);

       gtk_widget_show (initial_window->main_window);

       startup_args = poptGetArgs (ctx);
       if (startup_args)
	       run_file (initial_window, (char **)startup_args);
       poptFreeContext (ctx);

       gtk_main ();

       return 0;
}
