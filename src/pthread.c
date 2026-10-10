#include "pthread_private.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// POSIX threads on the kernel's threads, critical sections, events and TLS slots, for newlib, libgcc and libstdc++.

enum {
	// The XEX header asks for 64 TLS slots per thread (tools/elf2xex.py TLS_SLOTS).
	KEY_SLOTS = 64,
	// POSIX's PTHREAD_DESTRUCTOR_ITERATIONS.
	DESTRUCTOR_ROUNDS = 4,
	THREAD_DETACHED = 1,
	THREAD_ENDED = 2,
	ONCE_DONE = 1,
};

// One millisecond, relative, in 100 ns units.
static const int64_t POLL_INTERVAL = -10000;

typedef struct Thread {
	// Set only for a joinable thread pthread_create started.
	HANDLE handle;
	void *(*start)(void *);
	void *argument;
	void *result;
	// Wakes the thread from a condition variable wait.
	HANDLE wake;
	struct Thread *next_waiter;
	uint32_t state;
} Thread;

typedef struct {
	RTL_CRITICAL_SECTION lock;
	Thread *head;
	Thread *tail;
} Condition;

typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t readers_go;
	pthread_cond_t writers_go;
	uint32_t readers;
	uint32_t writers_waiting;
	bool writer;
} ReadWriteLock;

static uint32_t self_slot = TLS_OUT_OF_INDEXES;
static void (*key_destructors[KEY_SLOTS])(void *);
static RTL_CRITICAL_SECTION once_lock;
static uint32_t yield_count;
static uint32_t blocking_wait_count;

void xsdk_thread_wait_counts(uint32_t *yields, uint32_t *blocking_waits)
{
	*yields = __atomic_load_n(&yield_count, __ATOMIC_RELAXED);
	*blocking_waits = __atomic_load_n(&blocking_wait_count, __ATOMIC_RELAXED);
}

// libstdc++ reaches this file only through weak references, which never pull an object out of a library.
void xsdk_pthread_init(void)
{
	RtlInitializeCriticalSection(&once_lock);
	self_slot = KeTlsAlloc();
	if (self_slot == TLS_OUT_OF_INDEXES)
		xsdk_log_write("pthread: KeTlsAlloc failed");
}

static Thread *current_thread(void)
{
	Thread *self = KeTlsGetValue(self_slot);
	if (self)
		return self;
	self = calloc(1, sizeof(*self));
	if (self)
		(void)KeTlsSetValue(self_slot, self);
	return self;
}

static HANDLE wake_event(Thread *self)
{
	if (self->wake)
		return self->wake;
	const NTSTATUS status = NtCreateEvent(&self->wake, NULL, SYNCHRONIZATION_EVENT, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("pthread: NtCreateEvent failed: 0x%08X", (unsigned)status);
		self->wake = 0;
	}
	return self->wake;
}

static void release_thread(Thread *thread)
{
	if (thread->handle)
		(void)NtClose(thread->handle);
	if (thread->wake)
		(void)NtClose(thread->wake);
	free(thread);
}

static void run_key_destructors(void)
{
	for (int round = 0; round < DESTRUCTOR_ROUNDS; round++) {
		bool ran = false;
		for (uint32_t slot = 0; slot < KEY_SLOTS; slot++) {
			void (*destructor)(void *) = __atomic_load_n(&key_destructors[slot], __ATOMIC_ACQUIRE);
			void *value = destructor ? KeTlsGetValue(slot) : NULL;
			if (!value)
				continue;
			(void)KeTlsSetValue(slot, NULL);
			destructor(value);
			ran = true;
		}
		if (!ran)
			return;
	}
}

void xsdk_pthread_thread_end(void)
{
	run_key_destructors();
	Thread *self = KeTlsGetValue(self_slot);
	// pthread_create's threads clear the slot themselves; any other thread's record was made on demand.
	if (!self || self->start)
		return;
	(void)KeTlsSetValue(self_slot, NULL);
	release_thread(self);
}

