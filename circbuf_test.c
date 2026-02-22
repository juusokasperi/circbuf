#define CIRCBUF_IMPLEMENTATION
#define MEMARENA_IMPLEMENTATION
#include "circbuf.h"
#include "arena_allocator.h"

#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#define CAPACITY    1024
#define MSG_COUNT   10000000

typedef struct {
	uint32_t	seq;
	uint64_t	value;
} Message;

typedef struct {
	CircularBuffer	*cb;
	uint32_t		count;
} ThreadArgs;

static void *producer(void *arg)
{
	ThreadArgs		*args = (ThreadArgs *)arg;
	CircularBuffer	*cb = args->cb;

	for (uint32_t i = 0; i < args->count; ++i)
	{
		Message msg = { .seq = i, .value = (uint64_t)i * 31337 };
		while (cb_push(cb, &msg, sizeof(msg)) != 0)
			; /* spin until slot available */
	}
	return (NULL);
}

static void *consumer(void *arg)
{
	ThreadArgs		*args = (ThreadArgs *)arg;
	CircularBuffer	*cb = args->cb;
	Message			msg;
	uint32_t		errors = 0;

	for (uint32_t i = 0; i < args->count; ++i)
	{
		while (cb_pop(cb, &msg, sizeof(msg)) != 0)
			; /* spin until data available */

		if (msg.seq != i)
		{
			fprintf(stderr, "ERROR: expected seq %u, got %u\n", i, msg.seq);
			errors++;
		}
		if (msg.value != (uint64_t)i * 31337)
		{
			fprintf(stderr, "ERROR: expected value %" PRIu64 ", got %" PRIu64 "\n",
				(uint64_t)i * 31337, msg.value);
			errors++;
		}
	}
	return ((void *)(uintptr_t)errors);
}

int main(void)
{
	CircularBuffer	cb;
	pthread_t		prod_thread, cons_thread;
	void			*cons_ret;
	Arena			arena = arena_init(PROT_READ | PROT_WRITE);

	if (cb_init(&cb, arena_allocator(&arena), CAPACITY, sizeof(Message)) != 0)
	{
		fprintf(stderr, "cb_init failed\n");
		arena_free(&arena);
		return (1);
	}

	ThreadArgs args = { .cb = &cb, .count = MSG_COUNT };

	printf("SPSC stress test: %d messages, capacity %d\n", MSG_COUNT, CAPACITY);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	pthread_create(&prod_thread, NULL, producer, &args);
	pthread_create(&cons_thread, NULL, consumer, &args);

	pthread_join(prod_thread, NULL);
	pthread_join(cons_thread, &cons_ret);

	clock_gettime(CLOCK_MONOTONIC, &t1);

	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	double throughput = MSG_COUNT / elapsed / 1e6;

	printf("Time:		%.3f s\n", elapsed);
	printf("Throughput:	%.2f M msg/s\n", throughput);

	uint32_t errors = (uint32_t)(uintptr_t)cons_ret;

	if (errors == 0)
		printf("OK: all messages received correctly\n");
	else
		printf("FAILED: %u errors\n", errors);

	cb_free(&cb);
	arena_free(&arena);
	return (errors != 0);
}
