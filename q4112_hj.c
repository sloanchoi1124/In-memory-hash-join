#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

#define BIG_NUMBER 0x9e3779b1
typedef struct {
	uint32_t aggr_key;
	uint64_t sum;
	uint32_t count;

} aggr_bucket_t;

pthread_barrier_t inner_table_barrier;
pthread_barrier_t global_hash_barrier;
pthread_barrier_t global_table_creation;
pthread_barrier_t aggr_barrier;

aggr_bucket_t *global_table = NULL;
int8_t log_global_buckets = 0;
size_t global_buckets = 0;

typedef struct {
	uint32_t key;
	uint32_t val;
} bucket_t;

//TODO: pack more stuff in thread_info_t
typedef struct {
	pthread_t id;
	int thread;
	int threads;
	size_t inner_tuples;
	size_t outer_tuples;
	const uint32_t *inner_keys;
	const uint32_t *inner_vals;
	const uint32_t *outer_keys;
	const uint32_t *outer_vals;
	const uint32_t *outer_aggr_keys;
	bucket_t *table;
	uint32_t *bitmaps_multi;
	size_t partitions;
	int8_t log_partitions;
	size_t groups;
	int8_t log_buckets;
	size_t buckets;
	uint64_t sum;
	uint32_t count;
} thread_info_t;

uint32_t trailing_zero_count(uint32_t num)
{
    uint32_t i = num;
    uint32_t count = 0;
    while (i != 0) {
	if ((i & 1) == 1)
	    return count;
	else {
	    count++;
	    i = i >> 1;
	}
    }
    return count;

}

int8_t log_two(size_t input)
{
	int8_t result = 0;
	size_t x = input;
	while (x > 0) {
		if(x == 1)
			break;
		else {
			result ++;
			x /= 2;
		}
	}
	return result;
}
void *worker_thread(void *arg)
{
	thread_info_t *info = (thread_info_t *)arg;
	assert(pthread_equal(pthread_self(), info->id));

	/*copy info*/
	size_t thread = info->thread;
	size_t threads = info->threads;
	size_t inner_tuples = info->inner_tuples;
	const uint32_t *inner_keys = info->inner_keys;
	const uint32_t *inner_vals = info->inner_vals;
	bucket_t *table = info->table;
	int8_t log_buckets = info->log_buckets;
	size_t buckets = info->buckets;
	size_t outer_tuples = info->outer_tuples;
	const uint32_t *outer_keys = info->outer_keys;
	const uint32_t *outer_vals = info->outer_vals;
	const uint32_t *outer_aggr_keys = info->outer_aggr_keys;
	size_t partitions = info->partitions;
	const int8_t log_partitions = info->log_partitions;

	
	//thread boundaries for inner table
	size_t inner_beg = (inner_tuples / threads) * (thread + 0);
	size_t inner_end = (inner_tuples / threads) * (thread + 1);
	if (thread + 1 == threads)
		inner_end = inner_tuples;

	//build hash table
	size_t i, h, h_global;
	for (i = inner_beg; i != inner_end; ++i) {
		uint32_t key = inner_keys[i];
		uint32_t val = inner_vals[i];
		//calculate hash value
		h = (uint32_t) (key * BIG_NUMBER);
		h >>= 32 - log_buckets;
		while (!__sync_bool_compare_and_swap(&table[h].key, 0, key))
			h = (h + 1) & (buckets - 1);
		table[h].val = val;
	}

	//estimate unique groups
	pthread_barrier_wait(&inner_table_barrier);
	//thread boundaries for outer table
	size_t outer_beg = (outer_tuples / threads) * (thread + 0);
	size_t outer_end = (outer_tuples / threads) * (thread + 1);
	if (thread + 1 == threads)
		outer_end = outer_tuples;

	size_t j;

	uint32_t *bitmaps_multi_local = calloc(partitions, 4);
	for (j = outer_beg; j != outer_end; ++j) {
	    uint32_t h = (uint32_t)(outer_aggr_keys[j] * BIG_NUMBER);
	    size_t p = h & (partitions - 1);
	    h >>= log_partitions;
	    bitmaps_multi_local[p] |= h & (-h);
	}
	int bitmaps_multi_beg = partitions * thread;
	int bitmaps_multi_end = partitions * (thread + 1);
	for (j = bitmaps_multi_beg; j != bitmaps_multi_end; ++j)
	    info->bitmaps_multi[j] = bitmaps_multi_local[j % partitions];

	free (bitmaps_multi_local);

	pthread_barrier_wait(&global_hash_barrier);

	if (thread == 0) {
	    for (i = 0; i < partitions; ++i) {
		for (j = 1; j < threads; ++j){ 
		    info->bitmaps_multi[i] |= info->bitmaps_multi[i + j * partitions];
		}
	    }
	    int estimation = 0;
	    for (i = 0; i < partitions; ++i)
		estimation += ((size_t) 1) << trailing_zero_count(~info->bitmaps_multi[i]);
	    estimation /= 0.77351;
	    global_buckets = estimation / 0.67;
	    log_global_buckets = log_two(global_buckets) + 1;
	    global_buckets = 1 << log_global_buckets;
	    //printf("DEBUG ==== estimation_with_partition = %d\n", estimation); 
	    global_table = (aggr_bucket_t *) calloc(global_buckets, sizeof(aggr_bucket_t));
	    
	    //debug only
	    //printf("====DEBUG =====global_buckets %d\n", (int)global_buckets);
	    //printf("====DEBUG======log_global_buckets %d\n", (int)log_global_buckets);
	    
	    for (i = 0; i < global_buckets; ++i) {
		global_table[i].aggr_key = 0;
		global_table[i].sum = 0;
		global_table[i].count = 0;
	    }
	}

	// do join and aggregation!
	pthread_barrier_wait(&global_table_creation);
	size_t o = 0;
	uint32_t count = 0;
	uint64_t sum = 0;
	for (o = outer_beg; o != outer_end; ++o) {
		uint32_t key = outer_keys[o];
		h = (uint32_t) (key * BIG_NUMBER);
		h >>= 32 - log_buckets;

		uint32_t tab = table[h].key;
		while (tab != 0) {
			if (tab == key) {
			    	uint64_t extra = table[h].val * (uint64_t)outer_vals[o];
				
				//TODO: update aggregation table here!
				uint32_t aggr_key = outer_aggr_keys[o];
				//printf("DEBUG=== outer_aggr_key[%d]=%u\n", (int)o, aggr_key);
				h_global = (uint32_t)(aggr_key * BIG_NUMBER);
				h_global >>= 32 - log_global_buckets;


				//printf("h_global=%u\n", (uint32_t)h_global);
				
				if (global_table[h_global].aggr_key == aggr_key) {
				    //printf("===DEBUG=== line 204 jumps to increment_bucket label\n");
				    goto increment_bucket;
				}

				while (!__sync_bool_compare_and_swap(&global_table[h_global].aggr_key, 0, aggr_key)) {
				    if (global_table[h_global].aggr_key == aggr_key)
					goto increment_bucket;
				    h_global = (h_global + 1) & (global_buckets - 1);
				}

				//TODO: atomic-add count and sum
increment_bucket:
				__sync_fetch_and_add(&global_table[h_global].count, 1);
				__sync_fetch_and_add(&global_table[h_global].sum, extra);
				//printf("===DEBUG=== after incrementation global_table[%u].count = %u\n", (uint32_t)h_global, (uint32_t)global_table[h_global].count);
				//printf("===DEBUG=== after incrementation global_table[%u].sum = %lu\n", (uint32_t)h_global, (uint64_t)global_table[h_global].sum);
				break;
			}
			h = (h + 1) & (buckets - 1);
			tab = table[h].key;
		}
	}

	//TODO: create another barrier; wait until all threads finish joining the table
	//loop though buckets in global table;
	
	pthread_barrier_wait(&aggr_barrier);
	size_t aggr_beg = (global_buckets / threads) * (thread + 0);
	size_t aggr_end = (global_buckets / threads) * (thread + 1);
	if (thread + 1 == threads)
	    aggr_end = global_buckets;
	
	for (j = aggr_beg; j != aggr_end; ++j) {
	    if (global_table[j].aggr_key == 0 && global_table[j].count != 0)
		printf("===BUG_ON=== fatal mistake! global_table incorrectly intilized!\n");
	    if (global_table[j].count > 0 && global_table[j].aggr_key !=0) {
		sum += global_table[j].sum / global_table[j].count;
		count++;
	    }
	}
	//TODO: average the aggregate here!
	info->sum = sum;
	info->count = count;
	//printf("debug sum=%d count = %d\n", (int)sum, (int)count);
	
	pthread_exit(NULL);
}