static uint32_t thread_main(void *context)
{
	Thread *self = context;
	(void)KeTlsSetValue(self_slot, self);
	self->result = self->start(self->argument);
	run_key_destructors();
	(void)KeTlsSetValue(self_slot, NULL);
	if (__atomic_fetch_or(&self->state, THREAD_ENDED, __ATOMIC_ACQ_REL) & THREAD_DETACHED)
		release_thread(self);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *argument)
{
	Thread *created = calloc(1, sizeof(*created));
	if (!created)
		return EAGAIN;
	created->start = start;
	created->argument = argument;
	uint32_t stack_bytes = 0;
	bool detached = false;
	if (attr && attr->is_initialized) {
		stack_bytes = (uint32_t)attr->stacksize;
		detached = attr->detachstate == PTHREAD_CREATE_DETACHED;
	}
	created->state = detached ? THREAD_DETACHED : 0;

	HANDLE handle;
	if (!NT_SUCCESS(xsdk_thread_create(&handle, stack_bytes, thread_main, created))) {
		free(created);
		return EAGAIN;
	}
	// A detached thread may already have ended and freed its record, so leave the record alone.
	if (detached)
		(void)NtClose(handle);
	else
		created->handle = handle;
	*thread = (pthread_t)(uintptr_t)created;
	return 0;
}

int pthread_join(pthread_t thread, void **result)
{
	Thread *target = (Thread *)(uintptr_t)thread;
	if (!target || !target->handle)
		return EINVAL;
	if (target == KeTlsGetValue(self_slot))
		return EDEADLK;
	const NTSTATUS status = NtWaitForSingleObjectEx(target->handle, USER_MODE, 0, NULL);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("pthread_join: NtWaitForSingleObjectEx failed: 0x%08X", (unsigned)status);
		return EINVAL;
	}
	if (result)
		*result = target->result;
	release_thread(target);
	return 0;
}

int pthread_detach(pthread_t thread)
{
	Thread *target = (Thread *)(uintptr_t)thread;
	if (!target || !target->start)
		return EINVAL;
	if (__atomic_fetch_or(&target->state, THREAD_DETACHED, __ATOMIC_ACQ_REL) & THREAD_ENDED)
		release_thread(target);
	return 0;
}

pthread_t pthread_self(void)
{
	return (pthread_t)(uintptr_t)current_thread();
}

int pthread_equal(pthread_t first, pthread_t second)
{
	return first == second;
}

// libstdc++ counts threads as available when this exists (gthr-posix.h); cancellation itself isn't supported.
int pthread_cancel(pthread_t thread)
{
	(void)thread;
	return ENOSYS;
}

// newlib's stdio holds cancellation off around its locks; with no cancellation, the state changes nothing.
int pthread_setcancelstate(int state, int *old_state)
{
	if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
		return EINVAL;
	if (old_state)
		*old_state = PTHREAD_CANCEL_ENABLE;
	return 0;
}

int sched_yield(void)
{
	__atomic_fetch_add(&yield_count, 1, __ATOMIC_RELAXED);
	(void)NtYieldExecution();
	return 0;
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
	const uint32_t slot = KeTlsAlloc();
	if (slot == TLS_OUT_OF_INDEXES)
		return EAGAIN;
	if (slot >= KEY_SLOTS) {
		(void)KeTlsFree(slot);
		return EAGAIN;
	}
	__atomic_store_n(&key_destructors[slot], destructor, __ATOMIC_RELEASE);
	*key = slot;
	return 0;
}

int pthread_key_delete(pthread_key_t key)
{
	if (key >= KEY_SLOTS)
		return EINVAL;
	__atomic_store_n(&key_destructors[key], NULL, __ATOMIC_RELEASE);
	return KeTlsFree(key) ? 0 : EINVAL;
}

void *pthread_getspecific(pthread_key_t key)
{
	return KeTlsGetValue(key);
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
	return KeTlsSetValue(key, (void *)value) ? 0 : EINVAL;
}

int pthread_once(pthread_once_t *once, void (*routine)(void))
{
	if (__atomic_load_n(&once->init_executed, __ATOMIC_ACQUIRE) == ONCE_DONE)
		return 0;
	RtlEnterCriticalSection(&once_lock);
	if (once->init_executed != ONCE_DONE) {
		routine();
		__atomic_store_n(&once->init_executed, ONCE_DONE, __ATOMIC_RELEASE);
	}
	RtlLeaveCriticalSection(&once_lock);
	return 0;
}

