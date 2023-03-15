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

static void WaitEvent(void* event) {
	static_cast<base::WaitableEvent*>(event)->Wait();
}

static bool TimedWaitEvent(void* event, size_t ms) {
	return static_cast<base::WaitableEvent*>(event)->TimedWait(base::Milliseconds(ms));
}

static void ResetEvent(void* event) {
	static_cast<base::WaitableEvent*>(event)->Reset();
}

static void SetEvent(void* event) {
	static_cast<base::WaitableEvent*>(event)->Signal();
}

static void checkin_worker_thread(struct pthreadpool* threadpool, uint32_t event_index) {
	if (pthreadpool_decrement_fetch_acquire_release_size_t(&threadpool->active_threads) == 0) {
		SetEvent(threadpool->completion_event[event_index]);
	}
}

static void wait_worker_threads(struct pthreadpool* threadpool, uint32_t event_index) {
	/* Initial check */
	size_t active_threads = pthreadpool_load_acquire_size_t(&threadpool->active_threads);
	if (active_threads == 0) {
		return;
	}

	/* Spin-wait */
	for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
		pthreadpool_yield();

		active_threads = pthreadpool_load_acquire_size_t(&threadpool->active_threads);
		if (active_threads == 0) {
			return;
		}
	}

	/* Fall-back to event wait */
	WaitEvent(threadpool->completion_event[event_index]);
	assert(pthreadpool_load_relaxed_size_t(&threadpool->active_threads) == 0);
}

static uint32_t wait_for_new_command(
	struct pthreadpool* threadpool,
	uint32_t last_command,
	uint32_t last_flags)
{
	uint32_t command = pthreadpool_load_acquire_uint32_t(&threadpool->command);
	if (command != last_command) {
		return command;
	}

	if ((last_flags & PTHREADPOOL_FLAG_YIELD_WORKERS) == 0) {
		/* Spin-wait loop */
		for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
			pthreadpool_yield();

			command = pthreadpool_load_acquire_uint32_t(&threadpool->command);
			if (command != last_command) {
				return command;
			}
		}
	}

	/* Spin-wait disabled or timed out, fall back to event wait */
	const uint32_t event_index = (last_command >> 31);
	if (!TimedWaitEvent(threadpool->command_event[event_index], 100 /* ms */)) {
		// Shutdown this worker if no more commands come in 100 ms.
		return threadpool_command_shutdown;
	}

	command = pthreadpool_load_relaxed_uint32_t(&threadpool->command);
	assert(command != last_command);
	return command;
}

