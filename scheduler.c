#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>



#define MAX_DOCKS 30
#define MAX_CRANES 25
#define MAX_SOLVERS 8
#define MAX_CRANE_CAPACITY 30
#define MAX_NEWSHIP_REQUESTS 100
#define MAX_CARGO_COUNT 200
#define MAX_AUTH_STRING_LEN 100
#define MAX_GUESS_THREADS 8
#define SOLVER_MTYPE_SET_DOCK 1
#define SOLVER_MTYPE_GUESS 2
#define SOLVER_MTYPE_RESPONSE 3
#define INITIAL_QUEUE_CAPACITY 100
#define QUEUE_GROWTH_FACTOR 2



// Structure for dock information
typedef struct {
    int dockId;
    int category;
    int cranes[MAX_CRANES];  // Crane capacities
    
    // Operational State (dynamic)
    int isOccupied;
    int currentShipId;
    int direction;           // IN(1)/OUT(-1)
    
    // Cargo Management
    int remainingCargo[MAX_CARGO_COUNT];
    int cargoCount;
    int lastCargoTimestep;   // Last cargo operation time
    bool cargoOp ;
    
    // Timing Rules
    int lastdockedTimestep;
    int lastUndockedTimestep;  // For cooldown period
    bool craneUsed[MAX_CRANES]; // Track crane usage
} Dock;


// Structure for message queue communication
typedef struct MessageStruct {
    long mtype;
    int timestep;
    int shipId;
    int direction;
    int dockId;
    int cargoId;
    int isFinished;
    union {
        int numShipRequests;
        int craneId;
    };
} MessageStruct;

// Structure for ship requests
typedef struct ShipRequest {
    int shipId;
    int timestep;
    int category;
    int direction;
    int emergency;
    int waitingTime;
    int numCargo;
    int cargo[MAX_CARGO_COUNT];
} ShipRequest;

// Shared memory structure
typedef struct MainSharedMemory {
    char authStrings[MAX_DOCKS][MAX_AUTH_STRING_LEN];
    ShipRequest newShipRequests[MAX_NEWSHIP_REQUESTS];
} MainSharedMemory;

typedef struct SolverRequest {
    long mtype;
    int dockId;
    char authStringGuess[MAX_AUTH_STRING_LEN];
} SolverRequest;

typedef struct SolverResponse {
    long mtype;
    int guessIsCorrect;
} SolverResponse;

typedef struct {
    ShipRequest** heap;
    int size;
    int capacity;
} PriorityQueue;


int compareDocks(const void *a, const void *b) {
    const Dock *dockA = (const Dock *)a;
    const Dock *dockB = (const Dock *)b;
    return (dockA->category - dockB->category); // Ascending order
}

// Function to create a priority queue

PriorityQueue* createPriorityQueue(int capacity) {
    PriorityQueue* pq = (PriorityQueue*)malloc(sizeof(PriorityQueue));
    pq->heap = (ShipRequest**)malloc(INITIAL_QUEUE_CAPACITY * sizeof(ShipRequest*));
    pq->size = 0;
    pq->capacity = INITIAL_QUEUE_CAPACITY;
    return pq;
}

void ensureCapacity(PriorityQueue* pq) {
    if (pq->size >= pq->capacity) {
        int newCapacity = pq->capacity * QUEUE_GROWTH_FACTOR;
        ShipRequest** newHeap = (ShipRequest**)realloc(pq->heap, newCapacity * sizeof(ShipRequest*));
        
        if (!newHeap) {
            perror("Failed to expand priority queue");
            exit(EXIT_FAILURE);
        }
        
        pq->heap = newHeap;
        pq->capacity = newCapacity;
        printf("Resized priority queue to capacity %d\n", newCapacity);
    }
}

