/* -*- mode: C; c-file-style: "linux" -*- */

#define _GNU_SOURCE

#include <stdarg.h>
#include <stdio.h>
#include <glade/glade.h>
#include <gnome.h>

#include "leakdetect.h"
#include "memprof.h"
#include "process.h"
#include "profile.h"

#include "config.h"

MPProcess *current_process;
Profile *current_profile;
GSList *current_leaks;

char *glade_file;

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

guint usage_max = 32*1024;
guint usage_high = 0;
guint usage_leaked = 0;

GnomeCanvasItem *usage_frame;
GnomeCanvasItem *usage_current_bar;
GnomeCanvasItem *usage_high_bar;
GnomeCanvasItem *usage_leak_bar;

guint status_update_timeout;

#define DEFAULT_SKIP "g_malloc g_malloc0 g_realloc g_strdup strdup strndup"
#define DEFAULT_STACK_COMMAND "emacsclient -n +%l \"%f\""

static int n_skip_funcs;
static char **skip_funcs;

char *stack_command;


/************************************************************
 * Status Page 
 ************************************************************/

static gboolean
update_status (gpointer data)
{
	char *tmp;

	tmp = g_strdup_printf ("%d", current_process->bytes_used);
	gtk_label_set_text (GTK_LABEL (total_bytes_label), tmp);
	g_free (tmp);

	tmp = g_strdup_printf ("%d", current_process->n_allocations);
	gtk_label_set_text (GTK_LABEL (n_allocations_label), tmp);
	g_free (tmp);

	tmp = g_strdup_printf ("%.2f",
			       (double)current_process->bytes_used /
			       	       current_process->n_allocations);
	gtk_label_set_text (GTK_LABEL (bytes_per_label), tmp);
	g_free (tmp);

	if (current_process->bytes_used > usage_max) {
		while ((current_process->bytes_used > usage_max))
			usage_max *= 2;
	
		tmp = g_strdup_printf ("%dk", usage_max / 1024);
		gtk_label_set_text (GTK_LABEL (usage_max_label), tmp);
		g_free (tmp);
	}

	usage_high = MAX (current_process->bytes_used, usage_high);
	
	gnome_canvas_item_set (usage_current_bar,
			       "x2", ((double)usage_canvas->allocation.width *
				      (double)current_process->bytes_used / usage_max),
			       NULL);
	gnome_canvas_item_set (usage_high_bar,
			       "x2", ((double)usage_canvas->allocation.width *
				      (double)usage_high / usage_max),
			       NULL);
	gnome_canvas_item_set (usage_leak_bar,
			       "x2", ((double)usage_canvas->allocation.width *
				      (double)usage_leaked / usage_max),
			       NULL);
	
	return TRUE;
}

static void
usage_canvas_size_allocate (GtkWidget *widget, GtkAllocation *allocation)
{
	if (!usage_frame) {
		usage_high_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
							gnome_canvas_rect_get_type (),
							"x1", 0.0,
							"y1", 0.0,
							"outline_color", "black", 
							"fill_color", "blue",
							NULL);
		usage_current_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
							   gnome_canvas_rect_get_type (),
							   "x1", 0.0,
							   "y1", 0.0,
							   "outline_color", "black", 
							   "fill_color", "yellow",
							   NULL);
		usage_leak_bar = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
							 gnome_canvas_rect_get_type (),
							 "x1", 0.0,
							 "y1", 0.0,
							 "outline_color", "black", 
							 "fill_color", "red",
							 NULL);
		usage_frame = gnome_canvas_item_new (gnome_canvas_root (GNOME_CANVAS (widget)),
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
		
	gnome_canvas_item_set (usage_frame,
			       "x2", (double)allocation->width - 1,
			       "y2", (double)allocation->height - 1,
			       NULL);
	gnome_canvas_item_set (usage_current_bar,
			       "y2", (double)allocation->height - 1,
			       NULL);
	gnome_canvas_item_set (usage_high_bar,
			       "y2", (double)allocation->height - 1,
			       NULL);
	gnome_canvas_item_set (usage_leak_bar,
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
profile_func_select_row (GtkWidget *widget, gint row, gint column, GdkEvent *event)
{
	ProfileFunc *function = gtk_clist_get_row_data (GTK_CLIST (widget), row);

	if (function == NULL)
		return;

	profile_fill_ref_clist (profile_child_clist, function->children);
	profile_fill_ref_clist (profile_caller_clist, function->inherited);
}

static gboolean
profile_ref_button_press (GtkWidget *widget, GdkEventButton *event, gpointer data)
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

			function_row = gtk_clist_find_row_from_data (GTK_CLIST (profile_func_clist), ref->function);

			if (function_row != -1) {
				gtk_clist_select_row (GTK_CLIST (profile_func_clist), function_row, 0);
				gtk_clist_moveto (GTK_CLIST (profile_func_clist),
						  function_row, -1, 0.5, 0.0);
			}
		}

		gtk_signal_emit_stop_by_name (GTK_OBJECT (widget), "button_press_event");
		return TRUE;
	}
	
	return FALSE;
}

