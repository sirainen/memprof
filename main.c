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

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <glade/glade.h>
#include <gnome.h>

#include "leakdetect.h"
#include "memprof.h"
#include "process.h"
#include "profile.h"

typedef struct _ProcessWindow ProcessWindow;

struct _ProcessWindow {
	MPProcess *process;
	Profile *profile;
	GSList *leaks;

	GtkWidget *main_window;
	GtkWidget *main_notebook;
	GtkWidget *n_allocations_label;
	GtkWidget *bytes_per_label;
	GtkWidget *total_bytes_label;

	GtkWidget *profile_func_clist;
	GtkWidget *profile_caller_clist;
	GtkWidget *profile_child_clist;

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

#define DEFAULT_SKIP "g_malloc g_malloc0 g_realloc g_strdup strdup strndup"
#define DEFAULT_STACK_COMMAND "emacsclient -n +%l \"%f\""

static ProcessWindow *process_window_new (void);
static void process_window_destroy (ProcessWindow *pwin);

static int n_skip_funcs;
static char **skip_funcs;

static gboolean default_follow_fork = FALSE;
static gboolean default_follow_exec = FALSE;

char *stack_command;


/************************************************************
 * Status Page 
 ************************************************************/

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
	
	gnome_canvas_item_set (pwin->usage_current_bar,
			       "x2", ((double)pwin->usage_canvas->allocation.width *
				      (double)pwin->process->bytes_used / pwin->usage_max),
			       NULL);
	gnome_canvas_item_set (pwin->usage_high_bar,
			       "x2", ((double)pwin->usage_canvas->allocation.width *
				      (double)pwin->usage_high / pwin->usage_max),
			       NULL);
	gnome_canvas_item_set (pwin->usage_leak_bar,
			       "x2", ((double)pwin->usage_canvas->allocation.width *
				      (double)pwin->usage_leaked / pwin->usage_max),
			       NULL);
	
	return TRUE;
}

static void
usage_canvas_size_allocate (GtkWidget     *widget,
			    GtkAllocation *allocation,
			    ProcessWindow *pwin)
{
	if (!pwin->usage_frame) {
		pwin->usage_high_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
							      gnome_canvas_rect_get_type (),
							      "x1", 0.0,
							      "y1", 0.0,
							      "outline_color", "black", 
							      "fill_color", "blue",
							      NULL);
		pwin->usage_current_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
								 gnome_canvas_rect_get_type (),
								 "x1", 0.0,
								 "y1", 0.0,
								 "outline_color", "black", 
								 "fill_color", "yellow",
								 NULL);
		pwin->usage_leak_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
							 gnome_canvas_rect_get_type (),
							 "x1", 0.0,
							 "y1", 0.0,
							 "outline_color", "black", 
							 "fill_color", "red",
							 NULL);
		pwin->usage_frame = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
							   gnome_canvas_rect_get_type (),
							   "x1", 0.0,
							   "y1", 0.0,
							   "outline_color", "black", 
							   "fill_color", NULL,
							   NULL);
	}

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

static gint
profile_compare_name (GtkCList     *clist,
		      gconstpointer ptr1,
		      gconstpointer ptr2)
{
	ProfileFunc *func1 = ((const GtkCListRow *)ptr1)->data;
	ProfileFunc *func2 = ((const GtkCListRow *)ptr2)->data;

	return strcmp (func1->symbol->name, func2->symbol->name);
}
							     
static gint
profile_compare_self (GtkCList     *clist,
		      gconstpointer ptr1,
		      gconstpointer ptr2)
{
	ProfileFunc *func1 = ((const GtkCListRow *)ptr1)->data;
	ProfileFunc *func2 = ((const GtkCListRow *)ptr2)->data;

	return func1->self < func2->self ? 1 :
		(func1->self > func2->self ? -1 : 0);
}
							     
static gint
profile_compare_total (GtkCList     *clist,
		       gconstpointer ptr1,
		       gconstpointer ptr2)
{
	ProfileFunc *func1 = ((const GtkCListRow *)ptr1)->data;
	ProfileFunc *func2 = ((const GtkCListRow *)ptr2)->data;

	return func1->total < func2->total ? 1 :
		(func1->total > func2->total ? -1 : 0);
}

static void
profile_func_click_column (GtkWidget *clist, gint column)
{
	static const GtkCListCompareFunc compare_funcs[] = {
		profile_compare_name,
		profile_compare_self,
		profile_compare_total
	};

	g_return_if_fail (column >= 0 && column < 3);

	gtk_clist_set_compare_func (GTK_CLIST (clist), compare_funcs[column]);
	gtk_clist_sort (GTK_CLIST (clist));
}
							     
