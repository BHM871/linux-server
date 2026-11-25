// pattern_workload.c
//
// Recurring, NON sequential page access pattern over a file backed mmap.
// Linux only (mmap, madvise, posix_fadvise). Build:
//   gcc -O2 -o pattern_workload pattern_workload.c
//
// Idea: the file is much bigger than what the kernel can guess with readahead.
// A "group" is a fixed list of pages (anchor + followers). Every round visits
// all the groups in a random order and, inside a group, always touches the
// pages in the same order. Between rounds the pages are evicted, so every
// round starts cold and the touches are major faults (disk I/O).
//
//   chain  groups of random scattered pages        (recurring, not sequential)
//   stride groups walking the file with a stride   (recurring, not sequential)
//   random groups are regenerated every round      (NEGATIVE control: no pattern)
//   seq    groups of consecutive pages             (kernel readahead control)
//
// What the collector can improve is the latency of the FOLLOWERS (k > 0): the
// anchor (k == 0) must fault by itself, it is what triggers the prefetch.
// Output: one CSV line per round and a final "# summary" line.

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

enum pattern { P_CHAIN, P_STRIDE, P_RANDOM, P_SEQ };

struct cfg {
	const char *file;
	size_t size_mb;
	enum pattern pattern;
	unsigned groups;
	unsigned members; // pages per group (anchor included)
	unsigned steady_rounds; // measured rounds
	unsigned warm_rounds; // minimum warmup rounds
	unsigned warm_seconds; // minimum warmup time
	unsigned think_us; // pause between two accesses (simulates the server work)
	int madv_random; // -R: MADV_RANDOM (turns the kernel fault readahead off)
	int evict_pageout; // -e pageout
	uint64_t seed;
};

// ---------------- rng (xorshift64*) ----------------

static uint64_t rng_state = 88172645463325252ULL;

static void rng_seed(uint64_t s)
{
	rng_state = s ? s : 88172645463325252ULL;

	for (int i = 0; i < 8; i++) {
		rng_state ^= rng_state >> 12;
		rng_state ^= rng_state << 25;
		rng_state ^= rng_state >> 27;
	}
}

static uint64_t rng_next(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;

	return rng_state * 2685821657736338717ULL;
}

static uint64_t rng_range(uint64_t n)
{
	return rng_next() % n;
}

// ---------------- helpers ----------------

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

// sorts v in place and returns the p-th percentile (0..100), 0 when empty

static double percentile(double *v, size_t n, double p)
{
	if (!n)
		return 0.0;

	qsort(v, n, sizeof(*v), cmp_double);
	size_t idx = (size_t)((p / 100.0) * (double)(n - 1) + 0.5);

	return v[idx];
}

static const char *pattern_name(enum pattern p)
{
	switch (p) {
	case P_CHAIN:
		return "chain";
	case P_STRIDE:
		return "stride";
	case P_RANDOM:
		return "random";
	default:
		return "seq";
	}
}

static int parse_pattern(const char *s, enum pattern *out)
{
	if (!strcmp(s, "chain"))
		*out = P_CHAIN;
	else if (!strcmp(s, "stride"))
		*out = P_STRIDE;
	else if (!strcmp(s, "random"))
		*out = P_RANDOM;
	else if (!strcmp(s, "seq"))
		*out = P_SEQ;
	else
		return -1;

	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [options]\n"
		"  -f FILE    data file (default ./pattern_data.bin, use a real disk, not tmpfs)\n"
		"  -s MB      file size in MB (default 512)\n"
		"  -p PATTERN chain|stride|random|seq (default chain)\n"
		"  -g N       number of groups (default 64)\n"
		"  -m N       pages per group, anchor included (default 8)\n"
		"  -r N       measured (steady) rounds (default 10)\n"
		"  -w N       minimum warmup rounds, the module learns here (default 5)\n"
		"  -T SEC     minimum warmup time in seconds (default 20)\n"
		"  -t US      pause between two accesses in us (default 500)\n"
		"             keep (members * US) below the module delta_ns (10 ms)\n"
		"  -R         madvise(MADV_RANDOM): disables the kernel fault readahead\n"
		"  -e MODE    evict between rounds: dontneed (default) | pageout\n"
		"  -S SEED    seed of the pattern (default 42)\n",
		prog);
}

// ---------------- data file ----------------

// creates (or reuses) the data file with real content, so it lives on disk
static int prepare_file(const struct cfg *c, size_t bytes)
{
	struct stat st;
	int fd = open(c->file, O_RDWR | O_CREAT, 0644);

	if (fd < 0) {
		perror("open");
		return -1;
	}

	if (fstat(fd, &st) < 0) {
		perror("fstat");
		close(fd);
		return -1;
	}

	if ((size_t)st.st_size != bytes) {
		long pg = sysconf(_SC_PAGESIZE);
		unsigned char *buf = malloc(pg);

		if (!buf || ftruncate(fd, 0) < 0) {
			perror("prepare");
			free(buf);
			close(fd);
			return -1;
		}

		fprintf(stderr, "# creating %s (%zu MB)...\n", c->file,
			c->size_mb);

		for (size_t off = 0; off < bytes; off += pg) {
			memset(buf, (int)((off / pg) & 0xff) | 1, pg);

			if (pwrite(fd, buf, pg, off) != pg) {
				perror("pwrite");
				free(buf);
				close(fd);
				return -1;
			}
		}
		free(buf);
	}

	fdatasync(fd);

	return fd;
}