static void
profile_fill (void)
{
	int i, row;

	gtk_clist_clear (GTK_CLIST (profile_func_clist));
	for (i=0; i<current_profile->n_functions; i++) {
		ProfileFunc *function = current_profile->functions[i];
		char *data[3];
		char buf[32];
		
		data[0] = function->symbol->name;
		g_snprintf(buf, 32, "%u", function->self);
		data[1] = g_strdup (buf);

		g_snprintf(buf, 32, "%u", function->total);
		data[2] = g_strdup (buf);

		row = gtk_clist_append (GTK_CLIST (profile_func_clist), data);

		g_free (data[1]);
		g_free (data[2]);

		gtk_clist_set_row_data (GTK_CLIST (profile_func_clist), row, function);
	}

	if (GTK_CLIST (profile_func_clist)->rows > 0)
		profile_func_select_row (profile_func_clist, 0, 0, NULL);
}


/************************************************************
 * GUI for leak detection
 ************************************************************/

static void
leak_block_select_row (GtkWidget *widget, gint row, gint column, GdkEvent *event)
{
	Block *block = gtk_clist_get_row_data (GTK_CLIST (widget), row);
	int i;

	if (block == NULL)
		return;

	gtk_clist_clear (GTK_CLIST (leak_stack_clist));
	for (i = 0; i < block->stack_size; i++) {
		char *data[3];
		char buf[32];
		const char *filename;
		char *functionname;
		unsigned int line;
		
		if (!process_find_line (current_process, block->stack[i],
					&filename, &functionname, &line)) {
			functionname = g_strdup ("(???)");
			filename = "(???)";
			line = 0;
		}
		
		data[0] = functionname;
		
		g_snprintf(buf, 32, "%d", line);
		data[1] = buf;
		
		data[2] = (char *)filename;

		gtk_clist_append (GTK_CLIST (leak_stack_clist), data);
		free (data[0]);
	}
		
}

