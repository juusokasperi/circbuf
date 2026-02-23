/*
   -----------------------------------------------------------------------------
   CIRCBUF.H v1.0.0
   -----------------------------------------------------------------------------
   Lock-free SPSC/MPMC circular buffer with slot sequence numbers.
   Memory-agnostic via Allocator pattern.
   
   Author:  Juuso Rinta
   Repo:    github.com/juusokasperi/circbuf
   License: MIT
   -----------------------------------------------------------------------------
   
   USAGE:
     Define CIRCBUF_IMPLEMENTATION in *one* .c file before including this header.
     
     #define CIRCBUF_IMPLEMENTATION
     #include "circbuf.h"

	 In all other files, just #include "circbuf.h" as per normal.
*/


#ifndef CIRCBUF_H
# define CIRCBUF_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include "allocator.h"

/* -- Types -- */

typedef struct {
	_Atomic uint32_t	seq;
	uint8_t				data[];
} Slot;

typedef struct {
	uint8_t							*slots;
	uint32_t						slot_size;
	uint32_t						stride;
	uint32_t						mask;
	Allocator						alloc;
	_Alignas(64) _Atomic uint32_t	head;
	_Alignas(64) _Atomic uint32_t	tail;
} CircularBuffer;

/* -- API -- */
int	cb_init(CircularBuffer *cb, Allocator alloc, uint32_t capacity, uint32_t slot_size);
void cb_free(CircularBuffer *cb);

void *cb_push_claim(CircularBuffer *cb, uint32_t *out_pos);
void cb_push_publish(CircularBuffer *cb, uint32_t pos);

void *cb_pop_claim(CircularBuffer *cb, uint32_t *out_pos);
void cb_pop_release(CircularBuffer *cb, uint32_t pos);

int cb_push(CircularBuffer *cb, const void *data, uint32_t size);
int cb_pop(CircularBuffer *cb, void *data, uint32_t size);

#define cb_init_malloc(cb, capacity, slot_size) \
	cb_init((cb), malloc_allocator(), (capacity), (slot_size))

#endif // CIRCBUF_H

#ifdef CIRCBUF_IMPLEMENTATION
#ifndef CIRCBUF_IMPLEMENTATION_GUARD
#define CIRCBUF_IMPLEMENTATION_GUARD

#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

static int	cb_is_power_of_two(uint32_t n) { return (n >= 2 && (n & (n - 1)) == 0); }
static Slot	*cb_slot(CircularBuffer *cb, uint32_t pos) { return ((Slot *)(cb->slots + (pos & cb->mask) * cb->stride)); }

int			cb_init(CircularBuffer *cb, Allocator alloc, uint32_t capacity, uint32_t slot_size)
{
	assert(cb != NULL && "cb is NULL");
	assert(alloc.alloc != NULL && "allocator must provide alloc function");
	assert(cb_is_power_of_two(capacity) && "capacity must be a power of two");
	assert(slot_size > 0 && "slot_size must be > 0");

	if (!cb || !alloc.alloc || !cb_is_power_of_two(capacity) || slot_size == 0)
		return (-EINVAL);

	cb->stride = ALIGN_UP(sizeof(Slot) + slot_size, _Alignof(Slot));
	cb->alloc = alloc;
	cb->slot_size = slot_size;
	cb->mask = capacity - 1;
	cb->slots = alloc.alloc(alloc.ctx, capacity * cb->stride, 0);

	if (!cb->slots)
		return (-ENOMEM);

	for (uint32_t i = 0; i < capacity; ++i)
		atomic_init(&((Slot *)(cb->slots + i * cb->stride))->seq, i);

	atomic_init(&cb->head, 0);
	atomic_init(&cb->tail, 0);
	return (0);
}

void cb_free(CircularBuffer *cb)
{
	if (!cb || !cb->slots)
		return;
	if (cb->alloc.free)
		cb->alloc.free(cb->alloc.ctx, cb->slots);
	cb->slots = NULL;
}

