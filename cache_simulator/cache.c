#include "cache.h"
#include "trace.h"

#include <getopt.h>
#include <stdlib.h>
#include <assert.h>

#include <coherence.h>
#include "stree.h"

typedef struct {
    bool valid;             // Indicates if the line contains valid data
    bool dirty;             // Indicates if the line has been modified (dirty)
    uint64_t tag;           // The tag part of the address
    int LRU_counter;        // Counter used for LRU (Least Recently Used) policy
    int RRPV;               // Counter for RRIP (Re-Reference Interval Prediction)
    uint8_t* data;          // Pointer to the data stored in this cache line
} cache_line;

typedef struct {
    cache_line* lines;      // Array of cache lines in the set
} cache_set;

cache* self = NULL;          // Cache object 
static cache_set* sets = NULL; // Pointer to the array of cache sets
static int num_sets = 0;     // Number of sets in the cache
static int lines_per_set = 0; // Number of lines per set (associativity)
static int block_size = 0;   // Size of a single cache block
static int RRPV_bits = 0;    // Number of bits used for RRIP
static bool use_RRIP = false; // Flag indicating whether RRIP is used

int processorCount = 1;      
coher* coherComp = NULL;     
int64_t* pendingTag = NULL;  

// Callback function for memory requests
typedef void (*memCallbackFunc)(int, int64_t);
memCallbackFunc* memCallback = NULL;

typedef struct _pendingRequest {
    int64_t tag;              
    int64_t addr;             
    int processorNum;         
    void (*callback)(int, int64_t); 
    struct _pendingRequest* next; 
} pendingRequest;

pendingRequest* readyReq = NULL;  // List of ready requests
pendingRequest* pendReq = NULL;   // List of pending requests

void coherCallback(int type, int processorNum, int64_t addr);
void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));

static int get_set_index(uint64_t address) {
    return (address / block_size) % num_sets; // Extract set index using block size and number of sets
}

static uint64_t get_tag(uint64_t address) {
    return address / (block_size * num_sets); // Extract the tag using block size and number of sets
}

static void update_LRU(cache_set* set, int way) {
    for (int i = 0; i < lines_per_set; i++) {
        if (set->lines[i].valid && set->lines[i].LRU_counter < set->lines[way].LRU_counter) {
            set->lines[i].LRU_counter++;  // Increment LRU counters for all valid lines
        }
    }
    set->lines[way].LRU_counter = 0; // Reset LRU counter for the accessed line
}

static void update_RRIP(cache_set* set, int way, bool hit) {
    if (hit) {
        set->lines[way].RRPV = 0;  // On hit, set RRPV to 0 (frequently accessed)
    } else {
        set->lines[way].RRPV = (1 << (RRPV_bits - 1)) - 1; // On miss, set RRPV to maximum value
        // Increment RRPV for other valid lines
        for (int i = 0; i < lines_per_set; i++) {
            if (i != way && set->lines[i].valid && set->lines[i].RRPV < ((1 << RRPV_bits) - 1)) {
                set->lines[i].RRPV++;
            }
        }
    }
}

static int find_victim(cache_set* set) {
    if (use_RRIP) {  // If RRIP is used
        while (1) {
            // Look for a line with the highest RRPV
            for (int i = 0; i < lines_per_set; i++) {
                if (set->lines[i].RRPV == (1 << RRPV_bits) - 1) {
                    return i;  // Return the first line with the maximum RRPV
                }
            }
            // If no line with max RRPV is found, increment RRPV of all valid lines
            for (int i = 0; i < lines_per_set; i++) {
                if (set->lines[i].valid) {
                    set->lines[i].RRPV++;
                }
            }
        }
    } else {  // If LRU is used
        int max_lru = -1;
        int victim = -1;
        // Find the line with the highest LRU counter (least recently used)
        for (int i = 0; i < lines_per_set; i++) {
            if (!set->lines[i].valid) {
                return i;  // Return the first invalid line
            }
            if (set->lines[i].LRU_counter > max_lru) {
                max_lru = set->lines[i].LRU_counter;
                victim = i;
            }
        }
        return victim;  // Return the victim line with the highest LRU counter
    }
}

