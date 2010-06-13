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

static void
dw_draw_memstats(ProcessWindow *pwin)
{
   GtkAllocation allocation;
   GdkWindow *window;
   GtkWidget *widget;
   GtkStyle *style;
   GdkPixmap *pixmap;
   GtkStateType state;
   gint64 i, j, x, y, w, h, hh;

   widget = pwin->time_graph;
   window = gtk_widget_get_window (widget);
   if (!window)
       return;

   style = gtk_widget_get_style (widget);
   gtk_widget_get_allocation(widget, &allocation);
   w = allocation.width;
   h = allocation.height;
   pixmap = gdk_pixmap_new(gtk_widget_get_window (widget), w, h, -1);
   gdk_draw_rectangle(pixmap,
		      style->base_gc[GTK_STATE_NORMAL],
		      TRUE,
		      0, 0, w, h);
   for (i = 0; i < MEMSTATS; i++)
     {
	x = w - i;
	if (x < 0) break;
	if (pwin->usage_high > 0)
	  hh = (h * pwin->memstats[i][1]) / pwin->usage_high;
	else
	  hh = 0;
	y = h - hh;
	gdk_draw_rectangle(pixmap,
			   style->base_gc[GTK_STATE_SELECTED],
			   TRUE,
			   x, y, 1, hh);
	if (pwin->usage_high > 0)
	  hh = (h * pwin->memstats[i][0]) / pwin->usage_high;
	else
	  hh = 0;
	y = h - hh;
	gdk_draw_rectangle(pixmap,
			   style->text_gc[GTK_STATE_NORMAL],
			   TRUE,
			   x, y, 1, hh);
     }
   if (pwin->usage_high > 0)
     {
	GdkGC *gc;

	gc = gdk_gc_new(pixmap);
	gdk_gc_copy(gc, style->dark_gc[GTK_STATE_NORMAL]);
	gdk_gc_set_line_attributes(gc, 0, GDK_LINE_ON_OFF_DASH,
				   GDK_CAP_BUTT, GDK_JOIN_MITER);
	for (j = 0, i = 0; i < pwin->usage_high; i += (256 * 1024), j++)
	  {
	     if (j > 3) j = 0;
	     y = h - ((i * h) / pwin->usage_high);
	     if (j == 0)
	       gdk_draw_line(pixmap, style->dark_gc[GTK_STATE_NORMAL],
			     0, y, w, y);
	     else
	       gdk_draw_line(pixmap, gc,
			     0, y, w, y);
	  }
	g_object_unref(gc);
     }

   state = gtk_widget_get_state (widget);
   gdk_draw_drawable(gtk_widget_get_window (widget),
		   gtk_widget_get_style (widget)->fg_gc[state],
		   pixmap, 0, 0, 0, 0, w, h);
   g_object_unref(pixmap);
}

typedef struct _Mem
{
   GtkWidget *widget;
   GdkPixmap *pixmap;
   GdkGC *gc_bg, *gc_bg2, *gc_mem, *gc_mem2, *gc_swapped, *gc_present;
   guint w, h, bpp;
   gulong mtotal;
   GList *regs;
} Mem;

typedef struct _MemReg
{
   void *start;
   gchar *file;
   unsigned long size;
   int y, h;
} MemReg;

