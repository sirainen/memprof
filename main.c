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

#define _GNU_SOURCE

#include "config.h"

#include <stdarg.h>
#include <stdio.h>

#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

#include <glade/glade.h>
#include <gnome.h>

#include <gconf/gconf-client.h>

#include <regex.h>

#include "leakdetect.h"
#include "gui.h"
#include "memprof.h"
#include "process.h"
#include "profile.h"
#include "server.h"

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

		g_signal_stop_emission_by_name (G_OBJECT (widget), "button_press_event");
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
		int frame;
		
		Block *block = tmp_list->data;

		for (frame = 0 ; frame < block->stack_size ; frame++) {
			if (process_find_line (pwin->process, block->stack[frame],
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
	GtkWidget *dialog;
	gint response;

	ProcessWindow *pwin = pwin_from_widget (widget);
       
	dialog = gtk_message_dialog_new (GTK_WINDOW (pwin->main_window),
					 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					 GTK_MESSAGE_QUESTION,
					 GTK_BUTTONS_YES_NO,
					 _("Really quit MemProf?"));
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_YES);

	response = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	
	if (response == GTK_RESPONSE_YES)
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
		gtk_clist_clear (GTK_CLIST (pwin->profile_func_clist));
		gtk_clist_clear (GTK_CLIST (pwin->profile_caller_clist));
		gtk_clist_clear (GTK_CLIST (pwin->profile_child_clist));
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
       run_dialog = glade_xml_get_widget (xml, "RunDialog");
       entry = glade_xml_get_widget (xml, "RunDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (glade_xml_get_widget (xml, "RunDialog-run")));

       g_object_unref (G_OBJECT (xml));

       while (1) {
	       gnome_dialog_set_parent (GNOME_DIALOG (run_dialog),
					GTK_WINDOW (pwin->main_window));
	       if (gnome_dialog_run (GNOME_DIALOG (run_dialog)) == 1) {
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

		pwin->profile = profile_create (pwin->process, skip_funcs);
		process_start_input (pwin->process);
		profile_fill (pwin, GTK_CLIST (pwin->profile_func_clist));

		gtk_notebook_set_page (GTK_NOTEBOOK (pwin->main_notebook), 0);
	}
}

static void
skip_fill (GtkWidget *clist)
{
	GSList *tmp_list;

	gtk_clist_clear (GTK_CLIST (clist));

	for (tmp_list = skip_funcs; tmp_list != NULL; tmp_list = tmp_list->next)
		gtk_clist_append (GTK_CLIST (clist), (gchar **)&tmp_list->data);
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

       xml = glade_xml_new (glade_file, "SkipAddDialog", NULL);
       dialog = glade_xml_get_widget (xml, "SkipAddDialog");
       entry = glade_xml_get_widget (xml, "SkipAddDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (glade_xml_get_widget (xml, "SkipAddDialog-add")));

       g_object_unref (G_OBJECT (xml));

       while (1) {
	       gnome_dialog_set_parent (GNOME_DIALOG (dialog),
					GTK_WINDOW (property_box));
	       if (gnome_dialog_run (GNOME_DIALOG (dialog)) == 1) {
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

			       skip_funcs = g_slist_append (skip_funcs,
							    g_strdup (text));
			       gconf_client_set_list (gconf_client_get_default (), "/apps/memprof/skip_funcs", GCONF_VALUE_STRING, skip_funcs, NULL);

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
skip_delete_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GtkWidget *clist = glade_xml_get_widget (preferences_xml, "skip-clist");

	if (GTK_CLIST (clist)->selection) {
		GSList *tmp_list;
		gint selected_row = GPOINTER_TO_INT (GTK_CLIST (clist)->selection->data);
		gtk_clist_remove (GTK_CLIST (clist), selected_row);

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
skip_regexes_fill (GtkWidget *clist)
{
	GSList *tmp_list;

	gtk_clist_clear (GTK_CLIST (clist));

	for (tmp_list = skip_regexes; tmp_list != NULL; tmp_list = tmp_list->next)
		gtk_clist_append (GTK_CLIST (clist), (gchar **)&tmp_list->data);
}

static void
skip_regexes_add_cb (GtkWidget *widget, GladeXML *preferences_xml)
{
       GladeXML *xml;
       GtkWidget *dialog;
       GtkWidget *entry;
       char *text;

       GtkWidget *skip_regexes_clist = glade_xml_get_widget (preferences_xml, "skip-regexes-clist");
       GtkWidget *property_box = glade_xml_get_widget (preferences_xml, "Preferences");

       xml = glade_xml_new (glade_file, "SkipRegexesAddDialog", NULL);
       dialog = glade_xml_get_widget (xml, "SkipRegexesAddDialog");
       entry = glade_xml_get_widget (xml, "SkipRegexesAddDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (glade_xml_get_widget (xml, "SkipRegexesAddDialog-add")));

       g_object_unref (G_OBJECT (xml));

       while (1) {
	       gnome_dialog_set_parent (GNOME_DIALOG (dialog),
					GTK_WINDOW (property_box));
	       if (gnome_dialog_run (GNOME_DIALOG (dialog)) == 1) {
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
			       gtk_clist_append (GTK_CLIST (skip_regexes_clist), &text);

			       skip_regexes = g_slist_append (skip_regexes,
							      g_strdup (text));
			       gconf_client_set_list (gconf_client_get_default (), "/apps/memprof/skip_regexes", GCONF_VALUE_STRING, skip_regexes, NULL);

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
       GtkWidget *clist = glade_xml_get_widget (preferences_xml, "skip-regexes-clist");

	if (GTK_CLIST (clist)->selection) {
		GSList *tmp_list;
		gint selected_row = GPOINTER_TO_INT (GTK_CLIST (clist)->selection->data);
		gtk_clist_remove (GTK_CLIST (clist), selected_row);

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
       GtkWidget *skip_clist, *skip_regexes_clist;
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
       skip_clist = glade_xml_get_widget (xml, "skip-clist");
       skip_regexes_clist = glade_xml_get_widget (xml, "skip-regexes-clist");
       stack_command_entry = glade_xml_get_widget (xml, "stack-command-entry");

       property_box = gtk_dialog_new_with_buttons ("Preferences",
						   NULL,
						   GTK_DIALOG_DESTROY_WITH_PARENT,
						   GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE,
						   NULL);
       gtk_widget_reparent (glade_xml_get_widget (xml, "notebook2"),
                            GTK_DIALOG (property_box)->vbox);

       skip_fill (skip_clist);
       skip_regexes_fill (skip_regexes_clist);

       gtk_entry_set_text (GTK_ENTRY (stack_command_entry), stack_command);
       g_signal_connect (G_OBJECT (stack_command_entry), "activate",
			 G_CALLBACK (stack_command_entry_update), NULL);
       g_signal_connect (G_OBJECT (stack_command_entry), "focus_out_event",
			 G_CALLBACK (stack_command_entry_focus_out), NULL);

       button = glade_xml_get_widget (xml, "skip-add-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_add_cb), xml);
       button = glade_xml_get_widget (xml, "skip-delete-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_delete_cb), xml);
       button = glade_xml_get_widget (xml, "skip-defaults-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_defaults_cb), xml);

       button = glade_xml_get_widget (xml, "skip-regexes-add-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_regexes_add_cb), xml);
       button = glade_xml_get_widget (xml, "skip-regexes-delete-button");
       g_signal_connect (G_OBJECT (button), "clicked",
			 G_CALLBACK (skip_regexes_delete_cb), xml);
       button = glade_xml_get_widget (xml, "skip-regexes-defaults-button");
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
	if (!strcmp(gconf_entry_get_key (entry), "/apps/memprof/skip_funcs")) {
		if (skip_funcs)
			g_slist_free (skip_funcs);
		skip_funcs = gconf_client_get_list (client,
						    "/apps/memprof/skip_funcs",
						    GCONF_VALUE_STRING, NULL);

		if (pref_dialog_xml) {
			GtkWidget *clist = glade_xml_get_widget (pref_dialog_xml, "skip-clist");
			skip_fill (clist);
		}
	} else if (!strcmp (gconf_entry_get_key (entry), "/apps/memprof/skip_regexes")) {
		if (skip_regexes)
			g_slist_free (skip_regexes);
		skip_regexes = gconf_client_get_list (client, "/apps/memprof/skip_regexes",
						      GCONF_VALUE_STRING, NULL);

		if (pref_dialog_xml) {
			GtkWidget *clist = glade_xml_get_widget (pref_dialog_xml, "skip-regexes-clist");
			skip_regexes_fill (clist);
		}
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

		e = glade_xml_get_widget (pref_dialog_xml, "stack-command-entry");
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
       dialog = glade_xml_get_widget (xml, "About");
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

static ProcessWindow *
process_window_new (void)
{
       gchar *fullfilename;
       GladeXML *xml;
       GtkWidget *vpaned;
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

       pwin->main_window = glade_xml_get_widget (xml, "MainWindow");
       fullfilename = gnome_program_locate_file (NULL, GNOME_FILE_DOMAIN_PIXMAP, "memprof.png", FALSE, NULL);
       gnome_window_icon_set_from_file (GTK_WINDOW (pwin->main_window),
					fullfilename);
       g_free (fullfilename);

       gtk_signal_connect (GTK_OBJECT (pwin->main_window), "delete_event",
			   GTK_SIGNAL_FUNC (hide_and_check_quit), pwin);
			   
       
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
              
       vpaned = glade_xml_get_widget (xml, "profile-vpaned");
       gtk_paned_set_position (GTK_PANED (vpaned), 150);
       
       vpaned = glade_xml_get_widget (xml, "leaks-vpaned");
       gtk_paned_set_position (GTK_PANED (vpaned), 150);

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
       static const struct poptOption memprof_popt_options [] = {
	       { "follow-fork", '\0', POPT_ARG_NONE, &default_follow_fork, 0,
		 N_("Create new windows for forked processes"), NULL },
	       { "follow-exec", '\0', POPT_ARG_NONE, &default_follow_exec, 0,
		 N_("Retain windows for processes after exec()"), NULL },
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
