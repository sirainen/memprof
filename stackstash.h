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

#include <glib.h>

/*
 * StackStash: a data structure for storing a set of stack traces as a tree.
 */

#ifndef __STACK_STASH_H__
#define __STACK_STASH_H__

typedef struct _StackElement StackElement;
typedef struct _StackStash StackStash;

struct _StackElement
{
	/* Address from stack trace */
	void *address;

	/* Pointer to parent StackElement; NULL means that this
	 * element is the dummy root element of the tree, and
	 * thus should be ignored.
	 */
	void *parent;
	
	int n_children;

	StackElement **children;
};

#define STACK_ELEMENT_IS_ROOT(element) ((element)->parent == NULL)

StackStash *stack_stash_new   (void);
void        stack_stash_free  (StackStash *stash);

/* The first element in addresses is the top frame in the stack;
 * the returned StackElement points to the StackElement for that address.
 */
StackElement *stack_stash_store (StackStash *stash,
				 gpointer   *addresses,
				 int         n_addresses);

#endif /* __STACK_STASH_H__ */