int compareShips(ShipRequest* a, ShipRequest* b) {
    // Separate incoming and outgoing ships
    int a_incoming = (a->direction == 1);
    int b_incoming = (b->direction == 1);

    // Case 1: One is incoming, the other is outgoing
    if (a_incoming != b_incoming) {
        return b_incoming - a_incoming; // Incoming ships come first
    }

    // Both are incoming ships
    if (a_incoming) {
        // Emergency ships have priority
        if (a->emergency != b->emergency) {
            return b->emergency - a->emergency; // Emergency first
        }
        
        // Both regular: prioritize by earliest expiration time
        int a_expiry = a->timestep + a->waitingTime;
        int b_expiry = b->timestep + b->waitingTime;
        return a_expiry - b_expiry; // Lower expiration time comes first
    }
    // Both are outgoing ships
    else {
        // Outgoing ships are prioritized by arrival time (FIFO)
        return a->timestep - b->timestep;
    }
}




void heapifyUp(PriorityQueue* pq, int index) {
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (compareShips(pq->heap[index], pq->heap[parent]) < 0) {
            ShipRequest* temp = pq->heap[index];
            pq->heap[index] = pq->heap[parent];
            pq->heap[parent] = temp;
            index = parent;
        } else {
            break;
        }
    }
}


void push(PriorityQueue* pq, ShipRequest* ship) {
    ensureCapacity(pq);
    pq->heap[pq->size] = ship;
    heapifyUp(pq, pq->size);
    pq->size++;
}



ShipRequest* pop(PriorityQueue* pq) {
    if (pq->size == 0) return NULL;
    ShipRequest* result = pq->heap[0];
    pq->heap[0] = pq->heap[pq->size - 1];
    pq->size--;
    
    int index = 0;
    while (1) {
        int left = 2 * index + 1;
        int right = 2 * index + 2;
        int smallest = index;
        
        if (left < pq->size && compareShips(pq->heap[left], pq->heap[smallest]) < 0)
            smallest = left;
        if (right < pq->size && compareShips(pq->heap[right], pq->heap[smallest]) < 0)
            smallest = right;
        
        if (smallest != index) {
            ShipRequest* temp = pq->heap[index];
            pq->heap[index] = pq->heap[smallest];
            pq->heap[smallest] = temp;
            index = smallest;
        } else {
            break;
        }
    }
    return result;
}

// Remainder Queue Implementation
typedef struct {
    ShipRequest* requests[MAX_NEWSHIP_REQUESTS];
    int front;
    int rear;
    int size;  // Track current queue size
} RemainderQueue;

void initRemainderQueue(RemainderQueue* rq) {
    rq->front = 0;
    rq->rear = -1;
    rq->size = 0;
}

void addToRemainder(RemainderQueue* rq, ShipRequest* ship) {
    if (rq->size < 0 || rq->size > MAX_NEWSHIP_REQUESTS) {
        fprintf(stderr, "Queue size corrupted! size = %d\n", rq->size);
        return;
    }

    if (rq->size >= MAX_NEWSHIP_REQUESTS) {
        printf("Remainder queue full! Ship %d not added\n", ship->shipId);
        free(ship);  // Critical: Prevent memory leak
        return;
    }
    
    rq->rear = (rq->rear + 1) % MAX_NEWSHIP_REQUESTS;
    rq->requests[rq->rear] = ship;
    rq->size++;
}

ShipRequest* removeFromRemainder(RemainderQueue* rq) {
    if (rq->size <= 0 || rq->size > MAX_NEWSHIP_REQUESTS) {
        fprintf(stderr, "Queue size invalid during removal: %d\n", rq->size);
        return NULL;
    }
    
    ShipRequest* ship = rq->requests[rq->front];
    rq->front = (rq->front + 1) % MAX_NEWSHIP_REQUESTS;
    rq->size--;
    return ship;
}
void destroyPriorityQueue(PriorityQueue* pq) {
    // Free all remaining ships in the queue
    while (pq->size > 0) {
        ShipRequest* ship = pop(pq);
        free(ship);
    }
    free(pq->heap);
    free(pq);
}
void cleanupRemainderQueue(RemainderQueue* rq) {
    while (rq->size > 0) {
        ShipRequest* ship = removeFromRemainder(rq);
        free(ship);
    }
}


