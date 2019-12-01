//
// Data Prefetching Championship Simulator 2
// Seth Pugsley, seth.h.pugsley@intel.com
//

/*

  This file describes a prefetcher that resembles a simplified version of the
  Access Map Pattern Matching (AMPM) prefetcher, which won the first
  Data Prefetching Championship.  The original AMPM prefetcher tracked large
  regions of virtual address space to make prefetching decisions, but this
  version works only on smaller 4 KB physical pages.

 */

#include <stdio.h>
#include "../inc/prefetcher.h"

#define AMPM_PAGE_COUNT 64

 // Parameters
#define T_INTERVAL 512
#define PREFETCH_EVICT_SIZE 4096
#define A_HIGH 0.75
#define A_LOW 0.40
#define T_LAT 0.01
#define T_POL 0.005

// #define DEBUG


#define MSHR_SIZE 2048
int useful_bit[L2_SET_COUNT][L2_ASSOCIATIVITY];

// This mshr is only for prefetched lines
// mshr_addr stores addr >> 6
unsigned long long int mshr_addr[MSHR_SIZE];
int mshr_valid[MSHR_SIZE];
int late_bit[MSHR_SIZE];
int prefetch_evict[PREFETCH_EVICT_SIZE];
// Values in interval
int used_cnt, prefetch_cnt, late_cnt, miss_cnt, miss_prefetch_cnt, evict_cnt;
// Values global
float used_total, prefetch_total, late_total, miss_total, miss_prefetch_total;

int prefetch_degree, stream_window, aggressive_level;

typedef struct ampm_page
{
	// page address
	unsigned long long int page;

	// The access map itself.
	// Each element is set when the corresponding cache line is accessed.
	// The whole structure is analyzed to make prefetching decisions.
	// While this is coded as an integer array, it is used conceptually as a single 64-bit vector.
	int access_map[64];

	// This map represents cache lines in this page that have already been prefetched.
	// We will only prefetch lines that haven't already been either demand accessed or prefetched.
	int pf_map[64];

	// used for page replacement
	unsigned long long int lru;
} ampm_page_t;

ampm_page_t ampm_pages[AMPM_PAGE_COUNT];

void l2_prefetcher_initialize(int cpu_num)
{
	printf("AMPM Lite Prefetcher\n");
	// you can inspect these knob values from your code to see which configuration you're runnig in
	printf("Knobs visible from prefetcher: %d %d %d\n", knob_scramble_loads, knob_small_llc, knob_low_bandwidth);

	int i;
	for (i = 0; i < AMPM_PAGE_COUNT; i++)
	{
		ampm_pages[i].page = 0;
		ampm_pages[i].lru = 0;

		int j;
		for (j = 0; j < 64; j++)
		{
			ampm_pages[i].access_map[j] = 0;
			ampm_pages[i].pf_map[j] = 0;
		}
	}

	prefetch_degree = 2;
	aggressive_level = 3;


	replacement_index = 0;
	used_total = 0;
	prefetch_total = 0;
	late_total = 0;
	miss_total = 0;
	miss_prefetch_total = 0;

	used_cnt = 0;
	prefetch_cnt = 0;
	late_cnt = 0;
	miss_cnt = 0;
	miss_prefetch_cnt = 0;
	evict_cnt = 0;

	// Initial configuration
	stream_window = 16;
	prefetch_degree = 2;
	aggressive_level = 3;

	for (i = 0; i < L2_SET_COUNT; i++)
		for (j = 0; j < L2_ASSOCIATIVITY; j++)
			useful_bit[i][j] = 0;
	for (i = 0; i < MSHR_SIZE; i++) {
		late_bit[i] = 0;
		mshr_valid[i] = 0;
	}
	for (i = 0; i < PREFETCH_EVICT_SIZE; i++)
		prefetch_evict[i] = 0;
}