// Time left until deadline on clock, in 100 ns units.
static int64_t remaining_until(clockid_t clock, const struct timespec *deadline)
{
	struct timespec now;
	if (clock_gettime(clock, &now) != 0)
		return 0;
	return ((int64_t)deadline->tv_sec - now.tv_sec) * 10000000 + (deadline->tv_nsec - now.tv_nsec) / 100;
}

// Mutexes start as PTHREAD_MUTEX_INITIALIZER and get a critical section on first use.
static RTL_CRITICAL_SECTION *mutex_section(pthread_mutex_t *mutex)
{
	uint32_t value = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
	if (value != (uint32_t)PTHREAD_MUTEX_INITIALIZER)
		return (RTL_CRITICAL_SECTION *)(uintptr_t)value;
	RTL_CRITICAL_SECTION *created = malloc(sizeof(*created));
	if (!created)
		return NULL;
	RtlInitializeCriticalSection(created);
	if (__atomic_compare_exchange_n(
			mutex, &value, (uint32_t)(uintptr_t)created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return created;
	free(created);
	return (RTL_CRITICAL_SECTION *)(uintptr_t)value;
}

// Every type is recursive, as critical sections are; relocking a normal mutex is undefined anyway.
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	(void)attr;
	*mutex = PTHREAD_MUTEX_INITIALIZER;
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	if (*mutex != (uint32_t)PTHREAD_MUTEX_INITIALIZER)
		free((void *)(uintptr_t)*mutex);
	*mutex = PTHREAD_MUTEX_INITIALIZER;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	RTL_CRITICAL_SECTION *section = mutex_section(mutex);
	if (!section)
		return ENOMEM;
	RtlEnterCriticalSection(section);
	return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	RTL_CRITICAL_SECTION *section = mutex_section(mutex);
	if (!section)
		return ENOMEM;
	return RtlTryEnterCriticalSection(section) ? 0 : EBUSY;
}

int pthread_mutex_clocklock(pthread_mutex_t *mutex, clockid_t clock, const struct timespec *deadline)
{
	RTL_CRITICAL_SECTION *section = mutex_section(mutex);
	if (!section)
		return ENOMEM;
	// Critical sections have no timed wait, so poll.
	while (!RtlTryEnterCriticalSection(section)) {
		if (remaining_until(clock, deadline) <= 0)
			return ETIMEDOUT;
		(void)KeDelayExecutionThread(USER_MODE, 0, &POLL_INTERVAL);
	}
	return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline)
{
	return pthread_mutex_clocklock(mutex, CLOCK_REALTIME, deadline);
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	const uint32_t value = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
	if (value == (uint32_t)PTHREAD_MUTEX_INITIALIZER)
		return EPERM;
	RtlLeaveCriticalSection((RTL_CRITICAL_SECTION *)(uintptr_t)value);
	return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	attr->is_initialized = 1;
	attr->type = PTHREAD_MUTEX_DEFAULT;
	attr->recursive = 0;
	return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
	if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_DEFAULT)
		return EINVAL;
	attr->type = type;
	attr->recursive = type == PTHREAD_MUTEX_RECURSIVE;
	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
	attr->is_initialized = 0;
	return 0;
}

