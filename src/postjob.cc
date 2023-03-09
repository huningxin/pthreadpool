/* Standard C headers */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Configuration header */
#include "threadpool-common.h"

// Chromium header
#include "base/functional/bind.h"
#include "base/system/sys_info.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/post_job.h"
#include "base/task/task_traits.h"

/* Public library header */
#include <pthreadpool.h>

/* Internal library headers */
#include "threadpool-atomics.h"
#include "threadpool-object.h"
#include "threadpool-utils.h"

static void* base_mutex_create() {
	return new base::Lock();
}

static void base_mutex_lock(void* lock) {
	static_cast<base::Lock*>(lock)->Acquire();
}

static void base_mutex_unlock(void* lock) {
	static_cast<base::Lock*>(lock)->Release();
}

static void base_mutex_destroy(void* lock) {
	delete static_cast<base::Lock*>(lock);
}

static void* base_cond_create(void* lock) {
       return new base::ConditionVariable(static_cast<base::Lock*>(lock));
}

static void base_cond_signal(void* cond) {
       static_cast<base::ConditionVariable*>(cond)->Signal();
}

static void base_cond_wait(void* cond) {
       static_cast<base::ConditionVariable*>(cond)->Wait();
}

static void base_cond_destroy(void* lock) {
       delete static_cast<base::Lock*>(lock);
}


static void thread_main(struct pthreadpool* threadpool, base::JobDelegate* job_delegate) {
	while (!job_delegate->ShouldYield()) {
		base_mutex_lock(threadpool->completion_mutex);
		if (pthreadpool_load_acquire_size_t(&threadpool->active_threads) == 0 ) {
			base_cond_signal(threadpool->completion_condvar);
			base_mutex_unlock(threadpool->completion_mutex);
			return;
		}
		size_t thread_index = pthreadpool_decrement_fetch_release_size_t(&threadpool->active_threads);
		base_mutex_unlock(threadpool->completion_mutex);
		struct thread_info* thread = &threadpool->threads[thread_index];

		const uint32_t flags = pthreadpool_load_relaxed_uint32_t(&threadpool->flags);
		const thread_function_t thread_function =
			(thread_function_t) pthreadpool_load_relaxed_void_p(&threadpool->thread_function);

		struct fpu_state saved_fpu_state = { 0 };
		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			saved_fpu_state = get_fpu_state();
			disable_fpu_denormals();
		}

		thread_function(threadpool, thread);

		if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
			set_fpu_state(saved_fpu_state);
		}
	}
}

size_t max_concurrent_threads(struct pthreadpool* threadpool, size_t /*worker_count*/) {
	return pthreadpool_load_acquire_size_t(&threadpool->active_threads);
}

struct pthreadpool* pthreadpool_create(size_t threads_count) {
	if (threads_count == 0) {
		threads_count = base::SysInfo::NumberOfProcessors();
		if (threads_count == 0) {
			return NULL;
		}
	}

	struct pthreadpool* threadpool = pthreadpool_allocate(threads_count);
	if (threadpool == NULL) {
		return NULL;
	}
	threadpool->threads_count = fxdiv_init_size_t(threads_count);
	for (size_t tid = 0; tid < threads_count; tid++) {
		threadpool->threads[tid].thread_number = tid;
		threadpool->threads[tid].threadpool = threadpool;
	}

	/* Thread pool with a single thread computes everything on the caller thread. */
	if (threads_count > 1) {
		threadpool->execution_mutex = base_mutex_create();
		threadpool->completion_mutex = base_mutex_create();
		threadpool->completion_condvar = base_cond_create(threadpool->completion_mutex);
		pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count);
	}
	return threadpool;
}

PTHREADPOOL_INTERNAL void pthreadpool_parallelize(
	struct pthreadpool* threadpool,
	thread_function_t thread_function,
	const void* params,
	size_t params_size,
	void* task,
	void* context,
	size_t linear_range,
	uint32_t flags)
{
	assert(threadpool != NULL);
	assert(thread_function != NULL);
	assert(task != NULL);
	assert(linear_range > 1);

	/* Protect the global threadpool structures */
	base_mutex_lock(threadpool->execution_mutex);

	/* Setup global arguments */
	pthreadpool_store_relaxed_void_p(&threadpool->thread_function, (void*) thread_function);
	pthreadpool_store_relaxed_void_p(&threadpool->task, task);
	pthreadpool_store_relaxed_void_p(&threadpool->argument, context);
	pthreadpool_store_relaxed_uint32_t(&threadpool->flags, flags);

	/* Locking of completion_mutex not needed: readers are sleeping on command_condvar */
	const struct fxdiv_divisor_size_t threads_count = threadpool->threads_count;
	pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count.value);

	if (params_size != 0) {
		memcpy(&threadpool->params, params, params_size);
	}

	/* Spread the work between threads */
	const struct fxdiv_result_size_t range_params = fxdiv_divide_size_t(linear_range, threads_count);
	size_t range_start = 0;
	for (size_t tid = 0; tid < threads_count.value; tid++) {
		struct thread_info* thread = &threadpool->threads[tid];
		const size_t range_length = range_params.quotient + (size_t) (tid < range_params.remainder);
		const size_t range_end = range_start + range_length;
		pthreadpool_store_relaxed_size_t(&thread->range_start, range_start);
		pthreadpool_store_relaxed_size_t(&thread->range_end, range_end);
		pthreadpool_store_relaxed_size_t(&thread->range_length, range_length);

		/* The next subrange starts where the previous ended */
		range_start = range_end;
	}

	auto job_handle = base::PostJob(
		FROM_HERE,
		{base::WithBaseSyncPrimitives()},
		base::BindRepeating(&thread_main, threadpool),
		base::BindRepeating(&max_concurrent_threads, threadpool));

	base_mutex_lock(threadpool->completion_mutex);
	while (pthreadpool_load_acquire_size_t(&threadpool->active_threads) != 0) {
		base_cond_wait(threadpool->completion_condvar);
	};
	base_mutex_unlock(threadpool->completion_mutex);

	job_handle.Join();

	/* Unprotect the global threadpool structures */
	base_mutex_unlock(threadpool->execution_mutex);
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	if (threadpool != NULL) {
		const size_t threads_count = threadpool->threads_count.value;
		if (threads_count > 1) {
			/* Release resources */
			base_mutex_destroy(threadpool->execution_mutex);
			base_mutex_destroy(threadpool->completion_mutex);
            base_cond_destroy(threadpool->completion_condvar);
		}
		pthreadpool_deallocate(threadpool);
	}
}
