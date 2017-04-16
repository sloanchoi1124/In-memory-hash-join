#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

pthread_barrier_t mybarrier;

typedef struct {
	uint32_t key;
	uint32_t val;
} bucket_t;

typedef struct {
	pthread_t id;
	int thread;
	int threads;
	size_t inner_tuples;
	size_t outer_tuples;
	const uint32_t* inner_keys;
	const uint32_t* inner_vals;
	const uint32_t* outer_keys;
	const uint32_t* outer_vals;
	bucket_t*table;
	int8_t log_buckets;
	size_t buckets;
	uint64_t sum;
	uint32_t count;
} thread_info_t;

void* worker_thread(void *arg) {
	thread_info_t* info = (thread_info_t*)arg;
	assert(pthread_equal(pthread_self(), info->id));

	//copy info
	size_t thread = info->thread;
	size_t threads = info->threads;
	size_t inner_tuples = info->inner_tuples;
	const uint32_t* inner_keys = info->inner_keys;
	const uint32_t* inner_vals = info->inner_vals;
	bucket_t* table = info->table;
	int8_t log_buckets = info->log_buckets;
	size_t buckets = info->buckets;

	size_t outer_tuples = info->outer_tuples;
	const uint32_t* outer_keys = info->outer_keys;
	const uint32_t* outer_vals = info->outer_vals;

	//thread boundaries for inner table
	size_t inner_beg = (inner_tuples / threads) * (thread + 0);
	size_t inner_end = (inner_tuples / threads) * (thread + 1);
	if (thread + 1 == threads)
		inner_end = inner_tuples;

	//put inner tuples into the table
	size_t i, h;
	for (i = inner_beg; i != inner_end; ++i) {
		uint32_t key = inner_keys[i];
		uint32_t val = inner_vals[i];
		//calculate hash value
		h = (uint32_t) (key * 0x9e3779b1);
		h >>= 32 - log_buckets;
		while (!__sync_bool_compare_and_swap(&table[h].key, 0, key)) {
			h = (h + 1) & (buckets - 1);
		}
		table[h].val = val;
	}

	pthread_barrier_wait(&mybarrier);
	
	//thread boundaries for outer table
	size_t outer_beg = (outer_tuples / threads) * (thread + 0);
	size_t outer_end = (outer_tuples / threads) * (thread + 1);
	if (thread + 1 == threads)
		outer_end = outer_tuples;

	//do hash join here
	size_t o;
	uint32_t count = 0;
	uint64_t sum = 0;
	for (o = outer_beg; o != outer_end; ++o) {
		uint32_t key = outer_keys[o];
		h = (uint32_t) (key * 0x9e3779b1);
		h >>= 32 - log_buckets;

		uint32_t tab = table[h].key;
		while (tab != 0) {
			if (tab == key) {
				sum += table[h].val * (uint64_t)outer_vals[o];
				count += 1;
				break;
			}
			h = (h + 1) & (buckets - 1);
			tab = table[h].key;
		}
	}
	info->sum = sum;
	info->count = count;
	pthread_exit(NULL);
}
uint64_t q4112_run(const uint32_t* inner_keys, const uint32_t* inner_vals,
		   size_t inner_tuples, const uint32_t* outer_join_keys,
		   const uint32_t* outer_aggr_keys, const uint32_t* outer_vals,
		   size_t outer_tuples, int threads) {
	int t, max_threads = sysconf(_SC_NPROCESSORS_ONLN);
	assert(max_threads > 0 && threads > 0 && threads <= max_threads);
	//TODO: allocate space for the hash table
	int8_t log_buckets = 1;
	size_t buckets = 2;
	while (buckets * 0.67 < inner_tuples) {
		log_buckets += 1;
		buckets += buckets;
	}
	bucket_t* table = (bucket_t*) calloc(buckets, sizeof(bucket_t));
	assert(table != NULL);


	pthread_barrier_init(&mybarrier, NULL, threads);
	//TODO: create some threads;
	
	thread_info_t* info = (thread_info_t*) 
		malloc(threads * sizeof(thread_info_t));
	assert(info != NULL);
	for (t = 0; t != threads; ++t) {
		info[t].thread = t;
		info[t].threads = threads;
		info[t].inner_tuples = inner_tuples;
		info[t].inner_keys = inner_keys;
		info[t].inner_vals = inner_vals;
		info[t].table = table;
		info[t].log_buckets = log_buckets;
		info[t].buckets = buckets;
		info[t].threads = threads;
		info[t].outer_tuples = outer_tuples;
		info[t].outer_keys = outer_join_keys;
		info[t].outer_vals = outer_vals;

		pthread_create(&info[t].id, NULL, worker_thread, &info[t]);

	}
	uint64_t sum = 0;
	uint32_t count = 0;
	for (t = 0; t != threads; ++t) {
		pthread_join(info[t].id, NULL);
		sum += info[t].sum;
		count += info[t].count;
	}
	free(info);
	free(table);
	return sum / count;

}