int main_msgq_id;
int solver_msgq_ids[MAX_SOLVERS];
MainSharedMemory* shared_mem;


void dockShip(Dock* dock, const ShipRequest* ship, int currentTimestep) {
   
    dock->isOccupied = 1;
    dock->lastCargoTimestep = -1;
    dock->lastdockedTimestep = currentTimestep; 
    dock->lastUndockedTimestep=-1;   
    dock->currentShipId = ship->shipId;
    dock->direction = ship->direction;
    dock->cargoCount = ship->numCargo;
    memcpy(dock->remainingCargo, ship->cargo, sizeof(int)*ship->numCargo);

}

void handleEmergencyShips(PriorityQueue* pq, RemainderQueue* rq, Dock* docks, int dockCount, int currentTimestep, int msgq_id) {
    // Extract and sort emergency ships by category
      // Get total ships first
      int totalShips = pq->size;
    
      // Allocate dynamic arrays
      ShipRequest** emergencyShips = malloc(totalShips * sizeof(ShipRequest*));
      ShipRequest** nonEmergency = malloc(totalShips * sizeof(ShipRequest*));

      int emergencyCount = 0;
      int nonEmergencyCount = 0;

      
      // Check allocation success
      if(!emergencyShips || !nonEmergency) {
          perror("Memory allocation failed");
          exit(EXIT_FAILURE);
      }
    
    // Separate ships
    while (pq->size > 0) {
        ShipRequest* ship = pop(pq);
        if (ship->emergency == 1 && ship->direction == 1) {
            emergencyShips[emergencyCount++] = ship;
        } else {
            nonEmergency[nonEmergencyCount++] = ship;
        }
    }
    
    // Sort emergency ships by category (ascending)
    for (int i = 0; i < emergencyCount - 1; i++) {
        for (int j = 0; j < emergencyCount - i - 1; j++) {
            if (emergencyShips[j]->category > emergencyShips[j+1]->category) {
                ShipRequest* temp = emergencyShips[j];
                emergencyShips[j] = emergencyShips[j+1];
                emergencyShips[j+1] = temp;
            }
        }
    }
    
    // Find available docks
    bool dockUsed[MAX_DOCKS] = {0};
    for (int i = 0; i < dockCount; i++) {
        dockUsed[i] = docks[i].isOccupied;
    }
    
    // Dock emergency ships
    for (int e = 0; e < emergencyCount; e++) {
        ShipRequest* ship = emergencyShips[e];
        bool docked = false;
        
        // Find first available dock with sufficient category
        for (int d = 0; d < dockCount; d++) {
            if (!dockUsed[d] && 
                !docks[d].isOccupied &&
                docks[d].category >= ship->category  ){
                // Dock the ship
                MessageStruct msg = {
                    .mtype = 2,
                    .timestep = currentTimestep,
                    .shipId = ship->shipId,
                    .direction = ship->direction,
                    .dockId = docks[d].dockId
                };
                msgsnd(msgq_id, &msg, sizeof(MessageStruct)-sizeof(long), 0);
                
                dockShip(&docks[d], ship, currentTimestep);
                dockUsed[d] = true;
                docked = true;
                printf("EMERGENCY: Docked Ship %d at Dock %d\n", ship->shipId, docks[d].dockId);
                break;
            }
        }
        
        if (!docked) {
            addToRemainder(rq, ship);
            printf("Failed to dock emergency Ship %d\n", ship->shipId);
        } else {
            free(ship);
        }
    }
    
    // Restore non-emergency ships
    for (int i = 0; i < nonEmergencyCount; i++) {
        push(pq, nonEmergency[i]);
    }

    free(emergencyShips);
    free(nonEmergency);
}




