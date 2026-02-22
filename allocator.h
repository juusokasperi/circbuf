#ifndef ALLOCATOR_H
# define ALLOCATOR_H

#include <stddef.h>
#include <assert.h>
#include <stdlib.h>

/* -- Allocator -- */ 
typedef void *(*alloc_fn)  (void *ctx, size_t size, size_t align);
typedef void *(*realloc_fn)(void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align);
typedef void  (*free_fn)   (void *ctx, void *ptr);

typedef struct {
	alloc_fn	alloc;
	realloc_fn	realloc;
	free_fn		free;
	void		*ctx;
} Allocator;

/* -- Malloc allocator -- */
static void *malloc_alloc(void *ctx, size_t size, size_t align)
{
	(void)ctx;

	if (align > 0)
		assert(0 && "malloc does not support alignment");
	return (malloc(size));
}

static void *malloc_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
	(void)ctx;
	(void)old_size;
	(void)align;
	return (realloc(ptr, new_size));
}

static void malloc_free(void *ctx, void *ptr)
{
	(void)ctx;
	free(ptr);
}

static Allocator malloc_allocator(void)
{
	Allocator a;

	a.alloc = malloc_alloc;
	a.realloc = malloc_realloc;
	a.free = malloc_free;
	a.ctx = NULL;
	return (a);
}

#endif
