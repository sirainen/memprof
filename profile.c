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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <libgnome/libgnome.h>	/* For i18n */

#include "profile.h"

static GList *
add_bytes_to_ref (GList *reflist, ProfileFunc *function, guint bytes)
{
  ProfileFuncRef *ref = NULL;	/* Quiet GCC */
  GList *tmp_list;

  tmp_list = reflist;
  while (tmp_list)
    {
      ref = tmp_list->data;
      if (ref->function == function)
	break;
      
      tmp_list = tmp_list->next;
    }

  if (!tmp_list)
    {
      ref = g_new (ProfileFuncRef, 1);
      ref->function = function;
      ref->bytes = 0;
      reflist = g_list_prepend (reflist, ref);
    }

  ref->bytes += bytes;
  
  return reflist;
}

static void
add_block_to_functions (gpointer key, gpointer value, gpointer data)
{
  Block *block = value;
  ProfileFunc *last_function = NULL;
  int i;

  Profile *profile = data;
  gboolean first = TRUE;

  for (i=0; i<block->stack_size; i++)
    {
      Symbol *symbol;
      ProfileFunc *function;
      
      symbol = process_locate_symbol (profile->process, (guint)block->stack[i]);
      if (symbol)
	{
	  if (g_hash_table_lookup (profile->skip_hash, symbol->name))
	    continue;
	  
	  function = g_hash_table_lookup (profile->function_hash, symbol);
	  if (!function)
	    {
	      function = g_new0 (ProfileFunc, 1);
	      function->symbol = symbol;
	      g_hash_table_insert (profile->function_hash, symbol, function);
	    }

	  if (first)
	    {
	      function->self += block->size;
	      first = FALSE;
	    }

	  function->total += block->size;

	  if (last_function)
	    {
	      function->children =
		add_bytes_to_ref (function->children, last_function, block->size);
	      last_function->inherited =
		add_bytes_to_ref (last_function->inherited, function, block->size);
	    }

	  last_function = function;
	}
    }
}

static void
add_function_to_array (gpointer key, gpointer value, gpointer data)
{
  GArray *functions = data;
  ProfileFunc *function = value;

  g_array_append_vals (functions, &function, 1);
}

static gint
compare_function (const void *a, const void *b)
{
  ProfileFunc * const *f1 = a;
  ProfileFunc * const *f2 = b;

  return strcmp ((*f1)->symbol->name, (*f2)->symbol->name);
}

Profile *
profile_create (MPProcess *process, char **skip_funcs, gint n_skip_funcs)
{
  GArray *functions;
  Profile *profile;
  int i;

  profile = g_new (Profile, 1);
  profile->process = process;
  
  profile->skip_hash = g_hash_table_new (g_str_hash, g_str_equal);
  for (i=0; i<n_skip_funcs; i++)
    g_hash_table_insert (profile->skip_hash, skip_funcs[i], "");
  
  /* Go through all blocks, and add up memory
   */
  profile->function_hash = g_hash_table_new (g_direct_hash, NULL);
  g_hash_table_foreach (process->block_table, add_block_to_functions, profile);

  /* Make a sorted list of functions
   */
  functions = g_array_new (FALSE, FALSE, sizeof(ProfileFunc *));
  g_hash_table_foreach (profile->function_hash, add_function_to_array, functions);

  qsort (functions->data, functions->len, sizeof(ProfileFunc *), compare_function);

  profile->n_functions = functions->len;
  profile->functions = (ProfileFunc **)functions->data;
  
  g_hash_table_destroy (profile->function_hash);
  profile->function_hash = NULL;
  g_hash_table_destroy (profile->skip_hash);
  profile->skip_hash = NULL;
  g_array_free (functions, FALSE);

  return profile;
}

static void
print_refs (FILE *out, GList *reflist)
{
  ProfileFuncRef *ref;
  GList *tmp_list;

  tmp_list = reflist;
  while (tmp_list)
    {
      ref = tmp_list->data;
      fprintf (out, "        %s: %u\n", ref->function->symbol->name, ref->bytes);
      
      tmp_list = tmp_list->next;
    }
}

static gint
refs_compare (const void *a, const void *b)
{
  const ProfileFuncRef *refa = a;
  const ProfileFuncRef *refb = b;

  return strcmp (refa->function->symbol->name, refb->function->symbol->name);
}

void
profile_write (Profile *profile, gchar *outfile)
{
  int i;
  FILE *out;
  
  /* Print results
   */

  out = fopen (outfile, "w");
  if (!out)
    {
      show_error (ERROR_MODAL, _("Cannot open output file: %s\n"), g_strerror (errno));
      return;
    }
  
  for (i=0; i<profile->n_functions; i++)
    {
      fprintf (out, "%s\n", profile->functions[i]->symbol->name);
      fprintf (out, "  children:\n");
      profile->functions[i]->children = g_list_sort (profile->functions[i]->children, refs_compare);
      print_refs (out, profile->functions[i]->children);
      fprintf (out, "  total: %d\n", profile->functions[i]->total);
      fprintf (out, "  self:  %d\n", profile->functions[i]->self);
      fprintf (out, "  inherited:\n");
      profile->functions[i]->inherited = g_list_sort (profile->functions[i]->inherited, refs_compare);
      print_refs (out, profile->functions[i]->inherited);
      
      fprintf (out, "\n");
    }

  fclose (out);
}

void
profile_free (Profile *profile)
{
  int i;
  
  for (i=0; i<profile->n_functions; i++)
    {
      g_list_foreach (profile->functions[i]->children, (GFunc)g_free, NULL);
      g_list_foreach (profile->functions[i]->inherited, (GFunc)g_free, NULL);
      g_free (profile->functions[i]);
    }

  g_free (profile);
}