static void
leaks_fill (void)
{
	gint row;
	GSList *tmp_list;

	gtk_clist_clear (GTK_CLIST (leak_block_clist));
	tmp_list = current_leaks;
	while (tmp_list) {
		char *data[3];
		char buf[32];
		const char *filename;
		char *functionname = NULL;
		unsigned int line;
		int frame;
		
		Block *block = tmp_list->data;

		for (frame = 0 ; frame < block->stack_size ; frame++) {
			if (process_find_line (current_process, block->stack[frame],
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
		
		row = gtk_clist_append (GTK_CLIST (leak_block_clist), data);
		gtk_clist_set_row_data (GTK_CLIST (leak_block_clist), row, block);

		g_free (data[0]);
		g_free (data[1]);
		free (data[2]);
		
		tmp_list = tmp_list->next;
	}

	if (GTK_CLIST (leak_block_clist)->rows > 0)
		leak_block_select_row (leak_block_clist, 0, 0, NULL);
}

static void
leak_stack_run_command (Block *block, int frame)
{
	const char *filename;
	char *functionname;
	unsigned int line;

	if (process_find_line (current_process, block->stack[frame],
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
leak_stack_button_press (GtkWidget *widget, GdkEventButton *event, gpointer data)
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

			g_return_val_if_fail (GTK_CLIST (leak_block_clist)->selection, FALSE);

			block_row = GPOINTER_TO_INT (GTK_CLIST (leak_block_clist)->selection->data);
			block = gtk_clist_get_row_data (GTK_CLIST (leak_block_clist), block_row);

			leak_stack_run_command (block, my_row);
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

void
exit_cb ()
{
	gtk_main_quit ();
}

void
run_cb ()
{
       GladeXML *xml;
       GtkWidget *run_dialog;
       GtkWidget *entry;
       char *text;

       xml = glade_xml_new (glade_file, "RunDialog");
       run_dialog = glade_xml_get_widget (xml, "RunDialog");
       entry = glade_xml_get_widget (xml, "RunDialog-entry");

       gtk_signal_connect_object (GTK_OBJECT (entry), "activate",
				  GTK_SIGNAL_FUNC (gtk_widget_activate),
				  GTK_OBJECT (glade_xml_get_widget (xml, "RunDialog-run")));

       gtk_object_destroy (GTK_OBJECT (xml));

       while (1) {
	       gnome_dialog_set_parent (GNOME_DIALOG (run_dialog),
					GTK_WINDOW (main_window));
	       if (gnome_dialog_run (GNOME_DIALOG (run_dialog)) == 0) {
		       text = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
		       
		       if (g_file_exists (text)) {
			       current_process = process_run (text);

			       if (!status_update_timeout)
				       status_update_timeout =
					       g_timeout_add (100,
							      update_status,
							      NULL);
			       
			       g_free (text);
			       break;
	 	       } else {
			       show_error (ERROR_MODAL,
					   _("Executable \"%s\" does not exist"),
					   text);
			       g_free (text);
		       }
			       
	       } else {
		       break;
	       }
       }

       gtk_widget_destroy (run_dialog);
}

void
save_leak_cb ()
{
	static gchar *suggestion = NULL;
	gchar *filename;

	if (current_leaks) {
		filename = get_filename ("Save Leak Report", "Output file",
					 suggestion ? suggestion : "memprof.leak");
		if (filename) {
			g_free (suggestion);
			suggestion = filename;
			
			leaks_print (current_process, current_leaks, filename);
		}
	}
}

void
save_profile_cb ()
{
	static gchar *suggestion = NULL;
	gchar *filename;

	if (current_profile) {
		filename = get_filename ("Save Profile", "Output file",
					 suggestion ? suggestion : "memprof.out");
		if (filename) {
			g_free (suggestion);
			suggestion = filename;
			
			profile_write (current_profile, filename);
		}
	}
}

void
save_current_cb ()
{
	switch (gtk_notebook_get_current_page (GTK_NOTEBOOK (main_notebook))) {
	case 0:
		save_profile_cb ();
		break;
	case 1:
		save_leak_cb ();
		break;
	}
}

void
generate_leak_cb ()
{
	GSList *tmp_list;
	
	if (current_process) {
		process_stop_input (current_process);

		if (current_leaks)
			g_slist_free (current_leaks);
		current_leaks = leaks_find (current_process);

		usage_leaked = 0;
		tmp_list = current_leaks;
		while (tmp_list) {
			usage_leaked += ((Block *)tmp_list->data)->size;
			tmp_list = tmp_list->next;
		}
		
		leaks_fill ();
		gtk_notebook_set_page (GTK_NOTEBOOK (main_notebook), 1);

		process_start_input (current_process);
	}
}

void
generate_profile_cb ()
{
	if (current_process) {
		process_stop_input (current_process);

		if (current_profile) {
			profile_free (current_profile);
			current_profile = NULL;
		}

		current_profile = profile_create (current_process, skip_funcs, n_skip_funcs);
		process_start_input (current_process);
		profile_fill ();

		gtk_notebook_set_page (GTK_NOTEBOOK (main_notebook), 0);
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
preferences_cb ()
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
about_cb ()
{
       GladeXML *xml;
       GtkWidget *dialog;

       xml = glade_xml_new (glade_file, "About");
       dialog = glade_xml_get_widget (xml, "About");
       gtk_object_destroy (GTK_OBJECT (xml));

       gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
       gnome_dialog_set_parent (GNOME_DIALOG (dialog),
				GTK_WINDOW (main_window));

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
		if (main_window)
			gnome_dialog_set_parent (GNOME_DIALOG (dialog),
						 GTK_WINDOW (main_window));
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

int
main(int argc, char **argv)
{
       int init_results;
       GladeXML *xml;
       GtkWidget *vpaned;       
       
       bindtextdomain (PACKAGE, GNOMELOCALEDIR);
       textdomain (PACKAGE);

       init_results = gnome_init(PACKAGE, VERSION, argc, argv);
       glade_gnome_init ();

       glade_file = "./memprof.glade";
       if (!g_file_exists (glade_file)) {
	       glade_file = g_concat_dir_and_file (DATADIR, "memprof.glade");
       }
       if (!g_file_exists (glade_file)) {
	       show_error (ERROR_FATAL, _("Cannot find memprof.glade"));
       }
       
       xml = glade_xml_new (glade_file, "MainWindow");

       main_window = glade_xml_get_widget (xml, "MainWindow");
       gtk_signal_connect (GTK_OBJECT (main_window), "destroy",
			   GTK_SIGNAL_FUNC (gtk_main_quit), NULL);
       gtk_window_set_default_size (GTK_WINDOW (main_window), 400, 600);

       main_notebook = glade_xml_get_widget (xml, "main-notebook");

       n_allocations_label = glade_xml_get_widget (xml, "n-allocations-label");
       bytes_per_label = glade_xml_get_widget (xml, "bytes-per-label");
       total_bytes_label = glade_xml_get_widget (xml, "total-bytes-label");

       profile_func_clist = glade_xml_get_widget (xml, "profile-func-clist");
       profile_child_clist =  glade_xml_get_widget (xml, "profile-child-clist");
       profile_caller_clist = glade_xml_get_widget (xml, "profile-caller-clist");

       gtk_clist_set_column_width (GTK_CLIST (profile_func_clist), 0, 250);
       gtk_clist_set_column_width (GTK_CLIST (profile_child_clist), 0, 250);
       gtk_clist_set_column_width (GTK_CLIST (profile_caller_clist), 0, 250);

       gtk_signal_connect (GTK_OBJECT (profile_func_clist), "select_row",
			   GTK_SIGNAL_FUNC (profile_func_select_row), NULL);
       gtk_signal_connect (GTK_OBJECT (profile_func_clist), "click_column",
			   GTK_SIGNAL_FUNC (profile_func_click_column), NULL);

       gtk_signal_connect (GTK_OBJECT (profile_caller_clist), "button_press_event",
			   GTK_SIGNAL_FUNC (profile_ref_button_press), NULL);
       gtk_signal_connect (GTK_OBJECT (profile_child_clist), "button_press_event",
			   GTK_SIGNAL_FUNC (profile_ref_button_press), NULL);
       
       leak_block_clist = glade_xml_get_widget (xml, "leak-block-clist");
       leak_stack_clist =  glade_xml_get_widget (xml, "leak-stack-clist");

       gtk_clist_set_column_width (GTK_CLIST (leak_stack_clist), 0, 250);
       gtk_clist_set_column_width (GTK_CLIST (leak_stack_clist), 2, 500);

       gtk_signal_connect (GTK_OBJECT (leak_block_clist), "select_row",
			   GTK_SIGNAL_FUNC (leak_block_select_row), NULL);
       gtk_signal_connect (GTK_OBJECT (leak_stack_clist), "button_press_event",
			   GTK_SIGNAL_FUNC (leak_stack_button_press), NULL);

       usage_max_label = glade_xml_get_widget (xml, "usage-max-label");
       usage_canvas = glade_xml_get_widget (xml, "usage-canvas");

       set_white_bg (usage_canvas);
       gtk_signal_connect (GTK_OBJECT (usage_canvas), "size_allocate",
			   GTK_SIGNAL_FUNC (usage_canvas_size_allocate), NULL);
              
       vpaned = glade_xml_get_widget (xml, "profile-vpaned");
       gtk_paned_set_position (GTK_PANED (vpaned), 150);
       
       vpaned = glade_xml_get_widget (xml, "leaks-vpaned");
       gtk_paned_set_position (GTK_PANED (vpaned), 150);

       glade_xml_signal_autoconnect (xml);

       process_init();

       gnome_config_get_vector ("/MemProf/Options/skip_funcs=" DEFAULT_SKIP,
				&n_skip_funcs, &skip_funcs);
       stack_command = gnome_config_get_string ("/MemProf/Options/stack_command=" DEFAULT_STACK_COMMAND);
       
       gtk_widget_show (main_window);
       gtk_main ();

       gtk_object_destroy (GTK_OBJECT (xml));
       
       return 0;
}