void processShips(PriorityQueue* pq, RemainderQueue* rq, Dock* docks, int dockCount, int currentTimestep, int msgq_id) {
    int initialShipCount = pq->size;
    
    // Dynamically allocate array with exact size needed
    ShipRequest** allShips = malloc(initialShipCount * sizeof(ShipRequest*));
    if (!allShips) {
        perror("Failed to allocate ships array");
        return;
    }

    // Extract ships with bounds checking
    int shipCount = 0;
    while (pq->size > 0 && shipCount < initialShipCount) {
        allShips[shipCount++] = pop(pq);
    }

    // Handle any unexpected overflow
    if (pq->size > 0) {
        fprintf(stderr, "CRITICAL: Priority queue inconsistency detected (%d remaining)\n", pq->size);
        while (pq->size > 0) {
            ShipRequest* ship = pop(pq);
            if (ship->emergency) {
                addToRemainder(rq, ship);
            } else {
                free(ship);
            }
        }
    }
    // Process each ship
    for (int s = 0; s < shipCount; s++) {
        ShipRequest* ship = allShips[s];
        bool docked = false;

        // Check if regular ship has expired
        if (ship->emergency == 0 && currentTimestep > ship->timestep + ship->waitingTime && ship->direction == 1) {  
            free(ship);
            continue; // Let validation resend as new request
        }

        // Try to dock the ship
        for (int i = 0; i < dockCount; i++) {
            Dock* dock = &docks[i];

            // Integrated docking logic
            if (!dock->isOccupied && dock->category >= ship->category && 
                currentTimestep > dock->lastUndockedTimestep) {
                    
                // Prepare docking message
                MessageStruct msg;
                msg.mtype = 2;
                msg.timestep = currentTimestep;
                msg.shipId = ship->shipId;
                msg.direction = ship->direction;
                msg.dockId = dock->dockId;
                        
                // Send message to validation
                if (msgsnd(msgq_id, &msg, sizeof(MessageStruct)-sizeof(long), 0) == -1) {
                    perror("Dock assignment failed");
                    exit(EXIT_FAILURE);
                }
                        
                // Update dock               
                dockShip(dock, ship, currentTimestep);
                docked = true;
                       
                break;
            }
        }

        if (!docked) {
            // Add to remainder queue if still valid
            if (ship->emergency == 1 || ship->direction == -1 || 
                (ship->direction == 1 && currentTimestep <= ship->timestep + ship->waitingTime)) {
                addToRemainder(rq, ship);
            } else {
                free(ship);
            }
        } else {
            // Ship was docked, free the memory
            free(ship);
        }
    }
    free(allShips);
}


void sortCranesDesc(int* craneOrder, const Dock* dock) {
    for (int i = 0; i < dock->category; i++) craneOrder[i] = i;
    for (int i = 0; i < dock->category-1; i++) {
        for (int j = 0; j < dock->category-i-1; j++) {
            if (dock->cranes[craneOrder[j]] < dock->cranes[craneOrder[j+1]]) {
                int temp = craneOrder[j];
                craneOrder[j] = craneOrder[j+1];
                craneOrder[j+1] = temp; 
            }
        }
    }
}
void sortCargoDesc(int* cargoOrder, const Dock* dock) {
    for (int i = 0; i < dock->cargoCount; i++) cargoOrder[i] = i;
    for (int i = 0; i < dock->cargoCount - 1; i++) {
        for (int j = 0; j < dock->cargoCount - i - 1; j++) {
            if (dock->remainingCargo[cargoOrder[j]] < dock->remainingCargo[cargoOrder[j + 1]]) {
                int temp = cargoOrder[j];
                cargoOrder[j] = cargoOrder[j + 1];
                cargoOrder[j + 1] = temp;
            }
        }
    }
}



