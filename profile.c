/* MemProf -- memory profiler and leak detector
 * Copyright 2002, Soeren Sandmann
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

/*=====*/

#include "profile.h"
#include <glib.h>

static GList *
block_create_stack_list (Block *block, MPProcess *process, GHashTable *skip_hash)
{
    StackElement *element;
    GList *stack = NULL;
    
    for (element = block->stack; !STACK_ELEMENT_IS_ROOT (element); element = element->parent)
    {
	const Symbol *symbol = process_locate_symbol (process, (guint)element->address);
	
	if (symbol && symbol->name && g_hash_table_lookup (skip_hash, symbol->name))
	    continue;
	
	stack = g_list_prepend (stack, element);
    }
    
    return stack;
}

static void
profile_add_stack_trace (Profile *profile, GList *stack, guint size)
{
    GList *list;
    GPtrArray *roots = profile->roots;
    ProfileNode *parent = NULL;
    GHashTable *seen_symbols = g_hash_table_new_full (symbol_hash, symbol_equal,
						      (GDestroyNotify)symbol_free, NULL);

    for (list = stack; list != NULL; list = list->next)
    {
	StackElement *element = list->data;
	ProfileNode *match = NULL;
	const Symbol *symbol =
	    process_locate_symbol (profile->process, (guint)element->address);
	int i;
	
	for (i = 0; i < roots->len; ++i)
	{
	    ProfileNode *node = roots->pdata[i];
	    
	    if (symbol_equal (node->symbol, symbol))
		match = node;
	}
	
	if (!match)
	{
	    ProfileNode *next_node;
	    
	    match = g_new (ProfileNode, 1);
	    
	    match->symbol = symbol_copy (symbol);
	    match->total = 0;
	    match->self = 0;
	    
	    if (g_hash_table_lookup (seen_symbols, symbol))
		match->toplevel = FALSE;
	    else
		match->toplevel = TRUE;
	    
	    next_node = g_hash_table_lookup (profile->nodes_by_symbol, symbol);
	    if (next_node)
		match->next = next_node;
	    else
		match->next = NULL;
	    g_hash_table_insert (profile->nodes_by_symbol, symbol_copy (symbol), match);
	    
	    match->children = g_ptr_array_new ();
	    match->parent = parent;
	    
	    g_ptr_array_add (roots, match);
	}
	
	g_hash_table_insert (seen_symbols, symbol_copy (symbol), GINT_TO_POINTER (1));
	
	match->total += size;
	if (!list->next)
	    match->self += size;
	
	parent = match;
	roots = match->children;
    }
    
    g_hash_table_destroy (seen_symbols);
}

static void
profile_build_tree (Profile *profile, GList *blocks, GSList *skip_funcs)
{
    GHashTable *skip_hash = g_hash_table_new (g_str_hash, g_str_equal);
    GList *list;
    GSList *slist;
    
    for (slist = skip_funcs; slist != NULL; slist = slist->next)
	g_hash_table_insert (skip_hash, slist->data, "");
    
    for (list = blocks; list != NULL; list = list->next)
    {
	Block *block = list->data;
	GList *stack = block_create_stack_list (block, profile->process, skip_hash);
	
	profile_add_stack_trace (profile, stack, block->size);
	
	g_list_free (stack);
    }
    
    g_hash_table_destroy (skip_hash);
}

static void
add_function (gpointer key, gpointer value, gpointer data)
{
    ProfileFunc *func;
    ProfileNode *node;
    Profile *profile = data;
    
    node = value;
    
    func = g_new (ProfileFunc, 1);
    func->node = node;
    func->total = 0;
    func->self = 0;
    
    while (node)
    {
	func->self += node->self;
	if (node->toplevel)
	    func->total += node->total;
	
	node = node->next;
    }
    
    g_ptr_array_add (profile->functions, func);
}

static void
add_block_to_list (Block *block, gpointer data)
{
    GList **blocks = data;
    
    *blocks = g_list_prepend (*blocks, block);
}

Profile *
profile_create (MPProcess *process, GSList *skip_funcs)
{ 
    Profile *profile;
    GList *blocks = NULL;
    GList *list;
    
    process_block_foreach (process, add_block_to_list, &blocks);
    
    profile = g_new (Profile, 1);
    profile->roots = g_ptr_array_new ();
    profile->functions = g_ptr_array_new ();
    profile->n_bytes = 0;
    profile->process = process;
    profile->nodes_by_symbol = g_hash_table_new_full (symbol_hash, symbol_equal,
						      (GDestroyNotify)symbol_free, NULL);
    
    profile_build_tree (profile, blocks, skip_funcs);
    
    for (list = blocks; list != NULL; list = list->next)
    {
	Block *block = list->data;
	
	profile->n_bytes += block->size;
    }
    
    g_hash_table_foreach (profile->nodes_by_symbol, add_function, profile);
    
    g_list_free (blocks);
    return profile;
}

