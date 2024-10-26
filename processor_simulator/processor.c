#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "processor.h"
#include "trace.h"
#include "cache.h"
#include "branch.h"

trace_reader* tr = NULL;
cache* cs = NULL;
branch* bs = NULL;
processor* self = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;

int* pendingMem = NULL;
int* pendingBranch = NULL;
int64_t* memOpTag = NULL;

int fetch_rate = 1;           // Fetch rate (instructions per cycle)
int dispatch_mult = 1;        // Dispatch queue multiplier
int sched_mult = 1;           // Schedule queue multiplier
int fast_alus = 1;            // Number of fast ALUs
int long_alus = 1;            // Number of long ALUs
int num_cdbs = 1;             // Number of Common Data Buses (CDBs)


//
// init
//
//   Parse arguments and initialize the processor simulator components
//

processor* init(processor_sim_args* psa)
{
    int op;

    tr = psa->tr;
    cs = psa->cache_sim;
    bs = psa->branch_sim;

    // TODO - get argument list from assignment
    while ((op = getopt(psa->arg_count, psa->arg_list, "f:d:m:j:k:c:")) != -1)
    {
        switch (op)
        {
            // fetch rate
            case 'f':
                fetch_rate = atoi(optarg);
                break;

            // dispatch queue multiplier
            case 'd':
                dispatch_mult = atoi(optarg);
                break;

            // Schedule queue multiplier
            case 'm':
                sched_mult = atoi(optarg);
                break;

            // Number of fast ALUs
            case 'j':
                fast_alus = atoi(optarg);
                break;

            // Number of long ALUs
            case 'k':
                long_alus = atoi(optarg);
                break;

            // Number of CDBs
            case 'c':
                num_cdbs = atoi(optarg);
                break;
        }
    }

    pendingBranch = calloc(processorCount, sizeof(int));
    pendingMem = calloc(processorCount, sizeof(int));
    memOpTag = calloc(processorCount, sizeof(int64_t));

    self = calloc(1, sizeof(processor));
    return self;

    self->fetch_rate = fetch_rate;
    self->dispatch_mult = dispatch_mult;
    self->sched_mult = sched_mult;
    self->fast_alus = fast_alus;
    self->long_alus = long_alus;
    self->num_cdbs = num_cdbs;
}

const int64_t STALL_TIME = 100000;
int64_t tickCount = 0;
int64_t stallCount = -1;

int64_t makeTag(int procNum, int64_t baseTag)
{
    return ((int64_t)procNum) | (baseTag << 8);
}

void memOpCallback(int procNum, int64_t tag)
{
    int64_t baseTag = (tag >> 8);

    // Is the completed memop one that is pending?
    if (baseTag == memOpTag[procNum])
    {
        memOpTag[procNum]++;
        pendingMem[procNum] = 0;
        stallCount = tickCount + STALL_TIME;
    }
    else
    {
        printf("memopTag: %ld != tag %ld\n", memOpTag[procNum], tag);
    }
}

int tick(void)
{
    // if room in pipeline, request op from trace
    //   for the sample processor, it requests an op
    //   each tick until it reaches a branch or memory op
    //   then it blocks on that op

    trace_op* nextOp = NULL;

    // Pass along to the branch predictor and cache simulator that time ticked
    bs->si.tick();
    cs->si.tick();
    tickCount++;

    if (tickCount == stallCount)
    {
        printf(
            "Processor may be stalled.  Now at tick - %ld, last op at %ld\n",
            tickCount, tickCount - STALL_TIME);
        for (int i = 0; i < processorCount; i++)
        {
            if (pendingMem[i] == 1)
            {
                printf("Processor %d is waiting on memory\n", i);
            }
        }
    }

    int progress = 0;
    for (int i = 0; i < processorCount; i++)
    {
        if (pendingMem[i] == 1)
        {
            progress = 1;
            continue;
        }

        // In the full processor simulator, the branch is pending until
        //   it has executed.
        if (pendingBranch[i] > 0)
        {
            pendingBranch[i]--;
            progress = 1;
            continue;
        }

        // TODO: get and manage ops for each processor core
        nextOp = tr->getNextOp(i);

        if (nextOp == NULL)
            continue;

        progress = 1;

        switch (nextOp->op)
        {
            case MEM_LOAD:
            case MEM_STORE:
                pendingMem[i] = 1;
                cs->memoryRequest(nextOp, i, makeTag(i, memOpTag[i]),
                                  memOpCallback);
                break;

            case BRANCH:
                pendingBranch[i]
                    = (bs->branchRequest(nextOp, i) == nextOp->nextPCAddress)
                          ? 0
                          : 1;
                break;

            case ALU:
                // Simple ALU operation
                instructionsFired++;
                break;

            case ALU_LONG:
                // Long ALU operation
                instructionsFired++;
                break;
        }

        free(nextOp);
    }

    return progress;
}

int finish(int outFd)
{
    int c = cs->si.finish(outFd);
    int b = bs->si.finish(outFd);

    char buf[32];
    size_t charCount = snprintf(buf, 32, "Ticks - %ld\n", tickCount);

    (void)!write(outFd, buf, charCount + 1);

    if (b || c)
        return 1;
    return 0;
}

int destroy(void)
{
    int c = cs->si.destroy();
    int b = bs->si.destroy();

    free(pendingBranch);
    free(pendingMem);
    free(memOpTag);
    free(self);

    if (b || c)
        return 1;
    return 0;
    
}
