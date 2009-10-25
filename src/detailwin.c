/* -*- mode: C; c-file-style: "linux" -*- */

/* MemProf -- memory profiler and leak detector
 * Copyright 2006 Carsten Haitzler
 * Copyright 2009 Holger Hans Peter Freyther
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
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <regex.h>
#include <signal.h>
#include <sys/wait.h>


#include <glade/glade.h>
#include <gtk/gtk.h>

#include "gui.h"
#include "memprof.h"

#define MEMSTATS 4096

typedef struct _DW
{
   ProcessWindow *pwin;
   GtkWidget *win, *da1, *da2;
   guint memstats[MEMSTATS][2];
} DW;

static void
dw_draw_memstats(DW *dw)
{
   GtkWidget *widget;
   GdkPixmap *pixmap;
   gint64 i, j, x, y, w, h, hh;

   widget = dw->da1;
   w = widget->allocation.width;
   h = widget->allocation.height;
   if (!widget->window) return;
   pixmap = gdk_pixmap_new(widget->window, w, h, -1);
   gdk_draw_rectangle(pixmap,
		      widget->style->base_gc[GTK_STATE_NORMAL],
		      TRUE,
		      0, 0, w, h);
   for (i = 0; i < MEMSTATS; i++)
     {
	x = w - i;
	if (x < 0) break;
	if (dw->pwin->usage_high > 0)
	  hh = (h * dw->memstats[i][1]) / dw->pwin->usage_high;
	else
	  hh = 0;
	y = h - hh;
	gdk_draw_rectangle(pixmap,
			   widget->style->base_gc[GTK_STATE_SELECTED],
			   TRUE,
			   x, y, 1, hh);
	if (dw->pwin->usage_high > 0)
	  hh = (h * dw->memstats[i][0]) / dw->pwin->usage_high;
	else
	  hh = 0;
	y = h - hh;
	gdk_draw_rectangle(pixmap,
			   widget->style->text_gc[GTK_STATE_NORMAL],
			   TRUE,
			   x, y, 1, hh);
     }
   if (dw->pwin->usage_high > 0)
     {
	GdkGC *gc;

	gc = gdk_gc_new(pixmap);
	gdk_gc_copy(gc, widget->style->dark_gc[GTK_STATE_NORMAL]);
	gdk_gc_set_line_attributes(gc, 0, GDK_LINE_ON_OFF_DASH,
				   GDK_CAP_BUTT, GDK_JOIN_MITER);
	for (j = 0, i = 0; i < dw->pwin->usage_high; i += (256 * 1024), j++)
	  {
	     if (j > 3) j = 0;
	     y = h - ((i * h) / dw->pwin->usage_high);
	     if (j == 0)
	       gdk_draw_line(pixmap, widget->style->dark_gc[GTK_STATE_NORMAL],
			     0, y, w, y);
	     else
	       gdk_draw_line(pixmap, gc,
			     0, y, w, y);
	  }
	gdk_gc_unref(gc);
     }
   gdk_draw_pixmap(widget->window,
		   widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		   pixmap, 0, 0, 0, 0, w, h);
   gdk_pixmap_unref(pixmap);
}

typedef struct _Mem
{
   GtkWidget *widget;
   GdkPixmap *pixmap;
   guint w, h, bpp;
   gulong mtotal;
   GList *regs;
} Mem;

typedef struct _MemReg
{
   void *start;
   unsigned long size;
   int y, h;
} MemReg;

static void
dw_draw_memmap_foreach(gpointer key, gpointer value, gpointer data)
{
   Mem *mem;
   Block *block;
   guint x, y, w, i;
   GdkPixmap *pixmap;
   GdkGC *gc;
   gulong addr;
   guint bpp, memw;
   GList *l;
   MemReg *reg;


   mem = data;
   block = value;
   bpp = mem->bpp;
   addr = (gulong)block->addr;
   for (i = 0, l = mem->regs; l; l = l->next, i++)
     {
	reg = l->data;
	if ((addr >= (gulong)reg->start) &&
	    (addr < (gulong)(reg->start + reg->size)))
	  break;
	reg = NULL;
     }
   if (!reg) return;
   addr = addr - (gulong)reg->start;
   w = (gulong)(addr + block->size + (bpp / 2)) / bpp;
   x = (gulong)(addr) / bpp;
   w -= x;
   memw = mem->w;
   y = reg->y + (x / memw);
   x -= (y * memw);
   pixmap = mem->pixmap;
   if (i & 0x1)
     gc = mem->widget->style->fg_gc[GTK_STATE_PRELIGHT];
   else
     gc = mem->widget->style->fg_gc[GTK_STATE_NORMAL];
   gdk_draw_rectangle(pixmap, gc, TRUE, x, y, w, 1);
   if ((x + w) > mem->w)
     gdk_draw_rectangle(pixmap, gc, TRUE, 0, y + 1, w - (memw - x), 1);
}

static void
dw_draw_memmap(DW *dw)
{
   GtkWidget *widget;
   GdkPixmap *pixmap;
   guint w, h, y, bpl, i;
   Mem mem;
   GList *l;

   widget = dw->da2;
   w = widget->allocation.width;
   h = widget->allocation.height;
   if (!widget->window) return;
   pixmap = gdk_pixmap_new(widget->window, w, h, -1);
   gdk_draw_rectangle(pixmap,
		      widget->style->base_gc[GTK_STATE_NORMAL],
		      TRUE,
		      0, 0, w, h);
   mem.regs = NULL;
   mem.widget = widget;
   mem.pixmap = pixmap;
   mem.w = w;
   mem.h = h;
   mem.mtotal = 0;
     {
        gchar buffer[1024];
	FILE *in;
	gchar perms[26];
	gchar file[256];
	gulong start, end;
	guint major, minor, inode;

	snprintf(buffer, 1023, "/proc/%d/maps", dw->pwin->process->pid);

	in = fopen(buffer, "r");
        while (fgets(buffer, 1023, in))
	  {
	     file[0] = 0;
	     int count = sscanf(buffer, "%lx-%lx %15s %*x %u:%u %u %255s",
				&start, &end, perms, &major, &minor, &inode,
				file);
	     if (count >= 5)
	       {
		  if ((!strcmp(perms, "rw-p")) && (inode == 0))
		    {
		       MemReg *reg;

		       reg = g_malloc(sizeof(MemReg));
		       reg->start = GSIZE_TO_POINTER(start);
		       reg->size = end - start;
		       mem.mtotal += reg->size;
		       mem.regs = g_list_append(mem.regs, reg);
		    }
	       }
	  }
        fclose (in);
     }
   bpl = (mem.mtotal + h - 1) / h;
   y = 0;
   for (i = 0, l = mem.regs; l; l = l->next, i++)
     {
	MemReg *reg;

	reg = l->data;
	reg->h = ((reg->size * h) + bpl - 1) / mem.mtotal;
	reg->y = y;
	y += reg->h;
	if (i & 0x1)
	  gdk_draw_rectangle(pixmap,
			     widget->style->base_gc[GTK_STATE_INSENSITIVE],
			     TRUE,
			     0, reg->y, w, reg->h);
     }
   mem.bpp = (bpl + w - 1) / w;
   if (mem.bpp < 1) mem.bpp = 1;

   g_hash_table_foreach(dw->pwin->process->block_table,
			dw_draw_memmap_foreach,
			&mem);

   for (l = mem.regs; l; l = l->next) g_free(l->data);
   g_list_free(mem.regs);

   gdk_draw_pixmap(widget->window,
		   widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
		   pixmap, 0, 0, 0, 0, w, h);
   gdk_pixmap_unref(pixmap);
}

static gboolean
on_da1_expose_event(GtkWidget *widget, GdkEventExpose  *event, gpointer user_data)
{
   DW *dw;

   dw = user_data;
   if (!dw->pwin->process) return FALSE;
   dw_draw_memstats(dw);
   return FALSE;
}

static gboolean
on_da2_expose_event(GtkWidget *widget, GdkEventExpose  *event, gpointer user_data)
{
   DW *dw;

   dw = user_data;
   if (!dw->pwin->process) return FALSE;
   dw_draw_memmap(dw);
   return FALSE;
}

void
dw_update(ProcessWindow *pwin)
{
   DW *dw;
   guint i;

   dw = pwin->detailwin_data;
   if (!dw) return;
   if (!pwin->process) return;
   for (i = MEMSTATS - 1; i > 0; i--)
     {
	dw->memstats[i][0] = dw->memstats[i - 1][0];
	dw->memstats[i][1] = dw->memstats[i - 1][1];
     }
   dw->memstats[0][0] = pwin->process->bytes_used;
   dw->memstats[0][1] = pwin->usage_high;
   dw_draw_memstats(dw);
   dw_draw_memmap(dw);
}

void
dw_init(ProcessWindow *pwin)
{
   DW *dw;

   GtkWidget *win;
   GtkWidget *notebook1;
   GtkWidget *frame1;
   GtkWidget *alignment1;
   GtkWidget *da1;
   GtkWidget *label1;
   GtkWidget *frame2;
   GtkWidget *alignment2;
   GtkWidget *da2;
   GtkWidget *label2;

   dw = calloc(1, sizeof(DW));
   pwin->detailwin_data = dw;
   dw->pwin = pwin;

   win = gtk_window_new (GTK_WINDOW_TOPLEVEL);
   gtk_window_set_title (GTK_WINDOW (win), "Extra Details");
   gtk_window_set_default_size (GTK_WINDOW (win), 400, 300);

   notebook1 = gtk_notebook_new ();
   gtk_widget_show (notebook1);
   gtk_container_add (GTK_CONTAINER (win), notebook1);
   gtk_container_set_border_width (GTK_CONTAINER (notebook1), 4);

   frame1 = gtk_frame_new (NULL);
   gtk_widget_show (frame1);
   gtk_container_add (GTK_CONTAINER (notebook1), frame1);
   gtk_container_set_border_width (GTK_CONTAINER (frame1), 4);
   gtk_frame_set_shadow_type (GTK_FRAME (frame1), GTK_SHADOW_IN);

   alignment1 = gtk_alignment_new (0.5, 0.5, 1, 1);
   gtk_widget_show (alignment1);
   gtk_container_add (GTK_CONTAINER (frame1), alignment1);

   da1 = gtk_drawing_area_new ();
   gtk_widget_show (da1);
   gtk_container_add (GTK_CONTAINER (alignment1), da1);

   label1 = gtk_label_new ("Time Graph");
   gtk_widget_show (label1);
   gtk_notebook_set_tab_label (GTK_NOTEBOOK (notebook1), gtk_notebook_get_nth_page (GTK_NOTEBOOK (notebook1), 0), label1);

   frame2 = gtk_frame_new (NULL);
   gtk_widget_show (frame2);
   gtk_container_add (GTK_CONTAINER (notebook1), frame2);
   gtk_container_set_border_width (GTK_CONTAINER (frame2), 4);
   gtk_frame_set_shadow_type (GTK_FRAME (frame2), GTK_SHADOW_IN);

   alignment2 = gtk_alignment_new (0.5, 0.5, 1, 1);
   gtk_widget_show (alignment2);
   gtk_container_add (GTK_CONTAINER (frame2), alignment2);

   da2 = gtk_drawing_area_new ();
   gtk_widget_show (da2);
   gtk_container_add (GTK_CONTAINER (alignment2), da2);

   label2 = gtk_label_new ("Memory Usage Maps");
   gtk_widget_show (label2);
   gtk_notebook_set_tab_label (GTK_NOTEBOOK (notebook1), gtk_notebook_get_nth_page (GTK_NOTEBOOK (notebook1), 1), label2);

   dw->win = win;
   dw->da1 = da1;
   dw->da2 = da2;

   g_signal_connect ((gpointer) da1, "expose_event",
		     G_CALLBACK (on_da1_expose_event),
		     dw);
   g_signal_connect ((gpointer) da2, "expose_event",
		     G_CALLBACK (on_da2_expose_event),
		     dw);

   gtk_widget_show(win);
}

void
dw_shutdown(ProcessWindow *pwin)
{
   DW *dw;

   dw = pwin->detailwin_data;
   free(dw);
   pwin->detailwin_data = NULL;
}