static void
profile_node_free (ProfileNode *node)
{
    int i;
    
    for (i = 0; i < node->children->len; ++i)
	profile_node_free (node->children->pdata[i]);
    if (node->symbol)
	symbol_free (node->symbol);
    g_ptr_array_free (node->children, TRUE);
    g_free (node);
}

void
profile_free (Profile *profile)
{
    int i;
    
    for (i = 0; i < profile->roots->len; ++i)
	profile_node_free (profile->roots->pdata[i]);
    g_ptr_array_free (profile->roots, TRUE);
    
    for (i = 0; i < profile->functions->len; ++i)
	g_free (profile->functions->pdata[i]);
    g_ptr_array_free (profile->functions, TRUE);
    
    g_hash_table_destroy (profile->nodes_by_symbol);
    g_free (profile);
}

static void
node_list_leaves (ProfileNode *node, GList **leaves)
{
    int i;
    
    if (node->self > 0)
	*leaves = g_list_prepend (*leaves, node);
    
    for (i = 0; i < node->children->len; ++i)
	node_list_leaves (node->children->pdata[i], leaves);
}

static void
add_trace_to_tree (GPtrArray *roots, GList *trace, guint size)
{
    GList *list;
    GList *nodes_to_unmark = NULL;
    GList *nodes_to_unmark_recursive = NULL;
    ProfileDescendantTreeNode *parent = NULL;
    
    GHashTable *seen_symbols = g_hash_table_new_full (symbol_hash, symbol_equal,
						      (GDestroyNotify)symbol_free, NULL);
    
    for (list = trace; list != NULL; list = list->next)
    {
	int i;
	ProfileNode *node = list->data;
	ProfileDescendantTreeNode *match = NULL;
	
	for (i = 0; i < roots->len; ++i)
	{
	    ProfileDescendantTreeNode *tree_node = roots->pdata[i];
	    
	    if (symbol_equal (tree_node->symbol, node->symbol))
		match = tree_node;
	}
	
	if (!match)
	{
	    ProfileDescendantTreeNode *seen_tree_node;
	    
	    seen_tree_node = g_hash_table_lookup (seen_symbols, node->symbol);
	    
	    if (seen_tree_node)
	    {
		ProfileDescendantTreeNode *node;
		
		g_assert (parent);
		
		for (node = parent; node != seen_tree_node->parent; node = node->parent)
		{
		    node->non_recursion -= size;
		    --node->marked_non_recursive;
		}
		
		match = seen_tree_node;
		
		g_hash_table_destroy (seen_symbols);
		seen_symbols = g_hash_table_new_full (symbol_hash, symbol_equal,
						      (GDestroyNotify)symbol_free, NULL);
		
		for (node = match; node != NULL; node = node->parent)
		    g_hash_table_insert (seen_symbols, symbol_copy (node->symbol), node);
		
	    }
	}
	
	if (!match)
	{
	    match = g_new (ProfileDescendantTreeNode, 1);
	    
	    match->symbol = symbol_copy (node->symbol);
	    match->non_recursion = 0;
	    match->total = 0;
	    match->self = 0;
	    match->children = g_ptr_array_new ();
	    match->marked_non_recursive = 0;
	    match->marked_total = FALSE;
	    match->parent = parent;
	    
	    g_ptr_array_add (roots, match);
	}
	
	if (!match->marked_non_recursive)
	{
	    nodes_to_unmark = g_list_prepend (nodes_to_unmark, match);
	    match->non_recursion += size;
	    ++match->marked_non_recursive;
	}
	
	if (!match->marked_total)
	{
	    nodes_to_unmark_recursive = g_list_prepend (
		nodes_to_unmark_recursive, match);
	    
	    match->total += size;
	    match->marked_total = TRUE;
	}
	
	if (!list->next)
	    match->self += size;
	
	g_hash_table_insert (seen_symbols, symbol_copy (node->symbol), match);
	
	roots = match->children;
	parent = match;
    }
    
    g_hash_table_destroy (seen_symbols);
    
    for (list = nodes_to_unmark; list != NULL; list = list->next)
    {
	ProfileDescendantTreeNode *tree_node = list->data;
	
	tree_node->marked_non_recursive = 0;
    }
    
    for (list = nodes_to_unmark_recursive; list != NULL; list = list->next)
    {
	ProfileDescendantTreeNode *tree_node = list->data;
	
	tree_node->marked_total = FALSE;
    }
    
    g_list_free (nodes_to_unmark);
}