bool handleCargoOperations(Dock* docks, int dockCount, int currentTimestep) {
    for (int i = 0; i < dockCount; i++) {
        Dock* dock = &docks[i];

        // Enforce T+1 rule: Skip if ship just undocked or dock is empty
        if (!dock->isOccupied || currentTimestep <= dock->lastdockedTimestep) continue;

        // Reset crane usage flags
        memset(dock->craneUsed, 0, sizeof(dock->craneUsed));

        // Prepare sorted index arrays
        int craneOrder[MAX_CRANES];
        int cargoOrder[MAX_CARGO_COUNT];
        sortCranesDesc(craneOrder, dock);
        sortCargoDesc(cargoOrder, dock);


      

        bool cargoRemaining = false;

        for (int i = 0; i < dock->cargoCount; i++) {
            int cargoIdx = cargoOrder[i];
            if (dock->remainingCargo[cargoIdx] <= 0) continue;

            for (int c = 0; c < dock->category; c++) {
                int craneIdx = craneOrder[c];
                if (dock->craneUsed[craneIdx]) continue;

                if (dock->cranes[craneIdx] >= dock->remainingCargo[cargoIdx]) {
                    // Create message
                    MessageStruct msg = {
                        .mtype = 4,
                        .timestep = currentTimestep,
                        .shipId = dock->currentShipId,
                        .direction = dock->direction,
                        .dockId = dock->dockId,
                        .cargoId = cargoIdx,
                        .craneId = craneIdx
                    };

                        

                    // Send message to validation
                    if (msgsnd(main_msgq_id, &msg, sizeof(MessageStruct) - sizeof(long), 0) == -1) {
                        perror("    Cargo operation failed");
                        return false;
                    }

                    // Mark crane used and cargo loaded
                    dock->craneUsed[craneIdx] = true;
                    dock->remainingCargo[cargoIdx] = 0;
                
                    break;
                }
            }

            if (dock->remainingCargo[cargoIdx] > 0) {
                cargoRemaining = true;
            }
        }

        if (!cargoRemaining) {
            dock->lastCargoTimestep = currentTimestep;
            dock->cargoOp = true;
        } 
    }

    return true;
}

//Undocking logic

pthread_mutex_t solver_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t solver_cond = PTHREAD_COND_INITIALIZER;
volatile int correct_guess_found = 0;
char global_correct_guess[MAX_AUTH_STRING_LEN] = {0};

// Add these global constants
static const char validFirstLast[] = "56789";  // 5 options
static const char validMiddle[] = "56789.";    // 6 options

// Thread arguments structure
typedef struct {
    int solverId;
    Dock* dock;
    int currentTimestep;
    int stringLength;
    unsigned long long startIdx;
    unsigned long long endIdx;
    int found;
} SolverThreadArgs;

// Modified solver thread function using index-based guessing
void* solver_thread_function(void* arg) {
    SolverThreadArgs* args = (SolverThreadArgs*)arg;
    Dock* dock = args->dock;
    int solverId = args->solverId;
    int stringLength = args->stringLength;
    
    // Initialize this solver with dock ID
    SolverRequest initReq = {
        .mtype = 1,
        .dockId = dock->dockId
    };
    msgsnd(solver_msgq_ids[solverId], &initReq, sizeof(SolverRequest)-sizeof(long), 0);

    char currentGuess[MAX_AUTH_STRING_LEN] = {0};
    
    // Main guessing loop
    for(unsigned long long attempt = args->startIdx; 
        attempt < args->endIdx && !correct_guess_found; 
        attempt++) {
        
        // Generate valid string directly from attempt index
        unsigned long long temp = attempt;
        
        // First character (5 options)
        currentGuess[0] = validFirstLast[temp % 5];
        temp /= 5;
        
        // Last character (5 options) if length > 1
        if(stringLength > 1) {
            currentGuess[stringLength-1] = validFirstLast[temp % 5];
            temp /= 5;
        }
        
        // Middle characters (6 options)
        for(int j = 1; j < stringLength-1; j++) {
            currentGuess[j] = validMiddle[temp % 6];
            temp /= 6;
        }
        
        currentGuess[stringLength] = '\0';
        
        // Send guess to solver
        SolverRequest guessReq = {
            .mtype = 2,
            .dockId = dock->dockId
        };
        strncpy(guessReq.authStringGuess, currentGuess, MAX_AUTH_STRING_LEN-1);
        
        if(msgsnd(solver_msgq_ids[solverId], &guessReq, sizeof(SolverRequest)-sizeof(long), 0) == -1) {
            if(errno != EAGAIN) perror("Failed to send guess");
            continue;
        }
        
        // Get response
        SolverResponse guessResp;
        if(msgrcv(solver_msgq_ids[solverId], &guessResp, 
                sizeof(SolverResponse)-sizeof(long), 3, 0) > 0) {
            if(guessResp.guessIsCorrect == 1) {
                pthread_mutex_lock(&solver_mutex);
                if(!correct_guess_found) {
                    correct_guess_found = 1;
                    strcpy(global_correct_guess, currentGuess);
                    pthread_cond_broadcast(&solver_cond);
                }
                pthread_mutex_unlock(&solver_mutex);
                break;
            }
        }
    }
    return NULL;
}