cache* init(cache_sim_args* csa) {
    int opt;
    int s = 0, E = 0, b = 0, R = 0;

    while ((opt = getopt(csa->arg_count, csa->arg_list, "E:s:b:R:")) != -1) {
        switch (opt) {
            case 'E':  // Number of lines per set (associativity)
                E = atoi(optarg);
                break;
            case 's':  // Number of sets (log base 2)
                s = atoi(optarg);
                break;
            case 'b':  // Block size (log base 2)
                b = atoi(optarg);
                break;
            case 'R':  // Number of bits for RRIP
                R = atoi(optarg);
                use_RRIP = true;  // Enable RRIP policy
                break;
        }
    }

    num_sets = 1 << s;       // Number of sets in the cache
    lines_per_set = E;       // Lines per set (associativity)
    block_size = 1 << b;     // Size of each cache block
    RRPV_bits = R;           // Number of bits for RRIP

    // Allocate memory for cache sets and lines
    sets = calloc(num_sets, sizeof(cache_set));
    for (int i = 0; i < num_sets; i++) {
        sets[i].lines = calloc(lines_per_set, sizeof(cache_line));
        for (int j = 0; j < lines_per_set; j++) {
            sets[i].lines[j].data = calloc(block_size, sizeof(uint8_t));  // Allocate space for cache line data
        }
    }

    // Allocate and initialize the cache object
    self = malloc(sizeof(cache));
    self->memoryRequest = memoryRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    coherComp = csa->coherComp;
    coherComp->registerCacheInterface(coherCallback);

    memCallback = calloc(processorCount, sizeof(memCallbackFunc));
    pendingTag = calloc(processorCount, sizeof(int));

    return self;  // Return the initialized cache object
}

// Function to handle memory requests
void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t)) {
    assert(op != NULL);
    assert(callback != NULL);

    // Calculate the address aligned to the block size
    uint64_t addr = (op->memAddress & ~(block_size - 1));
    int set_index = get_set_index(addr);    // Get the cache set index
    uint64_t cache_tag = get_tag(addr);     // Get the cache tag
    cache_set* set = &sets[set_index];      // Get the cache set

    bool hit = false;
    int way;

    // Check for cache hit by comparing tags
    for (way = 0; way < lines_per_set; way++) {
        if (set->lines[way].valid && set->lines[way].tag == cache_tag) {
            hit = true;  // Cache hit found
            break;
        }
    }

    if (hit) {
        // Update the replacement policy on cache hit
        if (use_RRIP) {
            update_RRIP(set, way, true);   // Update RRIP on hit
        } else {
            update_LRU(set, way);          // Update LRU on hit
        }
    } else {
        // On a cache miss, find a victim to evict
        int evict_way = find_victim(set);
        set->lines[evict_way].valid = true;  // Mark the victim line as valid
        set->lines[evict_way].tag = cache_tag; // Update the tag for the new block
        if (use_RRIP) {
            update_RRIP(set, evict_way, false); // Update RRIP on miss
        } else {
            update_LRU(set, evict_way);        // Update LRU on miss
        }
    }

    uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), addr, processorNum);

    pendingRequest* pr = malloc(sizeof(pendingRequest));
    pr->tag = tag;
    pr->addr = addr;
    pr->callback = callback;
    pr->processorNum = processorNum;

    if (perm == 1) {  // If permission is granted, add to the ready request queue
        pr->next = readyReq;
        readyReq = pr;
    } else {  // Otherwise, add to the pending request queue
        pr->next = pendReq;
        pendReq = pr;
    }
}

void coherCallback(int type, int processorNum, int64_t addr) {
    assert(pendReq != NULL);   // Ensure there are pending requests
    assert(processorNum < processorCount);

    // Only process data receive events
    if (type != DATA_RECV)
        return;

    // Check if the address matches the pending request
    if (pendReq->processorNum == processorNum && pendReq->addr == addr) {
        pendingRequest* pr = pendReq;
        pendReq = pendReq->next;
        pr->next = readyReq;
        readyReq = pr;  // Move the request to the ready queue
    } else {
        pendingRequest* prevReq = pendReq;
        pendingRequest* pr = pendReq->next;

        // Search the pending request list for a matching request
        while (pr != NULL) {
            if (pr->processorNum == processorNum && pr->addr == addr) {
                prevReq->next = pr->next;
                pr->next = readyReq;
                readyReq = pr;  // Move the matching request to the ready queue
                break;
            }
            pr = pr->next;
            prevReq = prevReq->next;
        }

        assert(pr != NULL);  // Ensure the matching request was found
    }
}

int tick() {
    coherComp->si.tick();  

    // Process ready requests
    pendingRequest* pr = readyReq;
    while (pr != NULL) {
        pendingRequest* t = pr;
        pr->callback(pr->processorNum, pr->tag);  // Execute the callback for each request
        pr = pr->next;
        free(t);  
    }
    readyReq = NULL;  // Clear the ready queue

    return 1;
}

int finish(int outFd) {
    return 0;
}

int destroy(void) {
    // Free the memory allocated for cache sets and lines
    for (int i = 0; i < num_sets; i++) {
        for (int j = 0; j < lines_per_set; j++) {
            free(sets[i].lines[j].data);  // Free data for each cache line
        }
        free(sets[i].lines);  // Free the cache lines array for each set
    }
    free(sets);  // Free the cache sets array

    free(memCallback);  // Free the memory for memory callback functions
    free(pendingTag);   // Free the memory for pending tags
    free(self);         // Free the cache object itself
    return 0;
}