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
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/post_job.h"
#include "base/task/task_traits.h"
#include "base/time/time.h"

/* Public library header */
#include <pthreadpool.h>

/* Internal library headers */
#include "threadpool-atomics.h"
#include "threadpool-object.h"
#include "threadpool-utils.h"

static void wait_all_work_done(struct pthreadpool* threadpool) {
	/* Initial check */
	size_t num_incomplete_work_items = static_cast<std::atomic_size_t*>(threadpool->num_incomplete_work_items)->load(std::memory_order_acquire);
	if (num_incomplete_work_items == 0) {
		return;
	}

	/* Spin-wait */
	for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
		pthreadpool_yield();

		num_incomplete_work_items = static_cast<std::atomic_size_t*>(threadpool->num_incomplete_work_items)->load(std::memory_order_acquire);
		if (num_incomplete_work_items == 0) {
			return;
		}
	}

	/* Fall-back to event wait */
	static_cast<base::WaitableEvent*>(threadpool->completion_event)->Wait();
}

static void do_work(struct pthreadpool* threadpool, size_t work_index) {
	struct thread_info* thread = &threadpool->threads[work_index];
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
static size_t num_incomplete_work_items(struct pthreadpool* threadpool, size_t worker_count) {
	return static_cast<std::atomic_size_t*>(threadpool->num_incomplete_work_items)->load(std::memory_order_acquire);
}

static size_t get_work_index(struct pthreadpool* threadpool) {
	return static_cast<std::atomic_size_t*>(threadpool->work_index)->fetch_add(1, std::memory_order_relaxed);
}

// Return true if all threads finish.
static bool work_is_done(struct pthreadpool* threadpool) {
	if (static_cast<std::atomic_size_t*>(threadpool->num_incomplete_work_items)->fetch_sub(1, std::memory_order_acq_rel) <= 1) {
		static_cast<base::WaitableEvent*>(threadpool->completion_event)->Signal();
		return true;
	}
	return false;
}

static void thread_main(struct pthreadpool* threadpool, base::JobDelegate* job_delegate) {
	while (num_incomplete_work_items(threadpool, 0) != 0 && !job_delegate->ShouldYield()) {
		const size_t work_index = get_work_index(threadpool);
		if (work_index >= threadpool->threads_count.value) {
			return;
		}

		do_work(threadpool, work_index);

		if (work_is_done(threadpool)) {
			return;
		}
	}
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
		threadpool->execution_mutex = new base::Lock();
		threadpool->completion_event = new base::WaitableEvent();
		threadpool->work_index = new std::atomic_size_t(0);
		threadpool->num_incomplete_work_items = new std::atomic_size_t(0);
		threadpool->job_handle = new base::JobHandle(base::CreateJob(
				FROM_HERE, {},
				base::BindRepeating(&thread_main, threadpool),
				base::BindRepeating(&num_incomplete_work_items, threadpool)));
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
	static_cast<base::Lock*>(threadpool->execution_mutex)->Acquire();

	/* Setup global arguments */
	pthreadpool_store_relaxed_void_p(&threadpool->thread_function, (void*) thread_function);
	pthreadpool_store_relaxed_void_p(&threadpool->task, task);
	pthreadpool_store_relaxed_void_p(&threadpool->argument, context);
	pthreadpool_store_relaxed_uint32_t(&threadpool->flags, flags);

	const struct fxdiv_divisor_size_t threads_count = threadpool->threads_count;
	/* Caller thread serves as worker #0. Thus, other workers start #1. */
	static_cast<std::atomic_size_t*>(threadpool->work_index)->store(1);
	static_cast<std::atomic_size_t*>(threadpool->num_incomplete_work_items)->store(threads_count.value - 1);

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

	if (threads_count.value == 1) {
		/* Do computations as worker #0 */
		do_work(threadpool, 0);
	} else {
		// Notifies the scheduler to adjust the number of workers.
		static_cast<base::JobHandle*>(threadpool->job_handle)->NotifyConcurrencyIncrease();

		/* Do computations as worker #0 */
		do_work(threadpool, 0);
		
		/*
		* Wait until the threads finish computation
		*/
		wait_all_work_done(threadpool);

		/*
		* Reset the completion event for the next command.
		*/
		static_cast<base::WaitableEvent*>(threadpool->completion_event)->Reset();
	}

	/* Unprotect the global threadpool structures */
	static_cast<base::Lock*>(threadpool->execution_mutex)->Release();
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	if (threadpool != NULL) {
		const size_t threads_count = threadpool->threads_count.value;
		if (threads_count > 1) {
			static_cast<base::JobHandle*>(threadpool->job_handle)->Cancel();
			delete static_cast<base::JobHandle*>(threadpool->job_handle);	

			/* Release resources */
			delete static_cast<base::Lock*>(threadpool->execution_mutex);
			delete static_cast<std::atomic_size_t*>(threadpool->work_index);
			delete static_cast<std::atomic_size_t*>(threadpool->num_incomplete_work_items);
			delete static_cast<base::WaitableEvent*>(threadpool->completion_event);
		}
		pthreadpool_deallocate(threadpool);
	}
}
