#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/capability.h>
#include <sys/psx_syscall.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

#define NUM_THREADS 5

/*
 * Shared state to coordinate threads and track results
 */
static pthread_barrier_t barrier;
static volatile int keepcaps_values[NUM_THREADS];
static volatile int thread_ready = 0;

/*
 * Worker thread that reads its own KEEPCAPS state after the main thread
 * modifies it using cap_prctlw().
 *
 * With libpsx: thread should see the synchronized value
 * Without libpsx: thread keeps its original value
 */
static void *worker_thread(void *arg) {
    int thread_id = *(int *)arg;
    long int value;

    /* Wait for all threads to be created */
    pthread_barrier_wait(&barrier);

    /* Signal ready and wait for main thread to modify KEEPCAPS */
    __sync_fetch_and_add(&thread_ready, 1);
    pthread_barrier_wait(&barrier);

    /* Small delay to ensure main thread's cap_prctlw() has completed */
    usleep(10000);

    /* Read this thread's KEEPCAPS value */
    value = cap_prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0, 0);
    if (value == -1) {
        perror("worker: failed to get KEEPCAPS");
        exit(1);
    }

    keepcaps_values[thread_id] = value;

    return NULL;
}

int main(int argc, char **argv) {
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    long int initial_keepcaps, set_value, verify_value;
    int i;
    int all_synchronized;

#ifdef EXPECT_PSX_SYNC
    printf("Testing libcap WITH libpsx (expecting thread synchronization)\n");
#else
    printf("Testing libcap WITHOUT libpsx (expecting NO thread synchronization)\n");
#endif
    fflush(stdout);

    /* Initialize barrier for thread coordination */
    pthread_barrier_init(&barrier, NULL, NUM_THREADS + 1);

    /* Get initial KEEPCAPS value */
    initial_keepcaps = cap_prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0, 0);
    if (initial_keepcaps == -1) {
        perror("FAILED: unable to get initial KEEPCAPS");
        exit(1);
    }

    /*
     * Set KEEPCAPS to a known value before creating threads.
     * This ensures all threads start with the same state.
     */
    set_value = 0;
    if (cap_prctlw(PR_SET_KEEPCAPS, set_value, 0, 0, 0, 0) != 0) {
        perror("FAILED: unable to set initial KEEPCAPS");
        exit(1);
    }

    /* Create worker threads - they will inherit current KEEPCAPS value */
    for (i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, worker_thread, &thread_ids[i]) != 0) {
            perror("FAILED: pthread_create");
            exit(1);
        }
    }

    /* Wait for all threads to start */
    pthread_barrier_wait(&barrier);

    /* Wait for all threads to be ready */
    while (__sync_fetch_and_add(&thread_ready, 0) < NUM_THREADS) {
        usleep(1000);
    }

    /*
     * Now change KEEPCAPS using cap_prctlw().
     * With libpsx: this will synchronize across ALL threads via psx_syscall().
     * Without libpsx: this affects only the calling thread via regular prctl().
     */
    set_value = 1;
    if (cap_prctlw(PR_SET_KEEPCAPS, set_value, 0, 0, 0, 0) != 0) {
        perror("FAILED: unable to set KEEPCAPS");
        exit(1);
    }

    /* Verify main thread sees the new value */
    verify_value = cap_prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0, 0);
    if (verify_value != set_value) {
        printf("FAILED: main thread KEEPCAPS mismatch: expected %ld, got %ld\n",
               set_value, verify_value);
        exit(1);
    }

    /* Release worker threads to read their KEEPCAPS values */
    pthread_barrier_wait(&barrier);

    /* Wait for all threads to complete */
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Check if all worker threads saw the synchronized value */
    all_synchronized = 1;
    for (i = 0; i < NUM_THREADS; i++) {
        if (keepcaps_values[i] != set_value) {
            all_synchronized = 0;
        }
    }

    pthread_barrier_destroy(&barrier);

    /* Verify behavior matches expectations based on linking */
#ifdef EXPECT_PSX_SYNC
    if (all_synchronized) {
        printf("PASSED: All %d worker threads synchronized to KEEPCAPS=%ld\n",
               NUM_THREADS, set_value);
        printf("        (libpsx correctly synchronized across all threads)\n");
        exit(0);
    } else {
        printf("FAILED: libpsx linked but threads not synchronized:\n");
        printf("  Main thread KEEPCAPS: %ld\n", set_value);
        for (i = 0; i < NUM_THREADS; i++) {
            printf("  Worker thread %d: %d\n", i, keepcaps_values[i]);
        }
        exit(1);
    }
#else
    if (!all_synchronized) {
        printf("PASSED: Worker threads NOT synchronized (expected without libpsx)\n");
        printf("  Main thread KEEPCAPS: %ld\n", set_value);
        printf("  Worker threads KEEPCAPS: 0 (retained original value)\n");
        exit(0);
    } else {
        printf("FAILED: threads synchronized but libpsx NOT linked:\n");
        printf("  Main thread KEEPCAPS: %ld\n", set_value);
        for (i = 0; i < NUM_THREADS; i++) {
            printf("  Worker thread %d: %d (unexpected!)\n", i, keepcaps_values[i]);
        }
        printf("This suggests libpsx may be incorrectly linked\n");
        exit(1);
    }
#endif
}