void l2_prefetcher_operate(int cpu_num, unsigned long long int addr, unsigned long long int ip, int cache_hit)
{
	// uncomment this line to see all the information available to make prefetch decisions
	//printf("(0x%llx 0x%llx %d %d %d) ", addr, ip, cache_hit, get_l2_read_queue_occupancy(0), get_l2_mshr_occupancy(0));

	unsigned long long int cl_address = addr >> 6;
	unsigned long long int page = cl_address >> 6;
	unsigned long long int page_offset = cl_address & 63;

	unsigned long long int a0 = cl_address & 0xfff;
	unsigned long long int a1 = (cl_address >> 12) & 0xfff;
	unsigned long long int virt_addr = a0 ^ a1;

	if (cache_hit) {
		// Check pref-bit for usefulness
		int s = l2_get_set(addr);
		assert(s < L2_SET_COUNT);
		int w = l2_get_way(0, addr, s);
		assert(w < L2_ASSOCIATIVITY && w >= 0);

		if (useful_bit[s][w]) {
			used_cnt++;
			useful_bit[s][w] = 0;
		}


	}
	else {
		miss_cnt++;

		// Check pref-bit for lateness
		int mshr_index = 0;
		while (mshr_index < MSHR_SIZE) {
			if (mshr_valid[mshr_index] && mshr_addr[mshr_index] == cl_address)
				break;
			mshr_index++;
		}

		if (mshr_index < MSHR_SIZE) {
			if (late_bit[mshr_index]) {
				late_cnt++;
				used_cnt++;
				late_bit[mshr_index] = 0;
			}
		}

		// Check for cache pollution
		if (prefetch_evict[virt_addr])
			miss_prefetch_cnt++;
	}


	// check to see if we have a page hit
	int page_index = -1;
	int i;
	for (i = 0; i < AMPM_PAGE_COUNT; i++)
	{
		if (ampm_pages[i].page == page)
		{
			page_index = i;
			break;
		}
	}

	if (page_index == -1)
	{
		// the page was not found, so we must replace an old page with this new page

		// find the oldest page
		int lru_index = 0;
		unsigned long long int lru_cycle = ampm_pages[lru_index].lru;
		int i;
		for (i = 0; i < AMPM_PAGE_COUNT; i++)
		{
			if (ampm_pages[i].lru < lru_cycle)
			{
				lru_index = i;
				lru_cycle = ampm_pages[lru_index].lru;
			}
		}
		page_index = lru_index;

		// reset the oldest page
		ampm_pages[page_index].page = page;
		for (i = 0; i < 64; i++)
		{
			ampm_pages[page_index].access_map[i] = 0;
			ampm_pages[page_index].pf_map[i] = 0;
		}
	}

	// update LRU
	ampm_pages[page_index].lru = get_current_cycle(0);

	// mark the access map
	ampm_pages[page_index].access_map[page_offset] = 1;

	// positive prefetching
	int count_prefetches = 0;
	for (i = 1; i <= 16; i++)
	{
		int check_index1 = page_offset - i;
		int check_index2 = page_offset - 2 * i;
		int pf_index = page_offset + i;

		if (check_index2 < 0)
		{
			break;
		}

		if (pf_index > 63)
		{
			break;
		}

		if (count_prefetches >= prefetch_degree)
		{
			break;
		}

		if (ampm_pages[page_index].access_map[pf_index] == 1)
		{
			// don't prefetch something that's already been demand accessed
			continue;
		}

		if (ampm_pages[page_index].pf_map[pf_index] == 1)
		{
			// don't prefetch something that's alrady been prefetched
			continue;
		}

		if ((ampm_pages[page_index].access_map[check_index1] == 1) && (ampm_pages[page_index].access_map[check_index2] == 1))
		{
			// we found the stride repeated twice, so issue a prefetch

			unsigned long long int pf_address = (page << 12) + (pf_index << 6);

			// check the MSHR occupancy to decide if we're going to prefetch to the L2 or LLC
			if (get_l2_mshr_occupancy(0) < 8)
			{
				l2_prefetch_line(0, addr, pf_address, FILL_L2);
			}
			else
			{
				l2_prefetch_line(0, addr, pf_address, FILL_LLC);
			}

			// mark the prefetched line so we don't prefetch it again
			ampm_pages[page_index].pf_map[pf_index] = 1;
			count_prefetches++;
		}
	}

	// negative prefetching
	count_prefetches = 0;
	for (i = 1; i <= 16; i++)
	{
		int check_index1 = page_offset + i;
		int check_index2 = page_offset + 2 * i;
		int pf_index = page_offset - i;

		if (check_index2 > 63)
		{
			break;
		}

		if (pf_index < 0)
		{
			break;
		}

		if (count_prefetches >= prefetch_degree)
		{
			break;
		}

		if (ampm_pages[page_index].access_map[pf_index] == 1)
		{
			// don't prefetch something that's already been demand accessed
			continue;
		}

		if (ampm_pages[page_index].pf_map[pf_index] == 1)
		{
			// don't prefetch something that's alrady been prefetched
			continue;
		}

		if ((ampm_pages[page_index].access_map[check_index1] == 1) && (ampm_pages[page_index].access_map[check_index2] == 1))
		{
			// we found the stride repeated twice, so issue a prefetch

			unsigned long long int pf_address = (page << 12) + (pf_index << 6);

			// check the MSHR occupancy to decide if we're going to prefetch to the L2 or LLC
			if (get_l2_mshr_occupancy(0) < 12)
			{
				l2_prefetch_line(0, addr, pf_address, FILL_L2);

				// Add to MSHR
				int mshr_index = 0;
				while (mshr_index < MSHR_SIZE) {
					if (mshr_valid[mshr_index] && mshr_addr[mshr_index] == cl_address)
						break;
					mshr_index++;
				}

				if (mshr_index == MSHR_SIZE) {
					mshr_index = 0;
					while (mshr_index < MSHR_SIZE) {
						if (!mshr_valid[mshr_index])
							break;
						mshr_index++;
					}
					assert(mshr_index < MSHR_SIZE);

					mshr_valid[mshr_index] = 1;
					mshr_addr[mshr_index] = pf_address >> 6;
					late_bit[mshr_index] = 1;
				}

#ifdef DEBUG
				printf("\n");


				for (mshr_index = 0; mshr_index < MSHR_SIZE; mshr_index++) {
					if (mshr_valid[mshr_index]) {
						printf("In MSHR: 0x%llx\n", mshr_addr[mshr_index] << 6);
					}
				}

				// printf("MSHR: %d\n", get_l2_mshr_occupancy(0));

				printf("{%lld 0x%llx 0x%llx %d %d %d}\n\n", get_current_cycle(0), pf_address, ip, cache_hit, get_l2_read_queue_occupancy(0), get_l2_mshr_occupancy(0));
#endif

			}
			else
			{
				l2_prefetch_line(0, addr, pf_address, FILL_LLC);
			}

			// mark the prefetched line so we don't prefetch it again
			ampm_pages[page_index].pf_map[pf_index] = 1;
			count_prefetches++;
		}
	}
}

