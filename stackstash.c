/* -*- mode: C; c-file-style: "linux" -*- */

/* MemProf -- memory profiler and leak detector
 * Copyright 1999, 2000, 2001, 2002, Red Hat, Inc.
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

#include <stdlib.h>
#include <glib.h>
#include "stackstash.h"

/* #define BUILD_TEST_CASE */

#ifdef BUILD_TEST_CASE
static size_t test_unique_bytes = 0;
static size_t test_bytes_used = 0;
static int test_nodes_used = 0;
#endif

struct _StackStash {
	StackElement *root;
};

static StackElement *
stack_element_new (int n_children)
{
	StackElement *element = g_malloc (G_STRUCT_OFFSET (StackElement, children) + n_children * sizeof (StackElement *));
	element->n_children = n_children;

#ifdef BUILD_TEST_CASE	
	test_bytes_used += G_STRUCT_OFFSET (StackElement, children) + n_children * sizeof (StackElement *);
	test_nodes_used++;
#endif

	return element;
}

static void
stack_element_free (StackElement *element)
{
#ifdef BUILD_TEST_CASE	
	test_bytes_used -= G_STRUCT_OFFSET (StackElement, children) + element->n_children * sizeof (StackElement *);
	test_nodes_used--;
#endif
	
	g_free (element);
}

static void
copy_children_and_insert (StackElement *new,
			  StackElement *old,
			  StackElement *child)
{
	StackElement **old_child = old->children;
	StackElement **new_child = new->children;
	int n_children = old->n_children;
	gpointer child_address = child->address;

	while (n_children && (*old_child)->address < child_address) {
		*new_child = *old_child;
		(*new_child)->parent = new;
		new_child++;
		old_child++;
		n_children--;
	}
	*new_child = child;
	child->parent = new;
	new_child++;
	while (n_children) {
		*new_child = *old_child;
		(*new_child)->parent = new;
		new_child++;
		old_child++;
		n_children--;
	}
}

static void
replace_child (StackElement *parent,
	       StackElement *old_child,
	       StackElement *new_child)
{
	StackElement **child = parent->children;

	while (TRUE) {
		if (*child == old_child) {
			*child = new_child;
			return;
		}
		child++;
	}
}

StackStash *
stack_stash_new (void)
{
	StackStash *stash = g_new (StackStash, 1);

	stash->root = stack_element_new (0);
	stash->root->parent = NULL;
	stash->root->address = NULL;

	return stash;
}

StackElement *
stack_stash_store (StackStash *stash,
		   gpointer   *addresses,
		   int         n_addresses)
{
	StackElement *current;
	gboolean first_new;
	
	current = stash->root;
	
	while (n_addresses)
	{
		int first = 0;
		int last = current->n_children;
		gpointer address = addresses[n_addresses - 1];

		while (first < last) {
			int mid = (first + last) / 2;
			if (address == current->children[mid]->address) {
				current = current->children[mid];
				goto next_address;
			} else if (address < current->children[mid]->address) {
				last = mid;
			} else
				first = mid + 1;
		}

		break;
		
	next_address:
		n_addresses--;
	}

	first_new = TRUE;
	while (n_addresses) {
		StackElement *next = stack_element_new (n_addresses == 1 ? 0 : 1);
		next->address = addresses[n_addresses - 1];
		
		if (first_new) {
			StackElement *new_current = stack_element_new (current->n_children + 1);
			new_current->parent = current->parent;
			new_current->address = current->address;

			copy_children_and_insert (new_current, current, next);
			
			if (current == stash->root)
				stash->root = new_current;
			else
				replace_child (new_current->parent, current, new_current);

			stack_element_free (current);
			first_new = FALSE;
		} else {
			current->children[0] = next;
			next->parent = current;
		}

		current = next;
		n_addresses--;
	}

  return current;
}