static void
add_leaf_to_tree (ProfileDescendantTree *tree, ProfileNode *leaf, ProfileNode *top)
{
    ProfileNode *node;
    GList *trace = NULL;
    
    for (node = leaf; node != top->parent; node = node->parent)
	trace = g_list_prepend (trace, node);
    
    add_trace_to_tree (tree->roots, trace, leaf->self);
    
    g_list_free (trace);
}

ProfileDescendantTree *
profile_func_create_descendant_tree (ProfileFunc           *func)
{
    ProfileDescendantTree *tree = g_new (ProfileDescendantTree, 1);
    ProfileNode *node;
    
    tree->roots = g_ptr_array_new ();
    
    for (node = func->node; node != NULL; node = node->next)
	if (node->toplevel)
	{
	    GList *leaves = NULL;
	    GList *list;
	    
	    node_list_leaves (node, &leaves);
	    
	    for (list = leaves; list != NULL; list = list->next)
		add_leaf_to_tree (tree, list->data, node);
	    
	    g_list_free (leaves);
	}
    
    return tree;
}

static void
profile_descendant_tree_node_free (ProfileDescendantTreeNode *node)
{
    int i;
    
    for (i = 0; i < node->children->len; ++i)
    {
	ProfileDescendantTreeNode *child = node->children->pdata[i];
	
	profile_descendant_tree_node_free (child);
    }
    
    g_ptr_array_free (node->children, TRUE);
    g_free (node);
}

void
profile_descendant_tree_free (ProfileDescendantTree *descendant_tree)
{
    int i;
    
    for (i = 0; i < descendant_tree->roots->len; ++i)
    {
	ProfileDescendantTreeNode *node = descendant_tree->roots->pdata[i];
	
	profile_descendant_tree_node_free (node);
    }
    g_ptr_array_free (descendant_tree->roots, TRUE);
    g_free (descendant_tree);
}

GPtrArray *
profile_func_create_caller_list     (ProfileFunc	     *func)
{
    GPtrArray *result = g_ptr_array_new ();
    GHashTable *callers_by_symbol = g_hash_table_new_full (
	symbol_hash, symbol_equal,
	(GDestroyNotify)symbol_free, NULL);
    GHashTable *marked_callers = g_hash_table_new (g_direct_hash, g_direct_equal);
    ProfileFunc *spontaneous = NULL;
    ProfileNode *node;
    
    for (node = func->node; node != NULL; node = node->next)
    {
	if (node->parent)
	{
	    if (!g_hash_table_lookup (callers_by_symbol, node->parent->symbol))
	    {
		ProfileFunc *caller = g_new (ProfileFunc, 1);
		
		caller->total = 0;
		caller->self = 0;
		caller->node = node->parent;
		
		g_hash_table_insert (
		    callers_by_symbol, symbol_copy (node->parent->symbol), caller);
		g_ptr_array_add (result, caller);
	    }
	}
	else
	{
	    if (!spontaneous)
	    {
		spontaneous = g_new (ProfileFunc, 1);
		spontaneous->total = 0;
		spontaneous->self = 0;
		spontaneous->node = NULL;
		g_ptr_array_add (result, spontaneous);
	    }
	}
    }
    
    for (node = func->node; node != NULL; node = node->next)
    {
	ProfileNode *top_caller_node;
	ProfileNode *top_callee_node;
	ProfileFunc *caller;
	ProfileNode *n;
	
	if (!node->parent)
	{
	    g_assert (spontaneous);
	    caller = spontaneous;
	}
	else
	    caller = g_hash_table_lookup (callers_by_symbol, node->parent->symbol);
	
	/* find topmost node/parent pair identical to this node/parent */
	top_caller_node = node->parent;
	top_callee_node = node;
	for (n = node->parent; n && n->parent != NULL; n = n->parent)
	{
	    if (n->symbol == node->symbol &&
		n->parent->symbol == top_caller_node->symbol)
	    {
		top_caller_node = n->parent;
		top_callee_node = n;
	    }
	}
	if (!g_hash_table_lookup (marked_callers, top_caller_node))
	{
	    caller->total += top_callee_node->total;
	    
	    g_hash_table_insert (marked_callers, top_caller_node, GINT_TO_POINTER (1));
	}
	
	if (node->self > 0)
	    caller->self += node->self;
    }
    
    g_hash_table_destroy (marked_callers);
    g_hash_table_destroy (callers_by_symbol);
    
    return result;
}

void
profile_caller_list_free	     (GPtrArray		     *caller_list)
{
    int i;
    
    for (i = 0; i < caller_list->len; ++i)
	g_free (caller_list->pdata[i]);
    
    g_ptr_array_free (caller_list, TRUE);
}

void
profile_write (Profile *profile, const gchar *outfile)
{
    /* FIXME */
}