static void
dw_draw_memmap_foreach(gpointer key, gpointer value, gpointer data)
{
   Mem *mem;
   Block *block;
   guint64 x, y, w, i;
   GdkPixmap *pixmap;
   GdkGC *gc;
   gsize addr;
   guint64 bpp, memw;
   GList *l;
   MemReg *reg;
   
   
   mem = data;
   block = value;
   bpp = mem->bpp;
   addr = GPOINTER_TO_SIZE(block->addr);
   for (i = 0, l = mem->regs; l; l = l->next, i++)
     {
	reg = l->data;
	if ((addr >= GPOINTER_TO_SIZE(reg->start)) &&
	    (addr < GPOINTER_TO_SIZE(reg->start + reg->size)))
	  break;
	reg = NULL;
     }
   if (!reg) return;
   addr = addr - GPOINTER_TO_SIZE(reg->start);
   w = (guint64)(addr + block->size + (bpp / 2)) / bpp;
   x = (guint64)(addr) / bpp;
   w -= x;
   memw = mem->w;
   y = reg->y + (x / memw);
   x -= (y * memw);
   pixmap = mem->pixmap;
   if (i & 0x1)
     gc = mem->gc_mem2;
   else
     gc = mem->gc_mem;
   gdk_draw_rectangle(pixmap, gc, TRUE, x, y, w, 1);
   if ((x + w) > mem->w)
     gdk_draw_rectangle(pixmap, gc, TRUE, 0, y + 1, w - (memw - x), 1);
}

