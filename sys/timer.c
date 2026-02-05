// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif


#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "misc.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

#include "console.h"

#include <stdint.h>

//#include "stddef.h"

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;


// Implementing preemptive multitasking CP3
// Before we had cooperative multiasking where the threads voluntarily call sleep and yield
// To implement preemptive, need to implement a regular periodic timer that fires
// Triggers an interrupt at a regular interval regardless of what the user is doing
// Use the same hardware timer comparator stcmp 

// Start with defining the next tick that stores the time at which the next event will occur
// Initialize to 0 as nothing is on right now
static unsigned long long next_preemption_tick = 0;

// Define a variable that has the constant interval between the preemption events
// Keeping it at 100Hz for now
#define PREEMPTION_INTERVAL (TIMER_FREQ / 100ULL)


// Global flag that indicages that the preemption tick is expired
static volatile int sched_tick_pending = 0;


// INTERNAL FUNCTION DECLARATIONS
//

static void program_next_stcmp(void);

// EXPORTED FUNCTION DEFINITIONS
//

extern void timer_irq_probe(void);

void timer_init(void) {
    //kprintf("Calling set_stcmp() now=%llu\n", rdtime());
    //set_stcmp(UINT64_MAX);

    // Need to change the timer_init function for CP3
    // Create a variable now that reads the current value of hte time counter
    unsigned long long now = rdtime();

    // Now compute the absolute time for the first preemption event and store that value in the global var next_preemption_tick
    next_preemption_tick = now + PREEMPTION_INTERVAL;

    // Instead of programming the compare register tot he max value, change it to th enext_Tick so that we generate an event when stime reaches that value
    set_stcmp(next_preemption_tick);

    timer_initialized = 1;
}
/* Function Interface:
    void alarm_init(struct alarm *al, const char *name)
    Inputs: 
        struct alarm *al - pointer to an alarm structure to initialize
        const char *name - optional name for the alarm (if NULL, defaults to "alarm")
    Outputs: 
        None
    Description: 
        Initializes an alarm structure by setting up its condition variable, 
        clearing its next pointer, and setting an initial wake time based on the 
        current system time.
    Side Effects: 
        - Calls condition_init() to initialize the alarm condition
        - Reads current system time using rdtime()
*/
void alarm_init(struct alarm *al, const char *name) {
    if (name == NULL) {
        name = (char *)"alarm"; // default name if none provided
    }

    condition_init(&al->cond, name);  // initialize condition variable for alarm
    al->next = 0;                     // clear linked-list pointer
    al->twake = rdtime();             // initialize wake time to current time
}

/* Function Interface:
    void alarm_sleep(struct alarm *al, unsigned long long tcnt)
    Inputs:
        struct alarm *al - pointer to the alarm structure associated with the sleeping thread
        unsigned long long tcnt - number of time units to sleep before waking
    Outputs:
        None
    Description:
        Puts the current thread to sleep until the specified time duration has passed.
        The alarm is added into a global, ordered sleep list. When the timer interrupt
        occurs and the alarm time is reached, the thread is awakened.
    Side Effects:
        - Modifies the global sleep_list
        - Disables and restores interrupts during critical manipulation
        - Puts the thread to sleep via condition_wait()
*/
void alarm_sleep(struct alarm *al, unsigned long long tcnt) {
    unsigned long long now = rdtime();

    // Prevent integer overflow: if (twake + tcnt) wraps around, cap it
    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;

    // If the alarm time is already in the past, return immediately
    if (al->twake < now)
        return;

    long pre = disable_interrupts(); // Enter critical section

    unsigned long long target = al->twake;

    // Insert the alarm into the global sleep list in sorted order
    if (sleep_list == NULL || target < sleep_list->twake) {
        al->next = sleep_list;
        sleep_list = al;
    } else {
        struct alarm *ptr = sleep_list;
        // Walk through list until correct insertion point is found
        while ((ptr->next != NULL) && (ptr->next->twake <= target)) {
            ptr = ptr->next;
        }
        // Insert the alarm at proper position
        al->next = ptr->next;
        ptr->next = al;
    }

    // Set the timer compare register to next wake-up time
    // set_stcmp(sleep_list->twake);
    // Replace set_stcmp with the helper program_next_stcmp that we wrote
    program_next_stcmp();

    restore_interrupts(pre); // Exit critical section, re-enable interrupts

    // Put the thread to sleep, waiting to be woken when its alarm triggers
    condition_wait(&al->cond);
}


// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}

/* Function Interface:
    void handle_timer_interrupt(void)
    Inputs:
        None (invoked automatically by the interrupt system)
    Outputs:
        None
    Description:
        Handles timer interrupts by checking the global sleep list of alarms.
        Any alarms whose wake times have been reached are removed from the list
        and their threads are woken via condition broadcasts. The timer comparison
        register is updated to the next earliest alarm or disabled if there are none.
    Side Effects:
        - Modifies global sleep_list
        - Wakes waiting threads using condition_broadcast()
        - Updates timer compare register (set_stcmp)
*/
void handle_timer_interrupt(void) {

    struct alarm *head;

    // Change the type to unsinged long long as next_preemption_tick is defined as such
    unsigned long long now = rdtime(); // current system time

    // int woke_count = 0;

    // Process any alarms whose wake times have passed or are now due
    while ((sleep_list != NULL) && (sleep_list->twake <= now)) {
        head = sleep_list;
        sleep_list = head->next;   // remove from head of list
        head->next = NULL;         // ensure pointer is cleared
        condition_broadcast(&head->cond); // wake up waiting threads for this alarm
        // woke_count++;
    }

    // If no alarms remain, disable timer by setting a far-future compare value
    //if (sleep_list == NULL) {
        //set_stcmp(UINT64_MAX);
    //} else {
        // Otherwise, schedule timer for next alarm wake-up
        //set_stcmp(sleep_list->twake);
    //}

    // Create the preemption tick logic
    // Check if the timer interrupt corresponds to the preenption tick by checking next_preemption_tick
    // If the next tick is not 0 which means it has been initialized and if the current time has passed the next tick, then its the preemption
    if(next_preemption_tick != 0 && now >= next_preemption_tick){

        // Advance the next tick by one interval as we now that we are past the now time
        next_preemption_tick = next_preemption_tick + PREEMPTION_INTERVAL;

        // Using a while loop, keep adding intevals untl the next tick is greater than the current time
        while(next_preemption_tick <= now){

            // Keep advancing until we satisfy the while
            next_preemption_tick = next_preemption_tick + PREEMPTION_INTERVAL;

        }

        // Tell the rest of the kernel that the timer intterupt included a preemption event using this global flag
        sched_tick_pending = 1;
    }

    // Now program stcmp to the earliest interesting time by computing the next compare value
    // Use the helper function
    program_next_stcmp();
}

// Connect the timer to the interrupt as intr.c will call this function
// Define a function that tells the scheduler whether the timer interrupt wants a preemption
int timer_preemption_flag(void){

    // Check the schedule tick pending flag, if it is set to 1 inside the timer intterupt, then the interval was passed
    // If it is 0, then there was no preemption requested
    if(!sched_tick_pending){

        return 0;
    }

    // Set the pending preemption to 0 so that its not prempt multiple times for a single interrupt
    sched_tick_pending = 0;

    // Return 1 as a schedule tick has happened
    return 1;

}

// Create a helper that computes the earliest time and calls set_stcmp()
// Two ways for the kernel to wake, either sleepers/alarms or the preemption tick
// Compute so that the function computes the earliest of the two
static void program_next_stcmp(void){

    // Start by defining next_cmp as the largest possible number which represents that no event is scheduled
    unsigned long long next_cmp = UINT64_MAX;

    // Now for the conditional computation
    // If the list is not empty then sleep_list->twake is a valid candidate for the next interrupt
    // Check if sleep_list->twake is the earliest thread wake-up time
    // Compare to next_cmp and pick the smaller one
    if(sleep_list != NULL && sleep_list->twake < next_cmp){

        // If past the condition, take the smaller one
        next_cmp = sleep_list->twake;
    }

    // Compare the next_cmp to the next_preemption tick and take the smaller of the two
    if(next_preemption_tick != 0 && next_preemption_tick < next_cmp){

        next_cmp = next_preemption_tick;
    }

    // Now program the compare register when the rdtime() reaches this value
    set_stcmp(next_cmp);
}