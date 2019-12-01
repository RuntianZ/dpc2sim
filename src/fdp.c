//
// Data Prefetching Championship Simulator 2
// Seth Pugsley, seth.h.pugsley@intel.com
//

/*
  
  This file describes a streaming prefetcher. Prefetches are issued after
  a spatial locality is detected, and a stream direction can be determined.

  Prefetches are issued into the L2 or LLC depending on L2 MSHR occupancy.

 */

#include <stdio.h>
#include <assert.h>
#include "../inc/prefetcher.h"

#define STREAM_DETECTOR_COUNT 64


// Parameters
#define T_INTERVAL 1024
#define PREFETCH_EVICT_SIZE 4096
#define A_HIGH 0.75
#define A_LOW 0.40
#define T_LAT 0.01
#define T_POL 0.005


int useful_bit[L2_SET_COUNT][L2_ASSOCIATIVITY];

// This mshr is only for prefetched lines
// mshr_addr stores addr >> 6
unsigned long long int mshr_addr[L2_MSHR_COUNT];
int mshr_valid[L2_MSHR_COUNT];
int late_bit[L2_MSHR_COUNT];
int prefetch_evict[PREFETCH_EVICT_SIZE];
// Values in interval
int used_cnt, prefetch_cnt, late_cnt, miss_cnt, miss_prefetch_cnt, evict_cnt;
// Values global
int used_total, prefetch_total, late_total, miss_total, miss_prefetch_total;

int prefetch_degree, stream_window, aggressive_level;


typedef struct stream_detector
{
  // which 4 KB page this detector is monitoring
  unsigned long long int page;
  
  // + or - direction for the stream
  int direction;

  // this must reach 2 before prefetches can begin
  int confidence;

  // cache line index within the page where prefetches will be issued
  int pf_index;
} stream_detector_t;

stream_detector_t detectors[STREAM_DETECTOR_COUNT];
int replacement_index;

void l2_prefetcher_initialize(int cpu_num)
{
  printf("Streaming Prefetcher\n");
  // you can inspect these knob values from your code to see which configuration you're runnig in
  printf("Knobs visible from prefetcher: %d %d %d\n", knob_scramble_loads, knob_small_llc, knob_low_bandwidth);

  int i, j;
  for(i=0; i<STREAM_DETECTOR_COUNT; i++)
    {
      detectors[i].page = 0;
      detectors[i].direction = 0;
      detectors[i].confidence = 0;
      detectors[i].pf_index = -1;
    }

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
  for (i = 0; i < L2_MSHR_COUNT; i++) {
	  late_bit[i] = 0;
	  mshr_valid[i] = 0;
  }
  for (i = 0; i < PREFETCH_EVICT_SIZE; i++)
	  prefetch_evict[i] = 0;
}