static void do_job(struct pthreadpool* threadpool, size_t thread_index) {
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

static void thread_main(struct pthreadpool* threadpool, base::JobDelegate* job_delegate) {
	// Worker thread index should be in the range [1, threads_count - 1].
	const size_t thread_index = job_delegate->GetTaskId() + 1;
	if (thread_index < 1 || thread_index > threadpool->threads_count.value - 1) {
		return;
	}
	uint32_t last_command = threadpool_command_init;
	struct fpu_state saved_fpu_state = { 0 };
	uint32_t flags = 0;

	/* Monitor new commands and act accordingly */
	while (!job_delegate->ShouldYield()) {
		uint32_t command = wait_for_new_command(threadpool, last_command, flags);
		pthreadpool_fence_acquire();

		/* Process command */
		switch (command & THREADPOOL_COMMAND_MASK) {
			case threadpool_command_parallelize:
			{
				do_job(threadpool, thread_index);
				break;
			}
			case threadpool_command_shutdown:
				/* Exit immediately: the master thread is waiting on pthread_join */
				return;
			case threadpool_command_init:
				/* To inhibit compiler warning */
				break;
		}
		/* Notify the master thread that we finished processing */
		const uint32_t event_index = command >> 31;
		checkin_worker_thread(threadpool, event_index);
		/* Update last command */
		last_command = command;
	};
}

size_t max_concurrent_threads(struct pthreadpool* threadpool, size_t worker_count) {
	if (pthreadpool_load_acquire_uint32_t(&threadpool->command) == threadpool_command_shutdown) {
		return 0;
	} else {
		return pthreadpool_load_relaxed_size_t(&threadpool->active_threads);
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
		threadpool->execution_mutex = base_mutex_create();
		for (size_t i = 0; i < 2; i++) {
			threadpool->completion_event[i] = new base::WaitableEvent();
			threadpool->command_event[i] = new base::WaitableEvent();
		}
		pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);
		threadpool->job_handle = new base::JobHandle(base::CreateJob(
				FROM_HERE, {base::WithBaseSyncPrimitives()},
				base::BindRepeating(&thread_main, threadpool),
				base::BindRepeating(&max_concurrent_threads, threadpool)));
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

	const struct fxdiv_divisor_size_t threads_count = threadpool->threads_count;
	pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count.value - 1 /* caller thread */);

	if (params_size != 0) {
		memcpy(&threadpool->params, params, params_size);
		pthreadpool_fence_release();
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
		do_job(threadpool, 0);
	} else {
		/*
		* Update the threadpool command.
		* Imporantly, do it after initializing command parameters (range, task, argument, flags)
		* ~(threadpool->command | THREADPOOL_COMMAND_MASK) flips the bits not in command mask
		* to ensure the unmasked command is different then the last command, because worker threads
		* monitor for change in the unmasked command.
		*/
		const uint32_t old_command = pthreadpool_load_relaxed_uint32_t(&threadpool->command);
		const uint32_t new_command = ~(old_command | THREADPOOL_COMMAND_MASK) | threadpool_command_parallelize;

		/*
		* Reset the command event for the next command.
		* It is important to reset the event before writing out the new command, because as soon as the worker threads
		* observe the new command, they may process it and switch to waiting on the next command event.
		*
		* Note: the event is different from the command event signalled in this update.
		*/
		const uint32_t event_index = (old_command >> 31);
		ResetEvent(threadpool->command_event[event_index ^ 1]);

		/*
		* Store the command with release semantics to guarantee that if a worker thread observes
		* the new command value, it also observes the updated command parameters.
		*
		* Note: release semantics is necessary, because the workers might be waiting in a spin-loop
		* rather than on the event object.
		*/
		pthreadpool_store_release_uint32_t(&threadpool->command, new_command);

		/*
		* Signal the event to wake up the threads.
		* Event in use must be switched after every submitted command to avoid race conditions.
		* Choose the event based on the high bit of the command, which is flipped on every update.
		*/
		SetEvent(threadpool->command_event[event_index]);

		// Notifies the scheduler to adjust the number of workers.
		static_cast<base::JobHandle*>(threadpool->job_handle)->NotifyConcurrencyIncrease();

		/* Do computations as worker #0 */
		do_job(threadpool, 0);
		
		/*
		* Wait until the threads finish computation
		* Use the complementary event because it corresponds to the new command.
		*/
		wait_worker_threads(threadpool, event_index ^ 1);

		/*
		* Reset the completion event for the next command.
		* Note: the event is different from the one used for waiting in this update.
		*/
		ResetEvent(threadpool->completion_event[event_index]);

		/* Make changes by other threads visible to this thread */
		pthreadpool_fence_acquire();
	}

	/* Unprotect the global threadpool structures */
	base_mutex_unlock(threadpool->execution_mutex);
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	if (threadpool != NULL) {
		const size_t threads_count = threadpool->threads_count.value;
		if (threads_count > 1) {
			pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);

			/*
			 * Store the command with release semantics to guarantee that if a worker thread observes
			 * the new command value, it also observes the updated active_threads values.
			 */
			const uint32_t old_command = pthreadpool_load_relaxed_uint32_t(&threadpool->command);
			pthreadpool_store_release_uint32_t(&threadpool->command, threadpool_command_shutdown);

			/*
			 * Signal the event to wake up the threads.
			 * Event in use must be switched after every submitted command to avoid race conditions.
			 * Choose the event based on the high bit of the command, which is flipped on every update.
			 */
			const uint32_t event_index = (old_command >> 31);
			SetEvent(threadpool->command_event[event_index]);

			static_cast<base::JobHandle*>(threadpool->job_handle)->Join();
			delete static_cast<base::JobHandle*>(threadpool->job_handle);	

			/* Release resources */
			base_mutex_destroy(threadpool->execution_mutex);
			for (size_t i = 0; i < 2; i++) {
				delete static_cast<base::WaitableEvent*>(threadpool->command_event[i]);
				delete static_cast<base::WaitableEvent*>(threadpool->completion_event[i]);
			}
		}
		pthreadpool_deallocate(threadpool);
	}
}