static void
dw_draw_memmap(ProcessWindow *pwin)
{
   GtkAllocation allocation;
   GdkWindow *window;
   GtkWidget *widget;
   GtkStyle *style;
   GtkStateType state;
   GdkPixmap *pixmap, *stip;
   guint64 w, h, y, bpl, i;
   Mem mem;
   GList *l;
   GdkColor col;
   gint regnum = 0;
   GdkGC *gc_none, *gc_bg, *gc_bg2, *gc_mem, *gc_mem2, *gc_swapped, *gc_present, *gc_divider, *gc_0, *gc_1;
   

   /* disabled */
   if (!pwin->draw_memmap)
     return;

   widget = pwin->mem_map;
   window = gtk_widget_get_window (widget);
   if (!window) return;

   style = gtk_widget_get_style (widget);
   gtk_widget_get_allocation(widget, &allocation);
   w = allocation.width;
   h = allocation.height;
   pixmap = gdk_pixmap_new(window, w, h, -1);
   stip = gdk_pixmap_new(window, 2, 2, 1);
   
   gc_none = gdk_gc_new(pixmap);
   gc_bg = gdk_gc_new(pixmap);
   gc_bg2 = gdk_gc_new(pixmap);
   gc_mem = gdk_gc_new(pixmap);
   gc_mem2 = gdk_gc_new(pixmap);
   gc_swapped = gdk_gc_new(pixmap);
   gc_present = gdk_gc_new(pixmap);
   gc_divider = gdk_gc_new(pixmap);
   gc_0 = gdk_gc_new(stip);
   gc_1 = gdk_gc_new(stip);

   col.pixel = 0;
   gdk_gc_set_foreground(gc_0, &col);
   col.pixel = 1;
   gdk_gc_set_foreground(gc_1, &col);
   gdk_draw_rectangle(stip,
		      gc_0,
		      TRUE,
		      0, 0, 2, 2);
   gdk_draw_rectangle(stip,
		      gc_1,
		      TRUE,
		      1, 0, 1, 1);
   gdk_draw_rectangle(stip,
		      gc_1,
		      TRUE,
		      0, 1, 1, 1);

   col.red = 0xffff; col.green = 0xffff; col.blue = 0xffff;
   gdk_gc_set_rgb_fg_color(gc_none, &col);
   col.red = 0xaaaa; col.green = 0xaaaa; col.blue = 0xaaaa;
   gdk_gc_set_rgb_fg_color(gc_bg, &col);
   col.red = 0x8888; col.green = 0x8888; col.blue = 0x8888;
   gdk_gc_set_rgb_fg_color(gc_bg2, &col);
   col.red = 0xffff; col.green = 0xffff; col.blue = 0x8888;
   gdk_gc_set_rgb_fg_color(gc_mem, &col);
   col.red = 0xcccc; col.green = 0xcccc; col.blue = 0x2222;
   gdk_gc_set_rgb_fg_color(gc_mem2, &col);
   col.red = 0x8888; col.green = 0x0000; col.blue = 0x0000;
   gdk_gc_set_rgb_fg_color(gc_swapped, &col);
   gdk_gc_set_stipple(gc_swapped, stip);
   gdk_gc_set_ts_origin(gc_swapped, 0, 0);
   gdk_gc_set_fill(gc_swapped, GDK_STIPPLED);
   col.red = 0x6666; col.green = 0xaaaa; col.blue = 0x6666;
   gdk_gc_set_rgb_fg_color(gc_present, &col);
   col.red = 0x4444; col.green = 0x4444; col.blue = 0x4444;
   gdk_gc_set_rgb_fg_color(gc_divider, &col);
   
   gdk_draw_rectangle(pixmap,
		      gc_none,
		      TRUE,
		      0, 0, w, h);
   mem.regs = NULL;
   mem.widget = widget;
   mem.pixmap = pixmap;
   mem.gc_bg = gc_bg;
   mem.gc_bg2 = gc_bg2;
   mem.gc_mem = gc_mem;
   mem.gc_mem2 = gc_mem2;
   mem.gc_swapped = gc_swapped;
   mem.gc_present = gc_present;
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

	snprintf(buffer, 1023, "/proc/%d/maps", pwin->process->pid);

	in = fopen(buffer, "r");
	if (!in)
	  {
	     g_warning("Failed to open: '%s'", buffer);
	     return;
	  }

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
                       reg->file = g_strdup(file);
		       mem.mtotal += reg->size;
		       mem.regs = g_list_append(mem.regs, reg);
                       regnum++;
		    }
	       }
	  }
        fclose (in);
     }
   bpl = (mem.mtotal + (h - regnum) - 1) / (h - regnum);
   y = 0;
   mem.bpp = (bpl + w - 1) / w;
   if (mem.bpp < 1) mem.bpp = 1;
     {
        gchar buffer[1024];
        FILE *in;
        
        snprintf(buffer, 1023, "/proc/%d/pagemap", pwin->process->pid);
        in = fopen(buffer, "rb");
        for (i = 0, l = mem.regs; l; l = l->next, i++)
          {
             MemReg *reg;
             
             reg = l->data;
             reg->h = ((reg->size * (h - regnum)) + bpl - 1) / mem.mtotal;
             reg->y = y;
             y += reg->h;
             y += 1;
             if (i & 0x1)
               gdk_draw_rectangle(pixmap,
                                  gc_bg2,
                                  TRUE,
                                  0, reg->y, w, reg->h);
             else
               gdk_draw_rectangle(pixmap,
                                  gc_bg,
                                  TRUE,
                                  0, reg->y, w, reg->h);
             gdk_draw_rectangle(pixmap,
                                gc_divider,
                                TRUE,
                                0, reg->y + reg->h, w, 1);
             if (in)
               {
                  guint64 j, pos;
                  
                  pos = (GPOINTER_TO_SIZE(reg->start) >> 12) << 3;
                  fseek(in, pos, SEEK_SET);
                  for (j = 0; j < (reg->size >> 12); j++)
                    {
                       guint64 bpp, memw;
                       guint64 addr;
                       guint64 info;
                       guint swapped, present;
                       guint64 x, y, w;
                       GdkGC *gc;
                       
                       if (fread(&info, sizeof(guint64), 1, in) != 1)
                         continue;

                       swapped = (info & (((guint64)1) << 62)) >> 62;
                       present = (info & (((guint64)1) << 63)) >> 63;
                       if ((!present) && (!swapped)) continue;
                       bpp = mem.bpp;
                       addr = ((guint64)j) << 12;
                       w = (guint64)(addr + (1 << 12) + (bpp / 2)) / bpp;
                       x = (guint64)(addr) / bpp;
                       w -= x;
                       memw = mem.w;
                       y = reg->y + (x / memw);
                       x -= (y * memw);
                       if (!swapped)
                         {
                            gc = gc_present;
                            gdk_draw_rectangle(pixmap, gc, TRUE, x, y, w, 1);
                            if ((x + w) > mem.w)
                              gdk_draw_rectangle(pixmap, gc, TRUE, 0, y + 1, w - (memw - x), 1);
                         }
                    }
               }
          }
        if (in) fclose(in);
     }
   
   g_hash_table_foreach(pwin->process->block_table,
			dw_draw_memmap_foreach,
			&mem);
     {
        gchar buffer[1024];
        FILE *in;
        
        snprintf(buffer, 1023, "/proc/%d/pagemap", pwin->process->pid);
        in = fopen(buffer, "rb");
        y = 0;
        for (i = 0, l = mem.regs; l; l = l->next, i++)
          {
             MemReg *reg;
             
             reg = l->data;
             reg->h = ((reg->size * (h - regnum)) + bpl - 1) / mem.mtotal;
             reg->y = y;
             y += reg->h;
             y += 1;
             if (in)
               {
                  guint64 j, pos;
                  
                  pos = (GPOINTER_TO_SIZE(reg->start) >> 12) << 3;
                  fseek(in, pos, SEEK_SET);
                  for (j = 0; j < (reg->size >> 12); j++)
                    {
                       guint64 bpp, memw;
                       guint64 addr;
                       guint64 info;
                       guint swapped, present;
                       guint64 x, y, w;
                       GdkGC *gc;
                       
                       if (fread(&info, sizeof(guint64), 1, in) != 1)
                          continue;

                       swapped = (info & (((guint64)1) << 62)) >> 62;
                       if (swapped)
                         {
                            present = (info & (((guint64)1) << 63)) >> 63;
                            bpp = mem.bpp;
                            addr = ((guint64)j) << 12;
                            w = (guint64)(addr + (1 << 12) + (bpp / 2)) / bpp;
                            x = (guint64)(addr) / bpp;
                            w -= x;
                            memw = mem.w;
                            y = reg->y + (x / memw);
                            x -= (y * memw);
                            gc = gc_swapped;
                            gdk_draw_rectangle(pixmap, gc, TRUE, x, y, w, 1);
                            if ((x + w) > mem.w)
                              gdk_draw_rectangle(pixmap, gc, TRUE, 0, y + 1, w - (memw - x), 1);
                         }
                       
                    }
               }
             gtk_draw_string(style,
                             pixmap,
                             GTK_STATE_NORMAL,
                             0 + 5, reg->y + reg->h - 5,
                             reg->file);
          }
        if (in) fclose(in);
     }
   for (l = mem.regs; l; l = l->next)
     {
        MemReg *reg;
        
        reg = l->data;
        g_free(reg->file);
        g_free(reg);
     }
   g_list_free(mem.regs);

   state = gtk_widget_get_state (widget);
   gdk_draw_drawable(window,
		     style->fg_gc[state],
		     pixmap, 0, 0, 0, 0, w, h);
   
   g_object_unref(gc_none);
   g_object_unref(gc_bg);
   g_object_unref(gc_bg2);
   g_object_unref(gc_mem);
   g_object_unref(gc_mem2);
   g_object_unref(gc_swapped);
   g_object_unref(gc_present);
   g_object_unref(gc_0);
   g_object_unref(gc_1);
   g_object_unref(pixmap);
   g_object_unref(stip);
}

gboolean
time_graph_expose_event(GtkWidget *widget, GdkEventExpose  *event, ProcessWindow *pwin)
{
   if (!pwin->process) return FALSE;
   dw_draw_memstats(pwin);
   return FALSE;
}

gboolean
mem_map_expose_event(GtkWidget *widget, GdkEventExpose  *event, ProcessWindow *pwin)
{
   if (!pwin->process) return FALSE;
   dw_draw_memmap(pwin);
   return FALSE;
}

void
dw_update(ProcessWindow *pwin)
{
   guint i;
   if (!pwin->process) return;
   for (i = MEMSTATS - 1; i > 0; i--)
     {
	pwin->memstats[i][0] = pwin->memstats[i - 1][0];
	pwin->memstats[i][1] = pwin->memstats[i - 1][1];
     }
   pwin->memstats[0][0] = pwin->process->bytes_used;
   pwin->memstats[0][1] = pwin->usage_high;
   dw_draw_memstats(pwin);
   dw_draw_memmap(pwin);
}