void l2_prefetcher_operate(int cpu_num, unsigned long long int addr, unsigned long long int ip, int cache_hit)
{
  // uncomment this line to see all the information available to make prefetch decisions
  //printf("(%lld 0x%llx 0x%llx %d %d %d) ", get_current_cycle(0), addr, ip, cache_hit, get_l2_read_queue_occupancy(0), get_l2_mshr_occupancy(0));


	// Virtual address
	unsigned long long int cl_address = addr >> 6;
	unsigned long long int a0 = cl_address & 0xfff;
	unsigned long long int a1 = (cl_address >> 12) & 0xfff;
	unsigned long long int virt_addr = a0 ^ a1;

	if (cache_hit) {
		// Check pref-bit for usefulness
		int s = l2_get_set(addr);
		assert(s < L2_SET_COUNT);
		int w = l2_get_way(0, addr, s);
		assert(w < L2_ASSOCIATIVITY);

		if (useful_bit[s][w]) {
			used_cnt++;
			useful_bit[s][w] = 0;
		}


	}
	else {
		miss_cnt++;

		// Check pref-bit for lateness
		int mshr_index = 0;
		while (mshr_index < L2_MSHR_COUNT) {
			if (mshr_valid[mshr_index] && mshr_addr[mshr_index] == cl_address)
				break;
			mshr_index++;
		}

		if (mshr_index < L2_MSHR_COUNT) {
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


	// Original stream prefetch
  
  
  unsigned long long int page = cl_address>>6;
  int page_offset = cl_address&63;

  // check for a detector hit
  int detector_index = -1;

  int i;
  for(i=0; i<STREAM_DETECTOR_COUNT; i++)
    {
      if(detectors[i].page == page)
	{
	  detector_index = i;
	  break;
	}
    }

  if(detector_index == -1)
    {
      // this is a new page that doesn't have a detector yet, so allocate one
      detector_index = replacement_index;
      replacement_index++;
      if(replacement_index >= STREAM_DETECTOR_COUNT)
	{
	  replacement_index = 0;
	}

      // reset the oldest page
      detectors[detector_index].page = page;
      detectors[detector_index].direction = 0;
      detectors[detector_index].confidence = 0;
      detectors[detector_index].pf_index = page_offset;
    }

  // train on the new access
  if(page_offset > detectors[detector_index].pf_index)
    {
      // accesses outside the stream_window do not train the detector
      if((page_offset-detectors[detector_index].pf_index) < stream_window)
	{
	  if(detectors[detector_index].direction == -1)
	    {
	      // previously-set direction was wrong
	      detectors[detector_index].confidence = 0;
	    }
	  else
	    {
	      detectors[detector_index].confidence++;
	    }

	  // set the direction to +1
	  detectors[detector_index].direction = 1;
	}
    }
  else if(page_offset < detectors[detector_index].pf_index)
    {
      // accesses outside the stream_window do not train the detector
      if((detectors[detector_index].pf_index-page_offset) < stream_window)
	{
          if(detectors[detector_index].direction == 1)
            {
	      // previously-set direction was wrong
	      detectors[detector_index].confidence = 0;
            }
          else
            {
	      detectors[detector_index].confidence++;
            }

	  // set the direction to -1
          detectors[detector_index].direction = -1;
        }
    }

  // prefetch if confidence is high enough
  if(detectors[detector_index].confidence >= 2)
    {
      int i;
      for(i=0; i<prefetch_degree; i++)
	{
	  detectors[detector_index].pf_index += detectors[detector_index].direction;

	  if((detectors[detector_index].pf_index < 0) || (detectors[detector_index].pf_index > 63))
	    {
	      // we've gone off the edge of a 4 KB page
	      break;
	    }

	  // perform prefetches
	  unsigned long long int pf_address = (page<<12)+((detectors[detector_index].pf_index)<<6);
	  
	  // check MSHR occupancy to decide whether to prefetch into the L2 or LLC
	  if(get_l2_mshr_occupancy(0) > 8)
	    {
	      // conservatively prefetch into the LLC, because MSHRs are scarce
	      l2_prefetch_line(0, addr, pf_address, FILL_LLC);
	    }
	  else
	    {
	      // MSHRs not too busy, so prefetch into L2
	      l2_prefetch_line(0, addr, pf_address, FILL_L2);
		  prefetch_total++;

		  // Add to MSHR
		  int mshr_index = 0;
		  while (mshr_index < L2_MSHR_COUNT) {
			  if (!mshr_valid[mshr_index])
				  break;
			  mshr_index++;
		  }
		  assert(mshr_index < L2_MSHR_COUNT);

		  mshr_valid[mshr_index] = 1;
		  mshr_addr[mshr_index] = pf_address >> 6;
		  late_bit[mshr_index] = 1;
	    }
	}
    }
}

void l2_cache_fill(int cpu_num, unsigned long long int addr, int set, int way, int prefetch, unsigned long long int evicted_addr)
{
	assert(set < L2_SET_COUNT);
	assert(way < L2_ASSOCIATIVITY);
	evict_cnt++;

	// Virtual address
	unsigned long long int cl_address = addr >> 6;
	unsigned long long int cl_evict_address = addr >> 6;
	unsigned long long int a0 = cl_evict_address & 0xfff;
	unsigned long long int a1 = (cl_evict_address >> 12) & 0xfff;
	unsigned long long int virt_addr = a0 ^ a1;

	if (prefetch) {

		// Remove from mshr
		int mshr_index = 0;
		while (mshr_index < L2_MSHR_COUNT) {
			if (mshr_valid[mshr_index] && mshr_addr[mshr_index] == cl_address)
				break;
			mshr_index++;
		}
		assert(mshr_index < L2_MSHR_COUNT);

		// Set pref-bit for usefulness
		useful_bit[set][way] = late_bit[mshr_index];

		mshr_valid[mshr_index] = 0;
		late_bit[mshr_index] = 0;

		// Add to evicted bit vector
		prefetch_evict[virt_addr] = 1;

		// Reset fetched bit vector
		unsigned long long int a0 = cl_address & 0xfff;
		unsigned long long int a1 = (cl_address >> 12) & 0xfff;
		unsigned long long int virt_new_addr = a0 ^ a1;
		prefetch_evict[virt_new_addr] = 0;
	}
	else {
		useful_bit[set][way] = 0;
		prefetch_evict[virt_addr] = 0;
	}


	// Check interval
	if (evict_cnt == T_INTERVAL) {
		evict_cnt = 0;

		used_total = (used_total >> 1) + (used_cnt >> 1);
		prefetch_total = (prefetch_total >> 1) + (prefetch_cnt >> 1);
		late_total = (late_total >> 1) + (late_cnt >> 1);
		miss_total = (miss_total >> 1) + (miss_cnt >> 1);
		miss_prefetch_total = (miss_prefetch_total >> 1) + (miss_prefetch_cnt >> 1);

		used_cnt = 0;
		prefetch_cnt = 0;
		late_cnt = 0;
		miss_cnt = 0;
		miss_prefetch_cnt = 0;

		float acc = (float)used_total / (float)prefetch_total;
		float lat = (float)late_total / (float)used_total;
		float pol = (float)miss_prefetch_total / (float)miss_total;

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

		switch (aggressive_level) {
		case 1:
			stream_window = 4;
			prefetch_degree = 1;
			break;
		case 2:
			stream_window = 8;
			prefetch_degree = 1;
			break;
		case 3:
			stream_window = 16;
			prefetch_degree = 2;
			break;
		case 4:
			stream_window = 32;
			prefetch_degree = 4;
			break;
		case 5:
			stream_window = 64;
			prefetch_degree = 4;
		}


	}

  // uncomment this line to see the information available to you when there is a cache fill event
  //printf("0x%llx %d %d %d 0x%llx\n", addr, set, way, prefetch, evicted_addr);
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
