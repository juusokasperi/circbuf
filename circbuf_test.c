#include <stdatomic.h>
#define CIRCBUF_IMPLEMENTATION
#define MEMARENA_IMPLEMENTATION
// #define CIRCBUF_MPMC  // Uncomment this to test MPMC, or pass via compiler flags
#include "circbuf.h"
#include "arena_allocator.h"

#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#define CAPACITY    1024
#define MSG_COUNT   10000000 // Ensure this is divisible by thread counts

#ifdef CIRCBUF_MPMC
# define NUM_PRODUCERS 4
# define NUM_CONSUMERS 4
#else
# define NUM_PRODUCERS 1
# define NUM_CONSUMERS 1
#endif

typedef struct {
	uint32_t	seq;
	uint64_t	value;
} Message;

typedef struct {
	CircularBuffer	*cb;
	uint32_t		start_seq;
	uint32_t		count;
} ProdArgs;

typedef struct {
	CircularBuffer	*cb;
	uint32_t		count;
	_Atomic uint8_t	*received_tracker;
} ConsArgs;

static void *producer(void *arg)
{
	ProdArgs		*args = (ProdArgs *)arg;
	CircularBuffer	*cb = args->cb;

	for (uint32_t i = 0; i < args->count; ++i)
	{
		uint32_t seq = args->start_seq + i;
		Message msg = { .seq = seq, .value = (uint64_t)seq * 31337 };
		while (cb_push(cb, &msg, sizeof(msg)) != 0)
			; /* spin until slot available */
	}
	return (NULL);
}

static void *consumer(void *arg)
{
	ConsArgs		*args = (ConsArgs *)arg;
	CircularBuffer	*cb = args->cb;
	Message			msg;
	uint32_t		errors = 0;

	for (uint32_t i = 0; i < args->count; ++i)
	{
		while (cb_pop(cb, &msg, sizeof(msg)) != 0)
			; /* spin until data available */

		if (msg.value != (uint64_t)msg.seq * 31337)
		{
			fprintf(stderr, "ERROR: expected seq %u (got %" PRIu64 ")\n", msg.seq, msg.value);
			errors++;
		}
		else
		{
			uint8_t old_val = atomic_fetch_add_explicit(&args->received_tracker[msg.seq], 1, memory_order_relaxed);
			if (old_val != 0)
			{
				fprintf(stderr, "ERROR: duplicate message received: seq %u\n", msg.seq);
				errors++;
			}
		}
	}
	return ((void *)(uintptr_t)errors);
}

int main(void)
{
	CircularBuffer	cb;
	pthread_t		prod_threads[NUM_PRODUCERS];
	pthread_t		cons_threads[NUM_CONSUMERS];
	ProdArgs		prod_args[NUM_PRODUCERS];
	ConsArgs		cons_args[NUM_CONSUMERS];

	Arena			arena = arena_init(PROT_READ | PROT_WRITE);

	if (cb_init(&cb, arena_allocator(&arena), CAPACITY, sizeof(Message)) != 0)
	{
		fprintf(stderr, "cb_init failed\n");
		arena_free(&arena);
		return (1);
	}

	_Atomic uint8_t *received_tracker = arena_alloc(&arena, MSG_COUNT * sizeof(_Atomic uint8_t));
	if (!received_tracker)
	{
		fprintf(stderr, "failed to allocate tracker\n");
		arena_free(&arena);
		return (1);
	}

	printf("Mode:          %s\n", NUM_PRODUCERS > 1 ? "MPMC" : "SPSC");
	printf("Threads:       %d Producers, %d Consumers\n", NUM_PRODUCERS, NUM_CONSUMERS);
	printf("Messages:      %d total, capacity %d\n", MSG_COUNT, CAPACITY);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	uint32_t	msg_per_cons = MSG_COUNT / NUM_CONSUMERS;
	for (int i = 0; i < NUM_CONSUMERS; ++i)
	{
		cons_args[i] = (ConsArgs){ .cb = &cb, .count = msg_per_cons, .received_tracker = received_tracker };
		pthread_create(&cons_threads[i], NULL, consumer, &cons_args[i]);
	}

	uint32_t	msg_per_prod = MSG_COUNT / NUM_PRODUCERS;
	for (int i = 0; i < NUM_PRODUCERS; ++i)
	{
		prod_args[i] = (ProdArgs){ .cb = &cb, .start_seq = i * msg_per_prod, .count = msg_per_prod };
		pthread_create(&prod_threads[i], NULL, producer, &prod_args[i]);
	}

	for (int i = 0; i < NUM_PRODUCERS; ++i)
		pthread_join(prod_threads[i], NULL);
	
	uint32_t total_errors = 0;
	for (int i = 0; i < NUM_CONSUMERS; ++i)
	{
		void *ret;
		pthread_join(cons_threads[i], &ret);
		total_errors += (uint32_t)(uintptr_t)ret;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);

	for (uint32_t i = 0; i < MSG_COUNT; ++i)
	{
		if (atomic_load_explicit(&received_tracker[i], memory_order_relaxed) != 1)
		{
			if (total_errors == 0)
				fprintf(stderr, "ERROR: Missing message seq %u\n", i);
			total_errors++;
			if (total_errors > 10)
				break;
		}
	}

	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	double throughput = MSG_COUNT / elapsed / 1e6;

	printf("Time:		%.3f s\n", elapsed);
	printf("Throughput:	%.2f M msg/s\n", throughput);

	if (total_errors == 0)
		printf("OK: all messages received correctly\n");
	else
		printf("FAILED: %u errors\n", total_errors);

	cb_free(&cb);
	arena_free(&arena);
	return (total_errors != 0);
}