// makes the next round start cold: nothing mapped, nothing in the page cache
static void evict(int fd, void *base, size_t bytes, int pageout)
{
	if (pageout)
		madvise(base, bytes, MADV_PAGEOUT);

	madvise(base, bytes, MADV_DONTNEED);
	fdatasync(fd);
	posix_fadvise(fd, 0, (off_t)bytes, POSIX_FADV_DONTNEED);
}

// ---------------- patterns ----------------

// fills pages[groups * members] with page indexes for this round
static void build_groups(const struct cfg *c, uint64_t npages, unsigned round,
			 uint64_t *pages, uint32_t *perm)
{
	size_t need = (size_t)c->groups * c->members;

	switch (c->pattern) {
	case P_CHAIN:
	case P_RANDOM: {
		// random pattern changes every round, chain stays the same
		rng_seed(c->seed +
			 (c->pattern == P_RANDOM ? round * 7919ULL : 0));

		for (uint64_t i = 0; i < npages; i++)
			perm[i] = (uint32_t)i;

		// partial Fisher-Yates: unique pages for the whole table

		for (size_t i = 0; i < need; i++) {
			uint64_t j = i + rng_range(npages - i);
			uint32_t t = perm[i];
			perm[i] = perm[j];
			perm[j] = t;
			pages[i] = perm[i];
		}

		break;
	}
	case P_STRIDE:
		rng_seed(c->seed);

		for (unsigned g = 0; g < c->groups; g++) {
			uint64_t start = rng_range(npages);
			uint64_t stride = 17 + rng_range(497);

			for (unsigned k = 0; k < c->members; k++)
				pages[(size_t)g * c->members + k] =
					(start + k * stride) % npages;
		}
		break;
	case P_SEQ:
		for (unsigned g = 0; g < c->groups; g++) {
			uint64_t start = (uint64_t)g * (npages / c->groups);

			for (unsigned k = 0; k < c->members; k++)
				pages[(size_t)g * c->members + k] =
					(start + k) % npages;
		}
		break;
	}
}

// ---------------- main ----------------

