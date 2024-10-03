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
int bhrSize = 0;          // Size of the Branch History Register (BHR)
int predictorModel = 0;   // Predictor model (0: 2-bit, 2: GSELECT)
int processorCount = 1;   // Number of processors (currently unused)

// Predictor data structures
uint8_t* predictorTable = NULL; // Predictor table of 2-bit saturating counters
uint32_t bhr = 0;               // Branch History Register

// Function declarations
uint64_t branchRequest(trace_op* op, int processorNum);
int tick();
int finish(int outFd);
int destroy(void);

// Initialize the branch predictor simulator
branch* init(branch_sim_args* csa)
{
    int op;

    // TODO - get argument list from assignment
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

            // BHR size
            case 'b':
                bhrSize = atoi(optarg);
                break;

            // Predictor model
            case 'g':
                predictorModel = atoi(optarg);
                break;
        }
    }

    // Initialize predictor table
    int numEntries = 1 << predictorSize;
    predictorTable = malloc(numEntries * sizeof(uint8_t));
    if (predictorTable == NULL) {
        fprintf(stderr, "Failed to allocate predictor table\n");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < numEntries; i++) {
        predictorTable[i] = 1; // Each counter defaults to state '01' (weakly not taken)
    }

    // Initialize BHR
    bhr = 0; // Initialize the Branch History Register to zero

    // Initialize branch struct
    self = malloc(sizeof(branch));
    if (self == NULL) {
        fprintf(stderr, "Failed to allocate branch struct\n");
        exit(EXIT_FAILURE);
    }
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
    uint32_t index = 0;

    // Variables for indexing
    uint32_t pcIndexBits = pc & ((1 << predictorSize) - 1);
    uint32_t bhrMask = (1 << bhrSize) - 1;

    if (predictorModel == 0) {
        // 2-bit saturating counter predictor
        index = pcIndexBits;
    } else if (predictorModel == 2) {
        // GSELECT predictor
        // Ensure BHR size is not larger than predictor size
        if (bhrSize > predictorSize) {
            fprintf(stderr, "BHR size cannot be larger than predictor size in GSELECT\n");
            exit(EXIT_FAILURE);
        }

        // Extract BHR bits and PC bits for indexing
        uint32_t bhrBits = bhr & bhrMask;
        uint32_t pcBits = pcIndexBits & ((1 << (predictorSize - bhrSize)) - 1);

        // Concatenate PC bits and BHR bits to form the index
        index = (pcBits << bhrSize) | bhrBits;
    }

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

    // Update BHR for GSELECT predictor
    if (predictorModel == 2 && bhrSize > 0) {
        // Shift in the actual outcome into the BHR
        bhr = ((bhr << 1) | actualOutcome) & bhrMask;
    }

    // Return the predicted address
    return predAddress;
}

// Called every tick (not used in this simulator)
int tick()
{
    // For this simulator, tick doesn't perform any action.
    return 1;
}

// Finalize the simulation (no statistics printed)
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