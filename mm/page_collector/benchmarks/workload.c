#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

const int SIZE = 100000;
int *array;

static void start_array(void)
{
	array = calloc(SIZE, sizeof(int));

	int n = 57;
	int m = n << 16;
	for (int i = 0; i < SIZE; i++) {
		n *= 4321;
		array[i] = n % m;
	}
}

static void run(void)
{
	int sum = 0;

	for (int i = 0; i < SIZE; i++) {
		sum *= array[i];
		sum += array[i];
		sum -= array[i];
	}

	for (int i = 1; i <= SIZE; i++) {
		sum *= array[SIZE % i];
		sum += array[SIZE % i];
		sum -= array[SIZE % i];
	}
}

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

	return (long long)ts.tv_sec * 1000000000ULL + (long long)ts.tv_nsec;
}

static double to_ms(long long ns)
{
	return ns / 1e6;
}

int main(void)
{
	int tests = 100;
	long long max = LLONG_MIN;
	long long media = 0;
	long long min = LLONG_MAX;
	long long total = 0;

	for (int i = 0; i < tests; i++) {
		start_array();

		long long start = now_ns();
		run();
		long long end = now_ns();

		long long actually = end - start;
		printf("tempo: %.3f ms\n", to_ms(actually));

		total += actually;
		max = max > actually ? max : actually;
		min = min < actually ? min : actually;

		free(array);
	}

	media = total / tests;

	printf("total: %.3f ms min: %.3f ms max: %.3f ms media: %.3f ms\n",
	       to_ms(total), to_ms(min), to_ms(max), to_ms(media));

	return 0;
}
