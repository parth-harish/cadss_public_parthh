#include "cache.h"
#include "trace.h"

#include <getopt.h>
#include <stdlib.h>
#include <assert.h>

#include <coherence.h>
#include "stree.h"

typedef struct {
    bool valid;
    bool dirty;
    uint64_t tag;
    int LRU_counter;
    int RRPV;
    uint8_t* data;
} cache_line;

typedef struct {
    cache_line* lines;
} cache_set;

cache* self = NULL;
static cache_set* sets = NULL;
static int num_sets = 0;
static int lines_per_set = 0;
static int block_size = 0;
static int RRPV_bits = 0;
static bool use_RRIP = false;

int processorCount = 1;
int CADSS_VERBOSE = 0;
coher* coherComp = NULL;
int64_t* pendingTag = NULL;

typedef void (*memCallbackFunc)(int, int64_t);
memCallbackFunc* memCallback = NULL;

typedef struct _pendingRequest {
    int64_t tag;
    int64_t addr;
    int processorNum;
    void (*callback)(int, int64_t);
    struct _pendingRequest* next;
} pendingRequest;

pendingRequest* readyReq = NULL;
pendingRequest* pendReq = NULL;

void coherCallback(int type, int processorNum, int64_t addr);
void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));

static int get_set_index(uint64_t address) {
    return (address / block_size) % num_sets;
}

static uint64_t get_tag(uint64_t address) {
    return address / (block_size * num_sets);
}

static void update_LRU(cache_set* set, int way) {
    for (int i = 0; i < lines_per_set; i++) {
        if (set->lines[i].valid && set->lines[i].LRU_counter < set->lines[way].LRU_counter) {
            set->lines[i].LRU_counter++;
        }
    }
    set->lines[way].LRU_counter = 0;
}

static void update_RRIP(cache_set* set, int way, bool hit) {
    if (hit) {
        set->lines[way].RRPV = 0;
    } else {
        set->lines[way].RRPV = (1 << (RRPV_bits - 1)) - 1;
        for (int i = 0; i < lines_per_set; i++) {
            if (i != way && set->lines[i].valid && set->lines[i].RRPV < ((1 << RRPV_bits) - 1)) {
                set->lines[i].RRPV++;
            }
        }
    }
}

static int find_victim(cache_set* set) {
    if (use_RRIP) {
        while (1) {
            for (int i = 0; i < lines_per_set; i++) {
                if (set->lines[i].RRPV == (1 << RRPV_bits) - 1) {
                    return i;
                }
            }
            for (int i = 0; i < lines_per_set; i++) {
                if (set->lines[i].valid) {
                    set->lines[i].RRPV++;
                }
            }
        }
    } else {
        int max_lru = -1;
        int victim = -1;
        for (int i = 0; i < lines_per_set; i++) {
            if (!set->lines[i].valid) {
                return i;
            }
            if (set->lines[i].LRU_counter > max_lru) {
                max_lru = set->lines[i].LRU_counter;
                victim = i;
            }
        }
        return victim;
    }
}

cache* init(cache_sim_args* csa)
{
    int opt;
    int s = 0, E = 0, b = 0, R = 0;

    while ((opt = getopt(csa->arg_count, csa->arg_list, "E:s:b:i:R:")) != -1)
    {
        switch (opt)
        {
            case 'E':
                E = atoi(optarg);
                break;
            case 's':
                s = atoi(optarg);
                break;
            case 'b':
                b = atoi(optarg);
                break;
            case 'R':
                R = atoi(optarg);
                use_RRIP = true;
                break;
            case 'i':
                break;
        }
    }

    num_sets = 1 << s;
    lines_per_set = E;
    block_size = 1 << b;
    RRPV_bits = R;

    sets = calloc(num_sets, sizeof(cache_set));
    for (int i = 0; i < num_sets; i++) {
        sets[i].lines = calloc(lines_per_set, sizeof(cache_line));
        for (int j = 0; j < lines_per_set; j++) {
            sets[i].lines[j].data = calloc(block_size, sizeof(uint8_t));
        }
    }

    self = malloc(sizeof(cache));
    self->memoryRequest = memoryRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    coherComp = csa->coherComp;
    coherComp->registerCacheInterface(coherCallback);

    memCallback = calloc(processorCount, sizeof(memCallbackFunc));
    pendingTag = calloc(processorCount, sizeof(int));

    return self;
}

void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t))
{
    assert(op != NULL);
    assert(callback != NULL);

    uint64_t addr = (op->memAddress & ~(block_size - 1));
    int set_index = get_set_index(addr);
    uint64_t cache_tag = get_tag(addr);
    cache_set* set = &sets[set_index];

    bool hit = false;
    int way;
    for (way = 0; way < lines_per_set; way++) {
        if (set->lines[way].valid && set->lines[way].tag == cache_tag) {
            hit = true;
            break;
        }
    }

    if (hit) {
        if (use_RRIP) {
            update_RRIP(set, way, true);
        } else {
            update_LRU(set, way);
        }
    } else {
        int evict_way = find_victim(set);
        set->lines[evict_way].valid = true;
        set->lines[evict_way].tag = cache_tag;
        if (use_RRIP) {
            update_RRIP(set, evict_way, false);
        } else {
            update_LRU(set, evict_way);
        }
    }

    uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), addr, processorNum);

    pendingRequest* pr = malloc(sizeof(pendingRequest));
    pr->tag = tag;
    pr->addr = addr;
    pr->callback = callback;
    pr->processorNum = processorNum;

    if (perm == 1)
    {
        pr->next = readyReq;
        readyReq = pr;
    }
    else
    {
        pr->next = pendReq;
        pendReq = pr;
    }
}

void coherCallback(int type, int processorNum, int64_t addr)
{
    assert(pendReq != NULL);
    assert(processorNum < processorCount);

    if (type != DATA_RECV)
        return;

    if (pendReq->processorNum == processorNum && pendReq->addr == addr)
    {
        pendingRequest* pr = pendReq;
        pendReq = pendReq->next;
        pr->next = readyReq;
        readyReq = pr;
    }
    else
    {
        pendingRequest* prevReq = pendReq;
        pendingRequest* pr = pendReq->next;

        while (pr != NULL)
        {
            if (pr->processorNum == processorNum && pr->addr == addr)
            {
                prevReq->next = pr->next;
                pr->next = readyReq;
                readyReq = pr;
                break;
            }
            pr = pr->next;
            prevReq = prevReq->next;
        }

        assert(pr != NULL);
    }
}

int tick()
{
    coherComp->si.tick();

    pendingRequest* pr = readyReq;
    while (pr != NULL)
    {
        pendingRequest* t = pr;
        pr->callback(pr->processorNum, pr->tag);
        pr = pr->next;
        free(t);
    }
    readyReq = NULL;

    return 1;
}

int finish(int outFd)
{
    return 0;
}

int destroy(void)
{
    for (int i = 0; i < num_sets; i++) {
        for (int j = 0; j < lines_per_set; j++) {
            free(sets[i].lines[j].data);
        }
        free(sets[i].lines);
    }
    free(sets);

    free(memCallback);
    free(pendingTag);
    free(self);
    return 0;
}