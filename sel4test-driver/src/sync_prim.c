#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "thread_lib.h"

void
sync_prim_initial(thread_sync_prim_t *sync_prim)
{
    /* clean up struct */
    sync_prim->pre = NULL;
    sync_prim->next = NULL;
    sync_prim->waiting_start = NULL;
    sync_prim->waiting_end = NULL;

    /* add it into sync_prim list */
    if (pool->sync_prim_start == NULL) {
        pool->sync_prim_start = sync_prim;
        pool->sync_prim_end = sync_prim;
    } else {
        sync_prim->pre = pool->sync_prim_end;
        pool->sync_prim_end->next = sync_prim;
        pool->sync_prim_end = sync_prim;
    }

    return;
}



void
sync_prim_destroy(thread_sync_prim_t *sync_prim)
{
    /* delete it from sync_prim list */
    if (pool->sync_prim_start == sync_prim && pool->sync_prim_end == sync_prim) {
        pool->sync_prim_start = NULL;
        pool->sync_prim_end = NULL;
    } else if (pool->sync_prim_start == sync_prim) {
        pool->sync_prim_start = sync_prim->next;
        sync_prim->next->pre = NULL;
        sync_prim->next = NULL;
    } else if (pool->sync_prim_end == sync_prim) {
        pool->sync_prim_end = sync_prim->pre;
        sync_prim->pre->next = NULL;
        sync_prim->pre = NULL;
    } else {
        sync_prim->pre->next = sync_prim->next;
        sync_prim->next->pre = sync_prim->pre;
        sync_prim->pre = NULL;
        sync_prim->next = NULL;
    }

    assert(sync_prim->pre == NULL);
    assert(sync_prim->next == NULL);
}



/*
    Synchronization primitive: lock

    thread_lock_create();
    thread_lock_destory();
    thread_lock_acquire();
    thread_lock_release().
*/

/*
    thread_lock *thread_lock_create();

    Return value:
        pointer to the new lock if successfully created a lock;
        NULL otherwise.
*/
thread_lock_t *
thread_lock_create()
{
    /* malloc */
    thread_lock_t *lock = malloc(sizeof(thread_lock_t));
    if (lock == NULL) {
        printf("Error: Cannot allocate a new thread lock.\n");
        return NULL;
    }

    sync_prim_initial((thread_sync_prim_t *) lock);

    /* initial parameters */
    lock->helder = -1;
    lock->held = 0;

    return lock;
}

/*
    void thread_lock_destory()
*/
void
thread_lock_destory(thread_lock_t *lock)
{
    assert(lock != NULL);

    if (lock->sync_prim.waiting_start != NULL) {
        printf("Cannot delete the lock! Some threads waiting on it.\n");
        assert(0);
    }

    sync_prim_destroy(&lock->sync_prim);

    free(lock);
}

/*
    void thread_lock_acquire()
*/
void
thread_lock_acquire_intern(thread_lock_t *lock, void *arg)
{

    assert(lock != NULL);
    // assert(lock->helder != co_current_id);

    while (lock->held) {
        thread_sleep((void *) lock, arg);
    }

    lock->helder = pool->t_running->t->t_id;
    lock->held = 1;

    return;
}



void
thread_lock_acquire(thread_lock_t *lock)
{
    thread_lock_acquire_intern(lock, lock);

    return;
}

/*
    void thread_lock_release()
*/
void
thread_lock_release_intern(thread_lock_t *lock, void *arg)
{

    assert(lock != NULL);
    // assert(lock->helder == co_current_id);
    // assert(lock->helder == pool->t_running->t->t_id);

    lock->helder = -1;
    lock->held = 0;

    thread_wakeup((void *) lock, arg);

    return;
}



void
thread_lock_release(thread_lock_t *lock)
{
    thread_lock_release_intern(lock, lock);

    return;
}

void
thread_lock_release_acquire(thread_lock_t *lock)
{
    assert(lock != NULL);

    lock->helder = -1;
    lock->held = 0;

    thread_wakeup_wait((void *) lock);

    return;
}



/* semaphore implementation */
thread_semaphore_t *
thread_semaphore_create()
{
    thread_semaphore_t *semaphore = malloc(sizeof(thread_semaphore_t));
    if (semaphore == NULL) {
        printf("Error: Cannot malloc a semaphore.\n");
        return NULL;
    }

    sync_prim_initial((thread_sync_prim_t *) semaphore);

    semaphore->count = 0;

    return semaphore;
}



void
thread_semaphore_destory(thread_semaphore_t *semaphore)
{
    assert(semaphore != NULL);

    if (semaphore->sync_prim.waiting_start != NULL) {
        printf("Cannot delete the semaphore! Some thread waiting on it\n");
        assert(0);
    }

    sync_prim_destroy(&semaphore->sync_prim);

    free(semaphore);

    return;
}



void
thread_semaphore_P(thread_semaphore_t *semaphore)
{
    assert(semaphore != NULL);

    while (! semaphore->count)
        thread_sleep((void *) semaphore, (void *) semaphore);

    assert(semaphore->count > 0);
    semaphore->count --;

    return;
}



void
thread_semaphore_V(thread_semaphore_t *semaphore)
{
    assert(semaphore != NULL);

    semaphore->count ++;
    assert(semaphore->count > 0);

    thread_wakeup((void *) semaphore, (void *) semaphore);

    return;
}



/* condition variables */
// thread_cv_t *
// thread_cv_create()));
//     if (cv == NULL) {
//         printf("Error: Cannot allocate a cv.\n");
//         return NULL;
//     }
//
//     return cv;
// }
//
//
//
// void
// thread_cv_destory(thread_cv_t *cv)
// {
//     assert(cv != NULL);
//     assert(cv->lock != NULL);(void *) lock
//
//     free(cv->lock);
//     free(cv);
//
//     return;
// }



void
thread_cv_wait(void *cv, thread_lock_t *lock)
{
    assert(cv != NULL);
    assert(lock != NULL);

    thread_lock_release_intern(lock, cv);

    thread_sleep(cv, cv);

    thread_lock_acquire_intern(lock, cv);

    return;
}

void
thread_cv_signal(void *cv)
{
    assert(cv != NULL);

    thread_wakeup(cv, cv);

    return;
}