int undockShip(Dock *dock, int solverCount, int currentTimestep) {
    if (!dock->isOccupied) {
        printf("No ship at dock %d to undock\n", dock->dockId);
        return 0;
    }

    // Calculate the frequency string length
    int stringLength = dock->lastCargoTimestep - dock->lastdockedTimestep;
    if(stringLength <= 0 || stringLength >= MAX_AUTH_STRING_LEN) {
        printf("Invalid frequency string length: %d for dock %d\n", stringLength, dock->dockId);
        return 0;
    }

    printf("Attempting to undock ship %d from dock %d with string length %d\n", 
           dock->currentShipId, dock->dockId, stringLength);

    unsigned long long totalCombo = 5ULL * 5ULL;  // First and last chars
    for(int i=0; i<stringLength-2; i++) totalCombo *= 6ULL;  // Middle chars
           
    
    // Reset global state for this undocking operation
    pthread_mutex_lock(&solver_mutex);
    correct_guess_found = 0;
    memset(global_correct_guess, 0, MAX_AUTH_STRING_LEN);
    pthread_mutex_unlock(&solver_mutex);
    
    // Create and launch solver threads
    pthread_t threads[MAX_SOLVERS];
    SolverThreadArgs thread_args[MAX_SOLVERS];
    
    int num_threads = solverCount < MAX_GUESS_THREADS ? solverCount : MAX_GUESS_THREADS;
   // printf("Launching %d solver threads for dock %d\n", num_threads, dock->dockId);
    
   unsigned long long perThread = totalCombo / num_threads;
    
   for(int i=0; i<num_threads; i++) {
       thread_args[i] = (SolverThreadArgs){
           .solverId = i,
           .dock = dock,
           .currentTimestep = currentTimestep,
           .stringLength = stringLength,
           .startIdx = i * perThread,
           .endIdx = (i == num_threads-1) ? totalCombo : (i+1)*perThread
       };
       
       pthread_create(&threads[i], NULL, solver_thread_function, &thread_args[i]);
   }
    
    // Wait for all threads to complete
    for(int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // Check if a correct guess was found
    if(correct_guess_found) {
        
        // Update shared memory with the correct guess
        pthread_mutex_lock(&solver_mutex);
        strncpy(shared_mem->authStrings[dock->dockId], global_correct_guess, MAX_AUTH_STRING_LEN-1);
        shared_mem->authStrings[dock->dockId][MAX_AUTH_STRING_LEN-1] = '\0';
        pthread_mutex_unlock(&solver_mutex);
        
 
       
        // Send undock message
        MessageStruct undockMsg = {
            .mtype = 3,
            .timestep = currentTimestep,
            .shipId = dock->currentShipId,
            .dockId = dock->dockId,
            .direction = dock->direction
        };
        
        if(msgsnd(main_msgq_id, &undockMsg, sizeof(MessageStruct)-sizeof(long), 0) == -1) {
            perror("Undock message failed");
            return 0;
        }
        
        
        // Update dock status
        dock->isOccupied = 0;
        dock->currentShipId = -1;
        dock->lastdockedTimestep = -1;  
        dock->lastUndockedTimestep = currentTimestep;
        dock->direction = 0;
        dock->lastCargoTimestep = -1;
        dock->cargoOp = false;
        return 1;
    } else {

        return 0;
    }
}

// Updated dock checking logic
void check_and_undock(Dock *docks, int dockCount, int solverCount, int currentTimestep) {
    for(int i = 0; i < dockCount; i++) {
       if(docks[i].isOccupied && 
            docks[i].lastCargoTimestep > 0 && 
            docks[i].lastCargoTimestep < currentTimestep) {
            printf("Initiating undock for dock %d\n", docks[i].dockId);
        undockShip(&docks[i], solverCount, currentTimestep); 
        }

    }
}

ShipRequest* copyShip(const ShipRequest* src) {
    if (!src) {
        fprintf(stderr, "copyShip: NULL source pointer\n");
        return NULL;
    }
    
    ShipRequest* dest = malloc(sizeof(ShipRequest));
    if (!dest) {
        perror("copyShip: malloc failed");
        return NULL;
    }
    memcpy(dest, src, sizeof(ShipRequest));
    return dest;
}

void printSystemStatus(int currentTimestep, PriorityQueue* pq, Dock* docks, int dockCount) {
    // Count emergency ships
    int emergencyShips = 0;
    for(int i = 0; i < pq->size; i++) {
        if(pq->heap[i]->emergency == 1) emergencyShips++;
    }
    
    // Count dock status
    int freeDocks = 0;
    int occupiedDocks = 0;
    for(int i = 0; i < dockCount; i++) {
        if(docks[i].isOccupied) {
            occupiedDocks++;
        } else if(currentTimestep > docks[i].lastUndockedTimestep) {
            freeDocks++;
        }
    }
    
    printf("\n=== Timestep %d Initial Status ===\n", currentTimestep);
    printf("%d dockcount \n",dockCount);
    printf("Emergency ships in queue: %d\n", emergencyShips);
    printf("Docks - Free: %d, Occupied: %d, In cooldown: %d\n", 
           freeDocks, 
           occupiedDocks,
           dockCount - freeDocks - occupiedDocks);
    printf("==================================\n");
}


// MAIN PROGRAM STARTS HERE
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <testcase_number>\n", argv[0]);
        return EXIT_FAILURE;
    }


    char inputFileName[50];
    sprintf(inputFileName, "testcase%s/input.txt", argv[1]);

    FILE *file = fopen(inputFileName, "r");
    if (!file) {
        perror("Failed to open input file");
        return 1;
    }

    key_t shm_key, main_msgq_key, solver_msgq_keys[MAX_SOLVERS];
    int shm_id;
    int solverCount, dockCount;
    Dock docks[MAX_DOCKS];

    // Read shared memory and message queue keys
    fscanf(file, "%d", &shm_key);
    fscanf(file, "%d", &main_msgq_key);
    
    // Read number of solvers and their message queue keys
    fscanf(file, "%d", &solverCount);
    for (int i = 0; i < solverCount; i++) {
        fscanf(file, "%d", &solver_msgq_keys[i]);
    }
    
    // Read number of docks
    fscanf(file, "%d", &dockCount);
    for (int i = 0; i < dockCount; i++) {
        docks[i].dockId = i;
        fscanf(file, "%d", &docks[i].category);
        for (int j = 0; j < docks[i].category; j++) {
            fscanf(file, "%d", &docks[i].cranes[j]);
        }
    }
    
    fclose(file);

    // Sort docks by category in ascending order
    qsort(docks, dockCount, sizeof(Dock), compareDocks);

    // Print the parsed information
    printf("Shared Memory Key: %d\n", shm_key);
    printf("Main Message Queue Key: %d\n", main_msgq_key);
    printf("Number of Solvers: %d\n", solverCount);
    for (int i = 0; i < solverCount; i++) {
        printf("Solver %d Message Queue Key: %d\n", i, solver_msgq_keys[i]);
    }
    printf("Number of Docks: %d\n", dockCount);
    for (int i = 0; i < dockCount; i++) {
        printf(" Dock ID: %d, Category: %d, Crane Capacities: ",
                docks[i].dockId, docks[i].category);
        for (int j = 0; j < docks[i].category; j++) {
            printf("%d ", docks[i].cranes[j]);
        }
        docks[i].isOccupied = 0;
        docks[i].lastdockedTimestep = -2;  // Allow immediate docking
        printf("\n");
    }


    
    // Attach Shared Memory
    shm_id = shmget(shm_key, sizeof(MainSharedMemory), 0666);
    if (shm_id < 0) {
        perror("shmget failed");
        exit(EXIT_FAILURE);
    }
    shared_mem = (MainSharedMemory *)shmat(shm_id, NULL, 0);
    if (shared_mem == (void *)-1) {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }

    // Setup Message queues
    main_msgq_id = msgget(main_msgq_key, 0666);
    if (main_msgq_id < 0) {
        perror("Main message queue access failed");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < solverCount; i++) {
        solver_msgq_ids[i] = msgget(solver_msgq_keys[i], 0666);
        if (solver_msgq_ids[i] < 0) {

            perror("Solver message queue access failed");
            exit(EXIT_FAILURE);
        }
    }
    

    int currentTimestep = 1;
    // Initialize the priority queue
    PriorityQueue* pq = createPriorityQueue(MAX_NEWSHIP_REQUESTS);
    // Initialize the remainder queue
    // This queue will hold ships that couldn't be docked immediately
    RemainderQueue rq;
    initRemainderQueue(&rq);

    if(pthread_mutex_init(&solver_mutex, NULL) != 0) {
        perror("Mutex initialization failed");
        exit(EXIT_FAILURE);
    }
    
    if(pthread_cond_init(&solver_cond, NULL) != 0) {
        perror("Condition variable initialization failed");
        exit(EXIT_FAILURE);
    }
    
    

    while (1) {
        // Step 1: Poll for new requests
        MessageStruct incomingMsg;
        if (msgrcv(main_msgq_id, &incomingMsg, sizeof(MessageStruct) - sizeof(long), 1, 0) < 0) {
            perror("msgrcv failed initial just after starting while loop");
            break;
        } 

        if (incomingMsg.isFinished == 1) {
            printf("Simulation complete.\n");
            break;
        }

        // incomingMsg.numShipRequests has the number of new ship requests
        int numRequests = incomingMsg.numShipRequests;
                    // Process remainder queue from previous timestep
            ShipRequest* req;
            while ((req = removeFromRemainder(&rq)) != NULL) {
                push(pq, req);
        }
         
        for (int i = 0; i < numRequests; i++) {
            ShipRequest* shared_ship = &shared_mem->newShipRequests[i];
            ShipRequest* ship_copy = copyShip(shared_ship);
            push(pq, ship_copy);
        }      
        
	
        handleEmergencyShips(pq, &rq, docks, dockCount, currentTimestep, main_msgq_id);
         // Process current priority queue
        processShips(pq, &rq, docks, dockCount, currentTimestep, main_msgq_id);
   	

     
       
        check_and_undock(docks,dockCount,solverCount,currentTimestep);
            
        
        handleCargoOperations(docks, dockCount, currentTimestep);
        MessageStruct advanceMsg;
        advanceMsg.mtype = 5;
        if (msgsnd(main_msgq_id, &advanceMsg, sizeof(MessageStruct) - sizeof(long), 0) < 0) {
            perror("msgsnd to advance currentTimestep failed");
            break;
        }

        currentTimestep++;
        printf("Advancing to Timestep %d\n", currentTimestep);   
    }

    pthread_mutex_destroy(&solver_mutex);
    pthread_cond_destroy(&solver_cond);
    destroyPriorityQueue(pq);
    cleanupRemainderQueue(&rq);
    shmdt(shared_mem); 
    return 0;
}