void *cb_push_claim(CircularBuffer *cb, uint32_t *out_pos)
{
	assert(cb != NULL && "cb is NULL");
	assert(out_pos != NULL && "out_pos is NULL");

	uint32_t pos = atomic_load_explicit(&cb->head, memory_order_relaxed);
	Slot *slot;
#ifdef CIRCBUF_MPMC
	for (;;)
	{
		slot = cb_slot(cb, pos);
		uint32_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
		int32_t	 diff = (int32_t)(seq - pos);

		if (diff == 0)
		{
			if (atomic_compare_exchange_weak_explicit(&cb->head, &pos, pos + 1,
					memory_order_relaxed, memory_order_relaxed))
				break;
			// else CAS failed, pos updated, retry
		}
		else if (diff < 0)
			return (NULL); // Full
		else
			pos = atomic_load_explicit(&cb->head, memory_order_relaxed); // Retry
	}
#else
	slot = cb_slot(cb, pos);
	uint32_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
	if (seq != pos)
		return (NULL); // Full
	atomic_store_explicit(&cb->head, pos + 1, memory_order_relaxed);
#endif
	*out_pos = pos;
	return (slot->data);
}

void cb_push_publish(CircularBuffer *cb, uint32_t pos)
{
	assert(cb != NULL && "cb is NULL");
	
	Slot *slot = cb_slot(cb, pos);
	atomic_store_explicit(&slot->seq, pos + 1, memory_order_release);
}

int	cb_push(CircularBuffer *cb, const void *data, uint32_t size)
{
	assert(cb != NULL && "cb is NULL");
	assert(data != NULL && "data is NULL");
	assert(size <= cb->slot_size && "size exceeds slot_size");
	if (!cb || !data || size > cb->slot_size)
		return (-EINVAL);

	uint32_t pos;
	void *slot_data = cb_push_claim(cb, &pos);
	if (!slot_data)
		return (-1); // Full

	memcpy(slot_data, data, size);
	cb_push_publish(cb, pos);

	return (0);
}

void *cb_pop_claim(CircularBuffer *cb, uint32_t *out_pos)
{
    assert(cb != NULL && "cb is NULL");
    assert(out_pos != NULL && "out_pos is NULL");
	
	uint32_t	pos = atomic_load_explicit(&cb->tail, memory_order_relaxed);
	Slot		*slot;
#ifdef CIRCBUF_MPMC
	for (;;)
	{
		slot = cb_slot(cb, pos);
		uint32_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
		int32_t  diff = (int32_t)(seq - (pos + 1));

		if (diff == 0)
		{
			if (atomic_compare_exchange_weak_explicit(&cb->tail, &pos, pos + 1,
					memory_order_relaxed, memory_order_relaxed))
				break;
		}
		else if (diff < 0)
			return (NULL); // Empty
		else
			pos = atomic_load_explicit(&cb->tail, memory_order_relaxed);
	}
#else
	slot = cb_slot(cb, pos);
	uint32_t seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
	if (seq != pos + 1)
		return (NULL); // Empty
	atomic_store_explicit(&cb->tail, pos + 1, memory_order_relaxed);
#endif
	*out_pos = pos;
	return (slot->data);
}

void cb_pop_release(CircularBuffer *cb, uint32_t pos)
{
	assert(cb != NULL && "cb is NULL");

	Slot *slot = cb_slot(cb, pos);
	atomic_store_explicit(&slot->seq, pos + cb->mask + 1, memory_order_release);
}

int cb_pop(CircularBuffer *cb, void *data, uint32_t size)
{
	assert(cb != NULL && "cb is NULL");
	assert(data != NULL && "data is NULL");
	assert(size <= cb->slot_size && "size exceeds slot_size");

	if (!cb || !data || size > cb->slot_size)
		return (-EINVAL);

	uint32_t	pos;
	void		*slot_data = cb_pop_claim(cb, &pos);

	if (!slot_data)
		return (-1); // Empty

	memcpy(data, slot_data, size);
	cb_pop_release(cb, pos);
	return (0);
}

#endif // CIRCBUF_IMPLEMENTATION_GUARD
#endif // CIRCBUF_IMPLEMENTATION