static void
free_element_recurse (StackElement *element)
{
	int i;
	for (i = 0; i < element->n_children; i++)
		free_element_recurse (element->children[i]);

	stack_element_free (element);
}

void
stack_stash_free (StackStash *stash)
{
	free_element_recurse (stash->root);
	g_free (stash);
}

#ifdef BUILD_TEST_CASE
guint
stack_trace_hash (const void *trace)
{
	guint result = 0;
	const guint32 *p = trace;
	guint32 n_elements = *p;

	while (n_elements--) {
		p++;
		result ^= *p;
	}

	return result;
}

gboolean
stack_trace_equal (const void *trace_a,
		   const void *trace_b)
{
	const guint32 *p = trace_a;
	const guint32 *q = trace_b;

	guint32 n_elements = *p;
	if (n_elements != *q)
		return FALSE;
	
	while (n_elements--) {
		p++;
		q++;
		if (*p != *q)
			return FALSE;
	}

	return TRUE;
}

void *
stack_trace_copy (const void *trace)
{
	size_t n_bytes = sizeof (guint32) * (1 + *(guint32 *)trace);
	test_unique_bytes += n_bytes;
	return g_memdup (trace, n_bytes);
}

int main (void)
{
	char *bytes;
	guint32 *p;
	size_t len;
	GError *err = NULL;
	int n_traces, i;
	StackStash *stash;
	StackElement **traces;
	int n_unique = 0;;

	GHashTable *unique_hash = g_hash_table_new_full (stack_trace_hash, stack_trace_equal,
							 (GDestroyNotify)g_free, NULL);
	
	GTimer *timer = g_timer_new ();
	
	if (!g_file_get_contents ("stacktrace.test", &bytes, &len, &err)) {
		g_message ("Cannot read stracktrace.test: %s\n", err->message);
		exit (1);
	}
	
	/* Count the number of stacktraces
	 */
	g_timer_start (timer);
	
	n_traces = 0;
	p = (guint32 *)bytes;
	while (p < (guint32 *)(bytes + len)) {
		guint32 n_elements = *p;

		if (!g_hash_table_lookup (unique_hash, p)) { 
			g_hash_table_insert (unique_hash, stack_trace_copy (p), GUINT_TO_POINTER (1));
			n_unique++;
		}
		
		p += n_elements + 1;
		n_traces++;
	}
	g_timer_stop (timer);
	g_print ("Preparation took %g seconds\n", g_timer_elapsed (timer, NULL));
	
	traces = g_new (StackElement *, n_traces);

	/* Insert them all
	 */
	g_timer_start (timer);

	stash = stack_stash_new ();
	
	p = (guint32 *)bytes;
	for (i = 0; i < n_traces; i++) {
		guint32 n_elements = *p;
		p++;
		traces[i] = stack_stash_store (stash, (gpointer *)p, n_elements);
		p += n_elements;
	}

	g_timer_stop (timer);
	g_print ("Inserted %d stacktraces, %zd bytes (unique: %d/%zd):\n\t%g seconds, using %zd bytes, %d nodes\n",
		 n_traces, len,
		 n_unique, test_unique_bytes,
		 g_timer_elapsed (timer, NULL), test_bytes_used, test_nodes_used);

	/* Now verify the inserted traces
	 */
	p = (guint32 *)bytes;
	for (i = 0; i < n_traces; i++) {
		StackElement *tmp_element = traces[i];
		guint32 n_elements = *p;
		p++;
		
		while (n_elements--) {
			g_assert (tmp_element->address == *(gpointer *)p);
			g_assert (tmp_element->parent);
			tmp_element = tmp_element->parent;
			p++;
		}
		g_assert (!tmp_element->parent);
	}

	g_free (traces);
	stack_stash_free (stash);
	g_hash_table_destroy (unique_hash);
	g_free (bytes);
	g_timer_destroy (timer);

	return 0;
}
#endif /* BUILD_TEST_CASE */
