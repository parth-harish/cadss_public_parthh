#include <branch.h>
#include <trace.h>

#include <getopt.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

// Structure representing the branch predictor
branch* self = NULL;

// Simulator parameters
int predictorSize = 0;    // Log2 of the predictor size
int bhrSize = 0;          // Size of the Branch History Register (ignored in this implementation)
int predictorModel = 0;   // Predictor model (only 2-bit predictor is implemented)
int processorCount = 1;   // Number of processors (currently unused)

// Predictor data structures
uint8_t* predictorTable = NULL; // Predictor table of 2-bit saturating counters

// Function declarations
uint64_t branchRequest(trace_op* op, int processorNum);
int tick();
int finish(int outFd);
int destroy(void);

// Initialize the branch predictor simulator
branch* init(branch_sim_args* csa)
{
    int op;

    // Parse command-line arguments
    while ((op = getopt(csa->arg_count, csa->arg_list, "p:s:b:g:")) != -1)
    {
        switch (op)
        {
            // Processor count (currently unused)
            case 'p':
                processorCount = atoi(optarg);
                break;

            // Predictor size (log2 of the number of entries)
            case 's':
                predictorSize = atoi(optarg);
                break;

            // BHR size (ignored in this implementation)
            case 'b':
                bhrSize = atoi(optarg);
                break;

            // Predictor model (only model 0 is implemented)
            case 'g':
                predictorModel = atoi(optarg);
                break;
        }
    }

    // Initialize predictor table
    int numEntries = 1 << predictorSize;
    predictorTable = malloc(numEntries * sizeof(uint8_t));
   
    for (int i = 0; i < numEntries; i++) {
        predictorTable[i] = 1; // Each counter defaults to state '01' (weakly not taken)
    }

    // Initialize branch struct
    self = malloc(sizeof(branch));

    self->branchRequest = branchRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    return self;
}

// Given a branch operation, return the predicted PC address
uint64_t branchRequest(trace_op* op, int processorNum)
{
    assert(op != NULL);

    uint64_t pc = op->pcAddress >> 3; // Ignore the 3 least significant bits

    // Calculate index using PC bits
    uint32_t index = pc & ((1 << predictorSize) - 1);

    // Get the counter value from the predictor table
    uint8_t counter = predictorTable[index];
    int prediction = (counter >> 1) & 1; // Use MSB of counter for prediction

    uint64_t predAddress;

    if (prediction) {
        // Predict taken: Use the target address from BTB or op->nextPCAddress
        predAddress = op->nextPCAddress;
    } else {
        // Predict not taken: PC + 4 as a simplified "not taken"
        predAddress = op->pcAddress + 4;
    }

    // Determine the actual outcome of the branch
    int actualOutcome = (op->nextPCAddress != op->pcAddress + 4);

    // Update the 2-bit counter based on the actual outcome
    if (actualOutcome) {
        // Branch was taken
        if (predictorTable[index] < 3) predictorTable[index]++;
    } else {
        // Branch was not taken
        if (predictorTable[index] > 0) predictorTable[index]--;
    }

    // Return the predicted address
    return predAddress;
}

int tick()
{
    return 1;
}

int finish(int outFd)
{
    return 0;
}

// Clean up and free allocated memory
int destroy(void)
{
    // Free any internally allocated memory here
    if (predictorTable != NULL) {
        free(predictorTable);
        predictorTable = NULL;
    }
    if (self != NULL) {
        free(self);
        self = NULL;
    }
    return 0;
}