// Condition variables start as PTHREAD_COND_INITIALIZER and get their queue on first use.
static Condition *condition_object(pthread_cond_t *cond)
{
	uint32_t value = __atomic_load_n(cond, __ATOMIC_ACQUIRE);
	if (value != (uint32_t)PTHREAD_COND_INITIALIZER)
		return (Condition *)(uintptr_t)value;
	Condition *created = calloc(1, sizeof(*created));
	if (!created)
		return NULL;
	RtlInitializeCriticalSection(&created->lock);
	if (__atomic_compare_exchange_n(
			cond, &value, (uint32_t)(uintptr_t)created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return created;
	free(created);
	return (Condition *)(uintptr_t)value;
}

static bool remove_waiter(Condition *condition, Thread *waiter)
{
	Thread *previous = NULL;
	for (Thread *thread = condition->head; thread; previous = thread, thread = thread->next_waiter) {
		if (thread != waiter)
			continue;
		if (previous)
			previous->next_waiter = thread->next_waiter;
		else
			condition->head = thread->next_waiter;
		if (condition->tail == thread)
			condition->tail = previous;
		return true;
	}
	return false;
}

// Each waiter queues itself and sleeps on its own event, so a signal never wakes a thread that started waiting later.
static int condition_wait(
	pthread_cond_t *cond, pthread_mutex_t *mutex, clockid_t clock, const struct timespec *deadline)
{
	Condition *condition = condition_object(cond);
	Thread *self = current_thread();
	if (!condition || !self || !wake_event(self))
		return ENOMEM;
	int64_t timeout_storage = 0;
	const int64_t *timeout = NULL;
	if (deadline) {
		const int64_t remaining = remaining_until(clock, deadline);
		if (remaining <= 0)
			return ETIMEDOUT;
		timeout_storage = -remaining;
		timeout = &timeout_storage;
	}
	RtlEnterCriticalSection(&condition->lock);
	self->next_waiter = NULL;
	if (condition->tail)
		condition->tail->next_waiter = self;
	else
		condition->head = self;
	condition->tail = self;
	RtlLeaveCriticalSection(&condition->lock);

	(void)pthread_mutex_unlock(mutex);
	__atomic_fetch_add(&blocking_wait_count, 1, __ATOMIC_RELAXED);
	int result = 0;
	if (NtWaitForSingleObjectEx(self->wake, USER_MODE, 0, timeout) == STATUS_TIMEOUT) {
		RtlEnterCriticalSection(&condition->lock);
		const bool removed = remove_waiter(condition, self);
		RtlLeaveCriticalSection(&condition->lock);
		if (removed)
			result = ETIMEDOUT;
		else
			// A signal dequeued this thread as the wait timed out; take its wake-up so no later wait sees it.
			(void)NtWaitForSingleObjectEx(self->wake, USER_MODE, 0, NULL);
	}
	(void)pthread_mutex_lock(mutex);
	return result;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	(void)attr;
	*cond = PTHREAD_COND_INITIALIZER;
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	if (*cond != (uint32_t)PTHREAD_COND_INITIALIZER)
		free((void *)(uintptr_t)*cond);
	*cond = PTHREAD_COND_INITIALIZER;
	return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	return condition_wait(cond, mutex, CLOCK_REALTIME, NULL);
}

int pthread_cond_clockwait(
	pthread_cond_t *cond, pthread_mutex_t *mutex, clockid_t clock, const struct timespec *deadline)
{
	return condition_wait(cond, mutex, clock, deadline);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline)
{
	return condition_wait(cond, mutex, CLOCK_REALTIME, deadline);
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	Condition *condition = condition_object(cond);
	if (!condition)
		return ENOMEM;
	RtlEnterCriticalSection(&condition->lock);
	Thread *waiter = condition->head;
	if (waiter) {
		condition->head = waiter->next_waiter;
		if (!condition->head)
			condition->tail = NULL;
	}
	RtlLeaveCriticalSection(&condition->lock);
	if (waiter)
		(void)NtSetEvent(waiter->wake, NULL);
	return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
	Condition *condition = condition_object(cond);
	if (!condition)
		return ENOMEM;
	RtlEnterCriticalSection(&condition->lock);
	Thread *waiter = condition->head;
	condition->head = NULL;
	condition->tail = NULL;
	RtlLeaveCriticalSection(&condition->lock);
	while (waiter) {
		// Once woken, a waiter may queue again and change next_waiter.
		Thread *next = waiter->next_waiter;
		(void)NtSetEvent(waiter->wake, NULL);
		waiter = next;
	}
	return 0;
}

// Reader/writer locks start as PTHREAD_RWLOCK_INITIALIZER; writers waiting hold off new readers.
static ReadWriteLock *rwlock_object(pthread_rwlock_t *rwlock)
{
	uint32_t value = __atomic_load_n(rwlock, __ATOMIC_ACQUIRE);
	if (value != (uint32_t)PTHREAD_RWLOCK_INITIALIZER)
		return (ReadWriteLock *)(uintptr_t)value;
	ReadWriteLock *created = calloc(1, sizeof(*created));
	if (!created)
		return NULL;
	created->mutex = PTHREAD_MUTEX_INITIALIZER;
	created->readers_go = PTHREAD_COND_INITIALIZER;
	created->writers_go = PTHREAD_COND_INITIALIZER;
	if (__atomic_compare_exchange_n(
			rwlock, &value, (uint32_t)(uintptr_t)created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return created;
	free(created);
	return (ReadWriteLock *)(uintptr_t)value;
}

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr)
{
	(void)attr;
	*rwlock = PTHREAD_RWLOCK_INITIALIZER;
	return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
	if (*rwlock != (uint32_t)PTHREAD_RWLOCK_INITIALIZER) {
		ReadWriteLock *lock = (ReadWriteLock *)(uintptr_t)*rwlock;
		(void)pthread_cond_destroy(&lock->readers_go);
		(void)pthread_cond_destroy(&lock->writers_go);
		(void)pthread_mutex_destroy(&lock->mutex);
		free(lock);
	}
	*rwlock = PTHREAD_RWLOCK_INITIALIZER;
	return 0;
}

// A null deadline waits for ever.
static int read_lock(pthread_rwlock_t *rwlock, clockid_t clock, const struct timespec *deadline)
{
	ReadWriteLock *lock = rwlock_object(rwlock);
	if (!lock)
		return ENOMEM;
	(void)pthread_mutex_lock(&lock->mutex);
	int result = 0;
	while (!result && (lock->writer || lock->writers_waiting))
		result = condition_wait(&lock->readers_go, &lock->mutex, clock, deadline);
	if (!result)
		lock->readers++;
	(void)pthread_mutex_unlock(&lock->mutex);
	return result;
}

static int write_lock(pthread_rwlock_t *rwlock, clockid_t clock, const struct timespec *deadline)
{
	ReadWriteLock *lock = rwlock_object(rwlock);
	if (!lock)
		return ENOMEM;
	(void)pthread_mutex_lock(&lock->mutex);
	lock->writers_waiting++;
	int result = 0;
	while (!result && (lock->writer || lock->readers))
		result = condition_wait(&lock->writers_go, &lock->mutex, clock, deadline);
	lock->writers_waiting--;
	if (!result)
		lock->writer = true;
	else if (!lock->writer && !lock->writers_waiting)
		// Readers held off for this writer may go now.
		(void)pthread_cond_broadcast(&lock->readers_go);
	(void)pthread_mutex_unlock(&lock->mutex);
	return result;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
	return read_lock(rwlock, CLOCK_REALTIME, NULL);
}

int pthread_rwlock_clockrdlock(pthread_rwlock_t *rwlock, clockid_t clock, const struct timespec *deadline)
{
	return read_lock(rwlock, clock, deadline);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
	ReadWriteLock *lock = rwlock_object(rwlock);
	if (!lock)
		return ENOMEM;
	(void)pthread_mutex_lock(&lock->mutex);
	const bool free_to_read = !lock->writer && !lock->writers_waiting;
	if (free_to_read)
		lock->readers++;
	(void)pthread_mutex_unlock(&lock->mutex);
	return free_to_read ? 0 : EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
	return write_lock(rwlock, CLOCK_REALTIME, NULL);
}

int pthread_rwlock_clockwrlock(pthread_rwlock_t *rwlock, clockid_t clock, const struct timespec *deadline)
{
	return write_lock(rwlock, clock, deadline);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
	ReadWriteLock *lock = rwlock_object(rwlock);
	if (!lock)
		return ENOMEM;
	(void)pthread_mutex_lock(&lock->mutex);
	const bool free_to_write = !lock->writer && !lock->readers;
	if (free_to_write)
		lock->writer = true;
	(void)pthread_mutex_unlock(&lock->mutex);
	return free_to_write ? 0 : EBUSY;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
	ReadWriteLock *lock = rwlock_object(rwlock);
	if (!lock)
		return ENOMEM;
	(void)pthread_mutex_lock(&lock->mutex);
	if (lock->writer)
		lock->writer = false;
	else if (lock->readers)
		lock->readers--;
	if (!lock->readers && lock->writers_waiting)
		(void)pthread_cond_signal(&lock->writers_go);
	else if (!lock->writers_waiting)
		(void)pthread_cond_broadcast(&lock->readers_go);
	(void)pthread_mutex_unlock(&lock->mutex);
	return 0;
}