static void
profile_fill_ref_clist (GtkWidget *clist, GList *refs)
{
	gint row;
	GList *tmp_list;

	gtk_clist_clear (GTK_CLIST (clist));
	tmp_list = refs;
	while (tmp_list) {
		char *data[2];
		char buf[32];
		
		ProfileFuncRef *ref = tmp_list->data;

		data[0] = ref->function->symbol->name;
		g_snprintf(buf, 32, "%d", ref->bytes);
		data[1] = buf;

		row = gtk_clist_append (GTK_CLIST (clist), data);
		gtk_clist_set_row_data (GTK_CLIST (clist), row, ref);

		tmp_list = tmp_list->next;
	}
}

static void
profile_func_select_row (GtkWidget     *widget,
			 gint           row,
			 gint           column,
			 GdkEvent      *event,
			 ProcessWindow *pwin)
{
	ProfileFunc *function = gtk_clist_get_row_data (GTK_CLIST (widget), row);

	if (function == NULL)
		return;

	profile_fill_ref_clist (pwin->profile_child_clist, function->children);
	profile_fill_ref_clist (pwin->profile_caller_clist, function->inherited);
}

static gboolean
profile_ref_button_press (GtkWidget      *widget,
			  GdkEventButton *event,
			  ProcessWindow  *pwin)
{
	if (event->window == GTK_CLIST (widget)->clist_window &&
	    event->type == GDK_2BUTTON_PRESS) {
		int my_row, function_row;

		if (!gtk_clist_get_selection_info (GTK_CLIST (widget),
						   event->x, event->y,
						   &my_row, NULL))
			return FALSE;

		if (my_row != -1) {
			ProfileFuncRef *ref;
			ref = gtk_clist_get_row_data (GTK_CLIST (widget), my_row);

			function_row = gtk_clist_find_row_from_data (GTK_CLIST (pwin->profile_func_clist), ref->function);

			if (function_row != -1) {
				gtk_clist_select_row (GTK_CLIST (pwin->profile_func_clist), function_row, 0);
				gtk_clist_moveto (GTK_CLIST (pwin->profile_func_clist),
						  function_row, -1, 0.5, 0.0);
			}
		}

		gtk_signal_emit_stop_by_name (GTK_OBJECT (widget), "button_press_event");
		return TRUE;
	}
	
	return FALSE;
}