uint64_t q4112_run(const uint32_t *inner_keys, const uint32_t *inner_vals,
		   size_t inner_tuples, const uint32_t *outer_join_keys,
		   const uint32_t *outer_aggr_keys, const uint32_t *outer_vals,
		   size_t outer_tuples, int threads)
{
	int t, max_threads = sysconf(_SC_NPROCESSORS_ONLN);
	assert(max_threads > 0 && threads > 0 && threads <= max_threads);
	/*allocate space for the hash table*/
	int8_t log_buckets = 1;
	size_t buckets = 2;
	while (buckets * 0.67 < inner_tuples) {
		log_buckets += 1;
		buckets += buckets;
	}
	bucket_t *table = (bucket_t *) calloc(buckets, sizeof(bucket_t));
	assert(table != NULL);

	
	//TODO: allocate bitmaps multiple version;
	const int8_t log_partitions = 12;
	size_t partitions = 1 << log_partitions;
	uint32_t *bitmaps_multi = (uint32_t *)calloc(threads * partitions, 4);
	assert(bitmaps_multi != NULL);

	pthread_barrier_init(&inner_table_barrier, NULL, threads);
	pthread_barrier_init(&global_hash_barrier, NULL, threads);
	pthread_barrier_init(&global_table_creation, NULL, threads);
	pthread_barrier_init(&aggr_barrier, NULL, threads);
	/*create worker threads;*/
	thread_info_t *info = (thread_info_t *)
		malloc(threads * sizeof(thread_info_t));
	assert(info != NULL);
	for (t = 0; t != threads; ++t) {
		info[t].outer_aggr_keys = outer_aggr_keys;
		info[t].bitmaps_multi = bitmaps_multi;
		info[t].partitions = partitions;
		info[t].log_partitions = log_partitions;
		info[t].groups = 0;
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
	/*aggregate result*/
	for (t = 0; t != threads; ++t) {
		pthread_join(info[t].id, NULL);
		sum += info[t].sum;
		count += info[t].count;
	}
	free(global_table);
	free(bitmaps_multi);
	free(info);
	free(table);
	return sum / count;

}