int main(int argc, char **argv)
{
	struct cfg c = {
		.file = "./pattern_data.bin",
		.size_mb = 512,
		.pattern = P_CHAIN,
		.groups = 64,
		.members = 8,
		.steady_rounds = 50,
		.warm_rounds = 5,
		.warm_seconds = 20,
		.think_us = 500,
		.seed = 42,
	};
	int opt;

	while ((opt = getopt(argc, argv, "f:s:p:g:m:r:w:T:t:Re:S:h")) != -1) {
		switch (opt) {
		case 'f':
			c.file = optarg;
			break;
		case 's':
			c.size_mb = strtoul(optarg, NULL, 10);
			break;
		case 'p':
			if (parse_pattern(optarg, &c.pattern)) {
				usage(argv[0]);
				return 2;
			}
			break;
		case 'g':
			c.groups = strtoul(optarg, NULL, 10);
			break;
		case 'm':
			c.members = strtoul(optarg, NULL, 10);
			break;
		case 'r':
			c.steady_rounds = strtoul(optarg, NULL, 10);
			break;
		case 'w':
			c.warm_rounds = strtoul(optarg, NULL, 10);
			break;
		case 'T':
			c.warm_seconds = strtoul(optarg, NULL, 10);
			break;
		case 't':
			c.think_us = strtoul(optarg, NULL, 10);
			break;
		case 'R':
			c.madv_random = 1;
			break;
		case 'e':
			c.evict_pageout = !strcmp(optarg, "pageout");
			break;
		case 'S':
			c.seed = strtoull(optarg, NULL, 10);
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	long pg = sysconf(_SC_PAGESIZE);

	size_t bytes = c.size_mb * 1024UL * 1024UL;
	uint64_t npages = bytes / pg;
	size_t need = (size_t)c.groups * c.members;

	if (!c.groups || c.members < 2 || !c.steady_rounds || need > npages) {
		fprintf(stderr,

			"invalid sizes: need groups*members (%zu) <= pages (%llu), members >= 2\n",

			need, (unsigned long long)npages);

		return 2;
	}

	int fd = prepare_file(&c, bytes);
	if (fd < 0)
		return 1;

	unsigned char *base = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);

	if (base == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	if (c.madv_random)
		madvise(base, bytes, MADV_RANDOM);

	uint64_t *pages = malloc(need * sizeof(*pages));
	uint32_t *perm = malloc(npages * sizeof(*perm));
	unsigned *order = malloc(c.groups * sizeof(*order));
	double *anchor_lat = malloc(c.groups * sizeof(double));
	double *follow_lat = malloc(need * sizeof(double));
	// every follower latency of the steady rounds, for the final percentiles
	size_t steady_cap =
		(size_t)c.steady_rounds * c.groups * (c.members - 1);
	double *steady_follow = malloc(steady_cap * sizeof(double));
	double *steady_anchor =
		malloc((size_t)c.steady_rounds * c.groups * sizeof(double));

	if (!pages || !perm || !order || !anchor_lat || !follow_lat ||
	    !steady_follow || !steady_anchor) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	size_t steady_nf = 0, steady_na = 0;
	double steady_ms = 0.0, steady_maj = 0.0, steady_min = 0.0;
	volatile unsigned long sink = 0;
	unsigned steady_done = 0;

	printf("round,phase,pattern,accesses,anchor_p50_us,anchor_p99_us,follow_p50_us,follow_p90_us,follow_p99_us,follow_max_us,round_ms,majflt,minflt\n");

	evict(fd, base, bytes, c.evict_pageout);

	uint64_t t_start = now_ns();

	for (unsigned round = 0; steady_done < c.steady_rounds; round++) {
		int warm = round < c.warm_rounds ||
			   (now_ns() - t_start) <
				   (uint64_t)c.warm_seconds * 1000000000ULL;

		build_groups(&c, npages, round, pages, perm);

		// random visiting order of the groups (Fisher-Yates)

		rng_seed(c.seed * 31 + round);

		for (unsigned g = 0; g < c.groups; g++)
			order[g] = g;

		for (unsigned g = c.groups - 1; g > 0; g--) {
			unsigned j = rng_range(g + 1);
			unsigned t = order[g];

			order[g] = order[j];
			order[j] = t;
		}

		struct rusage r0, r1;
		size_t nf = 0, na = 0;
		uint64_t round_start;

		getrusage(RUSAGE_SELF, &r0);

		round_start = now_ns();

		for (unsigned oi = 0; oi < c.groups; oi++) {
			unsigned g = order[oi];

			for (unsigned k = 0; k < c.members; k++) {
				uint64_t page =
					pages[(size_t)g * c.members + k];

				uint64_t t0 = now_ns();

				sink += base[page * pg];

				double us = (double)(now_ns() - t0) / 1000.0;

				if (k == 0)
					anchor_lat[na++] = us;
				else
					follow_lat[nf++] = us;

				if (c.think_us) {
					// tv_nsec must stay below 1e9
					struct timespec ts = {
						.tv_sec = c.think_us / 1000000,
						.tv_nsec = (long)(c.think_us %
								  1000000) *
							   1000L,
					};

					nanosleep(&ts, NULL);
				}
			}
		}

		double round_ms = (double)(now_ns() - round_start) / 1e6;

		getrusage(RUSAGE_SELF, &r1);

		double majflt = (double)(r1.ru_majflt - r0.ru_majflt);
		double minflt = (double)(r1.ru_minflt - r0.ru_minflt);

		if (!warm) {
			memcpy(steady_follow + steady_nf, follow_lat,
			       nf * sizeof(double));
			memcpy(steady_anchor + steady_na, anchor_lat,
			       na * sizeof(double));

			steady_nf += nf;
			steady_na += na;
			steady_ms += round_ms;
			steady_maj += majflt;
			steady_min += minflt;
			steady_done++;
		}

		printf("%u,%s,%s,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f\n",
		       round, warm ? "warm" : "steady", pattern_name(c.pattern),
		       na + nf, percentile(anchor_lat, na, 50),
		       percentile(anchor_lat, na, 99),
		       percentile(follow_lat, nf, 50),
		       percentile(follow_lat, nf, 90),
		       percentile(follow_lat, nf, 99),
		       percentile(follow_lat, nf, 100), round_ms, majflt,
		       minflt);
		fflush(stdout);

		evict(fd, base, bytes, c.evict_pageout);
	}

	printf("# summary pattern=%s steady_rounds=%u follow_p50_us=%.1f follow_p90_us=%.1f follow_p99_us=%.1f anchor_p50_us=%.1f anchor_p99_us=%.1f avg_round_ms=%.1f avg_majflt=%.1f avg_minflt=%.1f groups=%u members=%u think_us=%u madv_random=%d evict=%s\n",
	       pattern_name(c.pattern), steady_done,
	       percentile(steady_follow, steady_nf, 50),
	       percentile(steady_follow, steady_nf, 90),
	       percentile(steady_follow, steady_nf, 99),
	       percentile(steady_anchor, steady_na, 50),
	       percentile(steady_anchor, steady_na, 99),
	       steady_ms / steady_done, steady_maj / steady_done,
	       steady_min / steady_done, c.groups, c.members, c.think_us,
	       c.madv_random, c.evict_pageout ? "pageout" : "dontneed");

	(void)sink;
	munmap(base, bytes);
	close(fd);

	free(pages);
	free(perm);
	free(order);
	free(anchor_lat);
	free(follow_lat);
	free(steady_follow);
	free(steady_anchor);

	return 0;
}