static void
profile_fill (ProcessWindow *pwin, GtkCList *clist)
{
	int i, row;

	gtk_clist_clear (clist);
	for (i=0; i<pwin->profile->n_functions; i++) {
		ProfileFunc *function = pwin->profile->functions[i];
		char *data[3];
		char buf[32];
		
		data[0] = function->symbol->name;
		g_snprintf(buf, 32, "%u", function->self);
		data[1] = g_strdup (buf);

		g_snprintf(buf, 32, "%u", function->total);
		data[2] = g_strdup (buf);

		row = gtk_clist_append (clist, data);

		g_free (data[1]);
		g_free (data[2]);

		gtk_clist_set_row_data (clist, row, function);
	}

	if (clist->rows > 0)
		profile_func_select_row (GTK_WIDGET (clist), 0, 0, NULL, pwin);
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
	int i;

	if (block == NULL)
		return;

	gtk_clist_clear (GTK_CLIST (pwin->leak_stack_clist));
	for (i = 0; i < block->stack_size; i++) {
		char *data[3];
		char buf[32];
		const char *filename;
		char *functionname;
		unsigned int line;
		
		if (!process_find_line (pwin->process, block->stack[i],
					&filename, &functionname, &line)) {
			functionname = g_strdup ("(???)");
			filename = "(???)";
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
		int frame;
		
		Block *block = tmp_list->data;

		for (frame = 0 ; frame < block->stack_size ; frame++) {
			if (process_find_line (pwin->process, block->stack[frame],
					       &filename, &functionname, &line)) {
				int i;
				for (i = 0; i < n_skip_funcs; i++) {
					if (!strcmp (functionname, skip_funcs[i])) {
						free (functionname);
						functionname = NULL;
						break;
					}
				}
			}
			
			if (functionname)
				break;
		}
		if (!functionname)
			functionname = g_strdup ("(???)");
		
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

	if (process_find_line (pwin->process, block->stack[frame],
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
			show_error (ERROR_MODAL, _("Executation of \"%s\" failed"),
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

		gtk_signal_emit_stop_by_name (GTK_OBJECT (widget), "button_press_event");
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
			menu_shell = GTK_MENU (menu_shell)->parent_menu_item->parent;
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
       
	process_window_destroy (pwin);
}

void
exit_cb (GtkWidget *widget)
{
	GtkWidget *dialog;

	ProcessWindow *pwin = pwin_from_widget (widget);
       
	dialog = gnome_message_box_new (_("Really quit MemProf?"), GNOME_MESSAGE_BOX_QUESTION,
					GNOME_STOCK_BUTTON_YES, GNOME_STOCK_BUTTON_NO, NULL);

	gnome_dialog_set_parent (GNOME_DIALOG (dialog), GTK_WINDOW (pwin->main_window));
	
	if (gnome_dialog_run (GNOME_DIALOG (dialog)) == 0)
		gtk_main_quit ();
}

static MPProcess *
create_child_process (MPProcess *parent_process, pid_t pid)
{
	ProcessWindow *pwin = process_window_new ();
	if (parent_process)
		pwin->process = process_duplicate (parent_process);
	else
		pwin->process = process_new ();

	pwin->process->pid = pid;
	
	pwin->status_update_timeout =
		g_timeout_add (100,
			       update_status,
			       pwin);

	gtk_widget_show (pwin->main_window);
	
	return pwin->process;
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
		pwin->process = process_new ();
		process_run (pwin->process, path, args);
		
		if (!pwin->status_update_timeout)
			pwin->status_update_timeout =
				g_timeout_add (100,
					       update_status,
					       pwin);
		result = TRUE;
		
	} else {
		show_error (ERROR_MODAL,
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
       
       xml = glade_xml_new (glade_file, "RunDialog");
       run_dialog = glade_xml_get_widget (xml, "RunDialog");
       entry = glade_xml_get_widget (xml, "RunDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (glade_xml_get_widget (xml, "RunDialog-run")));

       gtk_object_destroy (GTK_OBJECT (xml));

       while (1) {
	       gnome_dialog_set_parent (GNOME_DIALOG (run_dialog),
					GTK_WINDOW (pwin->main_window));
	       if (gnome_dialog_run (GNOME_DIALOG (run_dialog)) == 0) {
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

		pwin->profile = profile_create (pwin->process, skip_funcs, n_skip_funcs);
		process_start_input (pwin->process);
		profile_fill (pwin, GTK_CLIST (pwin->profile_func_clist));

		gtk_notebook_set_page (GTK_NOTEBOOK (pwin->main_notebook), 0);
	}
}

static void
preferences_apply_cb (GtkWidget *widget, gint page_num, GladeXML *preferences_xml)
{
	GtkWidget *skip_clist = glade_xml_get_widget (preferences_xml,
						      "skip-clist");
	GtkWidget *stack_command_entry = glade_xml_get_widget (preferences_xml,
							       "stack-command-entry");

	gchar *text;
	int i;

	for (i=0; i < n_skip_funcs; i++)
		g_free (skip_funcs[i]);
	g_free (skip_funcs);

	n_skip_funcs = GTK_CLIST (skip_clist)->rows;
	skip_funcs = g_new (gchar *, n_skip_funcs);
	
	for (i=0; i < n_skip_funcs; i++) {
		gchar *text;
		gtk_clist_get_text (GTK_CLIST (skip_clist), i, 0, &text);
		skip_funcs[i] = g_strdup (text);
	}

	gnome_config_set_vector ("/MemProf/Options/skip_funcs",
				 n_skip_funcs, (const char **const)skip_funcs);

	text = gtk_editable_get_chars (GTK_EDITABLE (stack_command_entry), 0, -1);
	if (stack_command)
		g_free (stack_command);
	stack_command = text;
	gnome_config_set_string ("/MemProf/Options/stack_command", text);
	
	gnome_config_sync ();
}

static void
skip_fill (GtkWidget *clist)
{
       int i;

       gtk_clist_clear (GTK_CLIST (clist));

       for (i=0; i<n_skip_funcs; i++) {
	       gtk_clist_append (GTK_CLIST (clist), &skip_funcs[i]);
       }
}

static void
skip_add_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GladeXML *xml;
       GtkWidget *dialog;
       GtkWidget *entry;
       char *text;

       GtkWidget *skip_clist = glade_xml_get_widget (preferences_xml, "skip-clist");
       GtkWidget *property_box = glade_xml_get_widget (preferences_xml, "Preferences");

       xml = glade_xml_new (glade_file, "SkipAddDialog");
       dialog = glade_xml_get_widget (xml, "SkipAddDialog");
       entry = glade_xml_get_widget (xml, "SkipAddDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (glade_xml_get_widget (xml, "SkipAddDialog-add")));

       gtk_object_destroy (GTK_OBJECT (xml));

       while (1) {
	       gnome_dialog_set_parent (GNOME_DIALOG (dialog),
					GTK_WINDOW (property_box));
	       if (gnome_dialog_run (GNOME_DIALOG (dialog)) == 0) {
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
			       gtk_clist_append (GTK_CLIST (skip_clist), &text);
			       g_free (text);
			       
			       gnome_property_box_changed (GNOME_PROPERTY_BOX (property_box));
			       break;
		       }
			       
	       } else {
		       break;
	       }
       }

       gtk_widget_destroy (dialog);
	
}

static void
skip_delete_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GtkWidget *clist = glade_xml_get_widget (preferences_xml, "skip-clist");
       GtkWidget *property_box = glade_xml_get_widget (preferences_xml, "Preferences");

	if (GTK_CLIST (clist)->selection) {
		gint selected_row = GPOINTER_TO_INT (GTK_CLIST (clist)->selection->data);
		gtk_clist_remove (GTK_CLIST (clist), selected_row);

		gnome_property_box_changed (GNOME_PROPERTY_BOX (property_box));
	}
}

static void
skip_defaults_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GtkWidget *clist = glade_xml_get_widget (preferences_xml, "skip-clist");
       GtkWidget *property_box = glade_xml_get_widget (preferences_xml, "Preferences");

	gchar **funcs = g_strsplit (DEFAULT_SKIP, " ", -1);
	int i;

	gtk_clist_clear (GTK_CLIST (clist));
	for (i=0; funcs[i]; i++) {
	       gtk_clist_append (GTK_CLIST (clist), &funcs[i]);
	}

	g_strfreev (funcs);

	gnome_property_box_changed (GNOME_PROPERTY_BOX (property_box));
}

void
preferences_cb (GtkWidget *widget)
{
       GladeXML *xml;
       GtkWidget *skip_clist;
       GtkWidget *property_box;
       GtkWidget *stack_command_entry;
       GtkWidget *button;

       xml = glade_xml_new (glade_file, "Preferences");
       skip_clist = glade_xml_get_widget (xml, "skip-clist");
       stack_command_entry = glade_xml_get_widget (xml, "stack-command-entry");
       property_box = glade_xml_get_widget (xml, "Preferences");

       skip_fill (skip_clist);

       gtk_entry_set_text (GTK_ENTRY (stack_command_entry), stack_command);
       gtk_signal_connect_object (GTK_OBJECT (stack_command_entry),
				  "changed",
				  GTK_SIGNAL_FUNC (gnome_property_box_changed),
				  GTK_OBJECT (property_box));
       
       button = glade_xml_get_widget (xml, "skip-add-button");
       gtk_signal_connect (GTK_OBJECT (button), "clicked",
			   GTK_SIGNAL_FUNC (skip_add_cb), xml);

       button = glade_xml_get_widget (xml, "skip-delete-button");
       gtk_signal_connect (GTK_OBJECT (button), "clicked",
			   GTK_SIGNAL_FUNC (skip_delete_cb), xml);

       button = glade_xml_get_widget (xml, "skip-defaults-button");
       gtk_signal_connect (GTK_OBJECT (button), "clicked",
			   GTK_SIGNAL_FUNC (skip_defaults_cb), xml);

       glade_xml_signal_autoconnect (xml);

       gtk_signal_connect (GTK_OBJECT (property_box), "apply",
			   GTK_SIGNAL_FUNC (preferences_apply_cb),
			   GTK_OBJECT (xml));
       gtk_signal_connect_object (GTK_OBJECT (property_box), "destroy",
				  GTK_SIGNAL_FUNC (gtk_object_destroy),
				  GTK_OBJECT (xml));
}

void
follow_fork_cb (GtkWidget *widget)
{
       ProcessWindow *pwin = pwin_from_widget (widget);
       pwin->follow_fork = GTK_CHECK_MENU_ITEM (widget)->active;
}

void
follow_exec_cb (GtkWidget *widget)
{
       ProcessWindow *pwin = pwin_from_widget (widget);
}

void
about_cb (GtkWidget *widget)
{
       GladeXML *xml;
       GtkWidget *dialog;

       ProcessWindow *pwin = pwin_from_widget (widget);
	
       xml = glade_xml_new (glade_file, "About");
       dialog = glade_xml_get_widget (xml, "About");
       gtk_object_destroy (GTK_OBJECT (xml));

       gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
       gnome_dialog_set_parent (GNOME_DIALOG (dialog),
				GTK_WINDOW (pwin->main_window));

       gtk_widget_show (dialog);
}

void 
show_error (ErrorType error,
	    const gchar *format,
	    ...)
{
	va_list args;
	char *message;
	GtkWidget *dialog;
	
	va_start (args, format);
	vasprintf (&message, format, args);
	va_end (args);

	dialog = gnome_message_box_new (message,
					(error == ERROR_WARNING) ?
					  GNOME_MESSAGE_BOX_WARNING :
					  GNOME_MESSAGE_BOX_ERROR,
					GNOME_STOCK_BUTTON_OK, NULL);
	free (message);

	gtk_window_set_title (GTK_WINDOW (dialog),
			      (error == ERROR_WARNING) ?
			      _("MemProf Error") : _("MemProf Warning"));

	if (error == ERROR_WARNING)
		gtk_widget_show (dialog);
	else {
#if 0
		if (pwin->main_window)
			gnome_dialog_set_parent (GNOME_DIALOG (dialog),
						 GTK_WINDOW (pwin->main_window));
#endif		
		gnome_dialog_run (GNOME_DIALOG (dialog));
	}

	if (error == ERROR_FATAL)
		exit(1);
}

static void
set_white_bg (GtkWidget *widget)
{
	GtkRcStyle *rc_style = gtk_rc_style_new();

	rc_style->color_flags[GTK_STATE_NORMAL] = GTK_RC_BG;
	rc_style->bg[GTK_STATE_NORMAL].red = 0xffff;
	rc_style->bg[GTK_STATE_NORMAL].green = 0xffff;
	rc_style->bg[GTK_STATE_NORMAL].blue = 0xffff;

	gtk_widget_modify_style (widget, rc_style);
}

static void
process_window_free (ProcessWindow *pwin)
{
	/* FIXME: we leak the process structure */
	
	if (pwin->leaks)
		g_slist_free (pwin->leaks);

	if (pwin->profile)
		profile_free (pwin->profile);

	g_free (pwin);
}


static void
process_window_destroy (ProcessWindow *pwin)
{
	if (pwin->status_update_timeout)
		g_source_remove (pwin->status_update_timeout);
	
	gtk_widget_destroy (pwin->main_window);
}

static ProcessWindow *
process_window_new (void)
{
       GladeXML *xml;
       GtkWidget *vpaned;
       ProcessWindow *pwin;
       GtkWidget *menuitem;

       pwin = g_new0 (ProcessWindow, 1);

       pwin->process = NULL;
       pwin->profile = NULL;
       pwin->leaks = NULL;

       pwin->usage_max = 32*1024;
       pwin->usage_high = 0;
       pwin->usage_leaked = 0;

       xml = glade_xml_new (glade_file, "MainWindow");

       pwin->main_window = glade_xml_get_widget (xml, "MainWindow");
       
       gtk_window_set_default_size (GTK_WINDOW (pwin->main_window), 400, 600);

       gtk_object_set_data_full (GTK_OBJECT (pwin->main_window),
				 "process-window",
				 pwin, (GDestroyNotify)process_window_free);

       pwin->main_notebook = glade_xml_get_widget (xml, "main-notebook");

       pwin->follow_fork = default_follow_fork;
       pwin->follow_exec = default_follow_exec;

       menuitem = glade_xml_get_widget (xml, "follow-fork");
       gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (menuitem), pwin->follow_fork);
       menuitem = glade_xml_get_widget (xml, "follow-exec");
       gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (menuitem), pwin->follow_exec);

       pwin->n_allocations_label = glade_xml_get_widget (xml, "n-allocations-label");
       pwin->bytes_per_label = glade_xml_get_widget (xml, "bytes-per-label");
       pwin->total_bytes_label = glade_xml_get_widget (xml, "total-bytes-label");

       pwin->profile_func_clist = glade_xml_get_widget (xml, "profile-func-clist");
       pwin->profile_child_clist =  glade_xml_get_widget (xml, "profile-child-clist");
       pwin->profile_caller_clist = glade_xml_get_widget (xml, "profile-caller-clist");

       gtk_clist_set_column_width (GTK_CLIST (pwin->profile_func_clist), 0, 250);
       gtk_clist_set_column_width (GTK_CLIST (pwin->profile_child_clist), 0, 250);
       gtk_clist_set_column_width (GTK_CLIST (pwin->profile_caller_clist), 0, 250);

       gtk_signal_connect (GTK_OBJECT (pwin->profile_func_clist), "select_row",
			   GTK_SIGNAL_FUNC (profile_func_select_row), pwin);
       gtk_signal_connect (GTK_OBJECT (pwin->profile_func_clist), "click_column",
			   GTK_SIGNAL_FUNC (profile_func_click_column), NULL);

       gtk_signal_connect (GTK_OBJECT (pwin->profile_caller_clist), "button_press_event",
			   GTK_SIGNAL_FUNC (profile_ref_button_press), pwin);
       gtk_signal_connect (GTK_OBJECT (pwin->profile_child_clist), "button_press_event",
			   GTK_SIGNAL_FUNC (profile_ref_button_press), pwin);
       
       pwin->leak_block_clist = glade_xml_get_widget (xml, "leak-block-clist");
       pwin->leak_stack_clist =  glade_xml_get_widget (xml, "leak-stack-clist");

       gtk_clist_set_column_width (GTK_CLIST (pwin->leak_stack_clist), 0, 250);
       gtk_clist_set_column_width (GTK_CLIST (pwin->leak_stack_clist), 2, 500);

       gtk_signal_connect (GTK_OBJECT (pwin->leak_block_clist), "select_row",
			   GTK_SIGNAL_FUNC (leak_block_select_row), pwin);
       gtk_signal_connect (GTK_OBJECT (pwin->leak_stack_clist), "button_press_event",
			   GTK_SIGNAL_FUNC (leak_stack_button_press), pwin);

       pwin->usage_max_label = glade_xml_get_widget (xml, "usage-max-label");
       pwin->usage_canvas = glade_xml_get_widget (xml, "usage-canvas");

       set_white_bg (pwin->usage_canvas);
       gtk_signal_connect (GTK_OBJECT (pwin->usage_canvas), "size_allocate",
			   GTK_SIGNAL_FUNC (usage_canvas_size_allocate), pwin);
              
       vpaned = glade_xml_get_widget (xml, "profile-vpaned");
       gtk_paned_set_position (GTK_PANED (vpaned), 150);
       
       vpaned = glade_xml_get_widget (xml, "leaks-vpaned");
       gtk_paned_set_position (GTK_PANED (vpaned), 150);

       glade_xml_signal_autoconnect (xml);
       gtk_object_destroy (GTK_OBJECT (xml));

       return pwin;
}

int
main(int argc, char **argv)
{
       static const struct poptOption memprof_popt_options [] = {
	       { "follow-fork", '\0', POPT_ARG_NONE, &default_follow_fork, 0,
		 N_("Create new windows for forked processes"), NULL },
	       { "follow-exec", '\0', POPT_ARG_NONE, &default_follow_exec, 0,
		 N_("Retain windows for processes after exec()"), NULL },
	       { NULL, '\0', 0, NULL, 0 },
       };
       poptContext ctx;

       int init_results;
       const char **startup_args;
       ProcessWindow *initial_window;

       bindtextdomain (PACKAGE, GNOMELOCALEDIR);
       textdomain (PACKAGE);

       init_results = gnome_init_with_popt_table (PACKAGE, VERSION,
						  argc, argv,
						  memprof_popt_options, 0, &ctx);
       glade_gnome_init ();

       glade_file = "./memprof.glade";
       if (!g_file_exists (glade_file)) {
	       glade_file = g_concat_dir_and_file (DATADIR, "memprof.glade");
       }
       if (!g_file_exists (glade_file)) {
	       show_error (ERROR_FATAL, _("Cannot find memprof.glade"));
       }

       process_init(create_child_process);

       initial_window = process_window_new();

       gnome_config_get_vector ("/MemProf/Options/skip_funcs=" DEFAULT_SKIP,
				&n_skip_funcs, &skip_funcs);
       stack_command = gnome_config_get_string ("/MemProf/Options/stack_command=" DEFAULT_STACK_COMMAND);

       gtk_widget_show (initial_window->main_window);

       startup_args = poptGetArgs (ctx);
       if (startup_args)
	       run_file (initial_window, (char **)startup_args);
       poptFreeContext (ctx);
       
       gtk_main ();

       return 0;
}
