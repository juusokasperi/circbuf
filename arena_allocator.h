/*
   -----------------------------------------------------------------------------
   ARENA_ALLOCATOR.H v1.0.0
   -----------------------------------------------------------------------------
   Helper header file for easy patching between my memarena.h and other memory-
   agnostic utils in my GitHub (e.g. vector.h, circbuf.h).
   
   Author:  Juuso Rinta
   Repo:    github.com/juusokasperi/memarena
   License: MIT
   -----------------------------------------------------------------------------
   
*/

#ifndef ARENA_ALLOCATOR_H
# define ARENA_ALLOCATOR_H

#include "allocator.h"
#include "memarena.h"

static void *arena_alloc_wrapper(void *ctx, size_t size, size_t align)
{
	if (align > 0)
		return (arena_alloc_aligned((Arena *)ctx, size, align));
	return (arena_alloc((Arena *)ctx, size));
}

static void *arena_realloc_wrapper(void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
	if (align > 0)
		return (arena_realloc_aligned((Arena *)ctx, ptr, old_size, new_size, align));
	return (arena_realloc((Arena *)ctx, ptr, old_size, new_size));
}

static Allocator arena_allocator(Arena *arena)
{
	Allocator a;

	a.alloc = arena_alloc_wrapper;
	a.realloc = arena_realloc_wrapper;
	a.free = NULL;
	a.ctx = arena;
	return (a);
}

#endif // ARENA_ALLOCATOR_H