void l2_cache_fill(int cpu_num, unsigned long long int addr, int set, int way, int prefetch, unsigned long long int evicted_addr)
{
	assert(set < L2_SET_COUNT);
	assert(way < L2_ASSOCIATIVITY);

	if (evicted_addr != 0)
		evict_cnt++;

	// Virtual address
	unsigned long long int cl_address = addr >> 6;
	unsigned long long int cl_evict_address = evicted_addr >> 6;
	unsigned long long int a0 = cl_evict_address & 0xfff;
	unsigned long long int a1 = (cl_evict_address >> 12) & 0xfff;
	unsigned long long int virt_addr = a0 ^ a1;

	// Remove from mshr
	int mshr_index = 0;
	while (mshr_index < MSHR_SIZE) {
		if (mshr_valid[mshr_index] && mshr_addr[mshr_index] == cl_address)
			break;
		mshr_index++;
	}

	if (mshr_index < MSHR_SIZE) {
		// Set pref-bit for usefulness
		useful_bit[set][way] = late_bit[mshr_index];

		mshr_valid[mshr_index] = 0;
		late_bit[mshr_index] = 0;
	}

	if (prefetch) {

		prefetch_cnt++;
		// Add to evicted bit vector
		if (evicted_addr != 0)
			prefetch_evict[virt_addr] = 1;
	}
	else {
		useful_bit[set][way] = 0;
		if (evicted_addr != 0)
			prefetch_evict[virt_addr] = 0;
	}

	// Reset fetched bit vector
	a0 = cl_address & 0xfff;
	a1 = (cl_address >> 12) & 0xfff;
	virt_addr = a0 ^ a1;
	prefetch_evict[virt_addr] = 0;


	// Check interval
	if (evict_cnt == T_INTERVAL) {
		evict_cnt = 0;

		printf("Count: %d %d %d %d %d\n", used_cnt, prefetch_cnt, late_cnt, miss_cnt, miss_prefetch_cnt);

		const float alpha = 0.5;

		used_total = alpha * used_total + (1 - alpha) * used_cnt;
		prefetch_total = alpha * prefetch_total + (1 - alpha) * prefetch_cnt;
		late_total = alpha * late_total + (1 - alpha) * late_cnt;
		miss_total = alpha * miss_total + (1 - alpha) * miss_cnt;
		miss_prefetch_total = alpha * miss_prefetch_total + (1 - alpha) * miss_prefetch_cnt;

		const float eps = 1e-3;
		if (used_total < eps)
			used_total = 0;
		if (prefetch_total < eps)
			prefetch_total = 0;
		if (late_total < eps)
			late_total = 0;
		if (miss_total < eps)
			miss_total = 0;
		if (miss_prefetch_total < eps)
			miss_prefetch_total = 0;

		used_cnt = 0;
		prefetch_cnt = 0;
		late_cnt = 0;
		miss_cnt = 0;
		miss_prefetch_cnt = 0;


		float acc = (prefetch_total == 0) ? 0 : (used_total / prefetch_total);
		float lat = (used_total == 0) ? 0 : (late_total / used_total);
		float pol = (miss_total == 0) ? 0 : (miss_prefetch_total / miss_total);

		printf("Metric: acc %f  lat %f  pol %f\n", acc, lat, pol);

		// acc_level: 0-Low, 1-Medium, 2-High
		// lat_level: 0-Not_late, 1-Late
		// pol_level: 0-Low, 1-High
		int acc_level, lat_level, pol_level;
		if (acc < A_LOW)
			acc_level = 0;
		else if (acc < A_HIGH)
			acc_level = 1;
		else
			acc_level = 2;
		lat_level = (lat < T_LAT) ? 0 : 1;
		pol_level = (pol < T_POL) ? 0 : 1;

		// Update Rule
		int update_rule;
		switch (acc_level) {
		case 0:
			if (lat_level) {
				update_rule = -1;
			}
			else {
				update_rule = (pol_level) ? -1 : 0;
			}
			break;
		case 1:
			if (lat_level) {
				update_rule = (pol_level) ? -1 : 1;
			}
			else {
				update_rule = (pol_level) ? -1 : 0;
			}
			break;
		case 2:
			if (lat_level) {
				update_rule = 1;
			}
			else {
				update_rule = (pol_level) ? -1 : 0;
			}
		}

		aggressive_level += update_rule;
		if (aggressive_level > 5)
			aggressive_level = 5;
		if (aggressive_level < 1)
			aggressive_level = 1;

		printf("Aggressive level: %d\n\n", aggressive_level);

		switch (aggressive_level) {
		case 1:
			prefetch_degree = 1;
			break;
		case 2:
			prefetch_degree = 1;
			break;
		case 3:
			prefetch_degree = 2;
			break;
		case 4:
			prefetch_degree = 4;
			break;
		case 5:
			prefetch_degree = 4;
		}


	}
#ifdef DEBUG
	// uncomment this line to see the information available to you when there is a cache fill event
	printf("0x%llx %d %d %d 0x%llx\n", addr, set, way, prefetch, evicted_addr);
#endif
}

void l2_prefetcher_heartbeat_stats(int cpu_num)
{
	printf("Prefetcher heartbeat stats\n");
}

void l2_prefetcher_warmup_stats(int cpu_num)
{
	printf("Prefetcher warmup complete stats\n\n");
}

void l2_prefetcher_final_stats(int cpu_num)
{
	printf("Prefetcher final stats\n");
}
