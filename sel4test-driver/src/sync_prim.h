#include <stdio.h>
#include <stdlib.h>



/*
    struct sync_prim
*/
struct thread_sync_prim_t {
    struct thread_sync_prim_t *pre;
    struct thread_sync_prim_t *next;

    void *waiting_start;

    void *waiting_end;
};
typedef struct thread_sync_prim_t thread_sync_prim_t;

/*
    struct Lock
*/
struct thread_lock_t {
    thread_sync_prim_t sync_prim;

    int helder;         /* specify which thread holding the lock */

    int held;           /* if a lock is held */

    // void *resource;     /* resource the lock take charge of */
};
typedef struct thread_lock_t thread_lock_t;



/*
    struct semaphore
    Dijkstra-style semaphore
*/
struct thread_semaphore_t {
    thread_sync_prim_t sync_prim;

    int count;
};
typedef struct thread_semaphore_t thread_semaphore_t;



/*
    struct condition variable
*/
// struct thread_cv_t {
//     void *addr;
//
//     thread_lock_t *lock;
// };
// typedef struct thread_cv_t thread_cv_t;


thread_lock_t * thread_lock_create();

void thread_lock_destory(thread_lock_t *lock);

void thread_lock_acquire(thread_lock_t *lock);

void thread_lock_release(thread_lock_t *lock);

void thread_lock_release_acquire(thread_lock_t *lock);

thread_semaphore_t *thread_semaphore_create();

void thread_semaphore_destory(thread_semaphore_t *semaphore);

void thread_semaphore_P(thread_semaphore_t *semaphore);

void thread_semaphore_V(thread_semaphore_t *semaphore);

// thread_cv_t *thread_cv_create();
//
// void thread_cv_destory(thread_cv_t* cv);

void thread_cv_wait(void *cv, thread_lock_t *lock);

void thread_cv_signal(void *cv);
