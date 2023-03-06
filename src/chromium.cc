/* Standard C headers */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Configuration header */
#include "threadpool-common.h"

// TODO: Include chromium header
#include "base/synchronization/lock.h"
#include "base/synchronization/condition_variable.h"

/* Public library header */
#include <pthreadpool.h>

/* Internal library headers */
#include "threadpool-atomics.h"
#include "threadpool-object.h"
#include "threadpool-utils.h"

static void* chromium_mutex_create() {
	return new base::Lock();
}

static void chromium_mutex_lock(void* lock) {
	static_cast<base::Lock*>(lock)->Acquire();
}

static void chromium_mutex_unlock(void* lock) {
	static_cast<base::Lock*>(lock)->Release();
}

static void chromium_mutex_destroy(void* lock) {
	delete static_cast<base::Lock*>(lock);
}

static void* chromium_cond_create(void* lock) {
	return new base::ConditionVariable(static_cast<base::Lock*>(lock));
}

static void chromium_cond_signal(void* cond) {
	static_cast<base::ConditionVariable*>(cond)->Signal();
}

static void chromium_cond_broadcast(void* cond) {
	static_cast<base::ConditionVariable*>(cond)->Broadcast();
}

static void chromium_cond_wait(void* cond) {
	static_cast<base::ConditionVariable*>(cond)->Wait();
}

static void chromium_cond_destroy(void* lock) {
	delete static_cast<base::Lock*>(lock);
}

static void checkin_worker_thread(struct pthreadpool* threadpool) {
	chromium_mutex_lock(threadpool->completion_mutex);
	if (pthreadpool_decrement_fetch_release_size_t(&threadpool->active_threads) == 0) {
		chromium_cond_signal(threadpool->completion_condvar);
	}
	chromium_mutex_unlock(threadpool->completion_mutex);
}

static void wait_worker_threads(struct pthreadpool* threadpool) {
	/* Initial check */
	size_t active_threads = pthreadpool_load_acquire_size_t(&threadpool->active_threads);
	if (active_threads == 0) {
		return;
	}

	/* Spin-wait */
	for (uint32_t i = PTHREADPOOL_SPIN_WAIT_ITERATIONS; i != 0; i--) {
		// TODO: Call base::JobDelegate::ShouldYield()
		pthreadpool_yield();

		active_threads = pthreadpool_load_acquire_size_t(&threadpool->active_threads);
		if (active_threads == 0) {
			return;
		}
	}

	/* Fall-back to mutex/futex wait */
	chromium_mutex_lock(threadpool->completion_mutex);
	while (pthreadpool_load_acquire_size_t(&threadpool->active_threads) != 0) {
		chromium_cond_wait(threadpool->completion_condvar);
	};
	chromium_mutex_unlock(threadpool->completion_mutex);
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
			// TODO: Call base::JobDelegate::ShouldYield()
			pthreadpool_yield();

			command = pthreadpool_load_acquire_uint32_t(&threadpool->command);
			if (command != last_command) {
				return command;
			}
		}
	}

	/* Spin-wait disabled or timed out, fall back to mutex/futex wait */
	/* Lock the command mutex */
	chromium_mutex_lock(threadpool->command_mutex);
	/* Read the command */
	while ((command = pthreadpool_load_acquire_uint32_t(&threadpool->command)) == last_command) {
		/* Wait for new command */
		chromium_cond_wait(threadpool->command_condvar);
	}
	/* Read a new command */
	chromium_mutex_unlock(threadpool->command_mutex);
	return command;
}

static void* thread_main(void* arg) {
	struct thread_info* thread = (struct thread_info*) arg;
	struct pthreadpool* threadpool = thread->threadpool;
	uint32_t last_command = threadpool_command_init;
	struct fpu_state saved_fpu_state = { 0 };
	uint32_t flags = 0;

	/* Check in */
	checkin_worker_thread(threadpool);

	/* Monitor new commands and act accordingly */
	for (;;) {
		uint32_t command = wait_for_new_command(threadpool, last_command, flags);
		pthreadpool_fence_acquire();

		flags = pthreadpool_load_relaxed_uint32_t(&threadpool->flags);

		/* Process command */
		switch (command & THREADPOOL_COMMAND_MASK) {
			case threadpool_command_parallelize:
			{
				const thread_function_t thread_function =
					(thread_function_t) pthreadpool_load_relaxed_void_p(&threadpool->thread_function);
				if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
					saved_fpu_state = get_fpu_state();
					disable_fpu_denormals();
				}

				thread_function(threadpool, thread);
				if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
					set_fpu_state(saved_fpu_state);
				}
				break;
			}
			case threadpool_command_shutdown:
				/* Exit immediately: the master thread is waiting on pthread_join */
				return NULL;
			case threadpool_command_init:
				/* To inhibit compiler warning */
				break;
		}
		/* Notify the master thread that we finished processing */
		checkin_worker_thread(threadpool);
		/* Update last command */
		last_command = command;
	};
}

struct pthreadpool* pthreadpool_create(size_t threads_count) {
	if (threads_count == 0) {
		// TODO: use base::SysInfo
		threads_count = 1;
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
		threadpool->execution_mutex = chromium_mutex_create();
		threadpool->completion_mutex = chromium_mutex_create();
		threadpool->completion_condvar = chromium_cond_create(threadpool->completion_mutex);
		threadpool->command_mutex = chromium_mutex_create();
		threadpool->command_condvar = chromium_cond_create(threadpool->command_mutex);

		pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);

		/* Caller thread serves as worker #0. Thus, we create system threads starting with worker #1. */
		// TODO: Call base::PostJob
		// for (size_t tid = 1; tid < threads_count; tid++) {
		// 	pthread_create(&threadpool->threads[tid].thread_object, NULL, &thread_main, &threadpool->threads[tid]);
		// }
		thread_main(nullptr);

		/* Wait until all threads initialize */
		wait_worker_threads(threadpool);
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
	chromium_mutex_lock(threadpool->execution_mutex);

	/* Lock the command variables to ensure that threads don't start processing before they observe complete command with all arguments */
	chromium_mutex_lock(threadpool->command_mutex);

	/* Setup global arguments */
	pthreadpool_store_relaxed_void_p(&threadpool->thread_function, (void*) thread_function);
	pthreadpool_store_relaxed_void_p(&threadpool->task, task);
	pthreadpool_store_relaxed_void_p(&threadpool->argument, context);
	pthreadpool_store_relaxed_uint32_t(&threadpool->flags, flags);

	/* Locking of completion_mutex not needed: readers are sleeping on command_condvar */
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
	 * Store the command with release semantics to guarantee that if a worker thread observes
	 * the new command value, it also observes the updated command parameters.
	 *
	 * Note: release semantics is necessary even with a conditional variable, because the workers might
	 * be waiting in a spin-loop rather than the conditional variable.
	 */
	pthreadpool_store_release_uint32_t(&threadpool->command, new_command);

	/* Unlock the command variables before waking up the threads for better performance */
	// TODO: Call base::Lock
	chromium_mutex_unlock(threadpool->command_mutex);
	// pthread_mutex_unlock(&threadpool->command_mutex);

	/* Wake up the threads */
	// TODO: Call base::ConditionVariable
	chromium_cond_broadcast(threadpool->command_condvar);
	// pthread_cond_broadcast(&threadpool->command_condvar);

	/* Save and modify FPU denormals control, if needed */
	struct fpu_state saved_fpu_state = { 0 };
	if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
		saved_fpu_state = get_fpu_state();
		disable_fpu_denormals();
	}

	/* Do computations as worker #0 */
	thread_function(threadpool, &threadpool->threads[0]);

	/* Restore FPU denormals control, if needed */
	if (flags & PTHREADPOOL_FLAG_DISABLE_DENORMALS) {
		set_fpu_state(saved_fpu_state);
	}

	/* Wait until the threads finish computation */
	wait_worker_threads(threadpool);

	/* Make changes by other threads visible to this thread */
	pthreadpool_fence_acquire();

	/* Unprotect the global threadpool structures */
	chromium_mutex_unlock(threadpool->execution_mutex);
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	if (threadpool != NULL) {
		const size_t threads_count = threadpool->threads_count.value;
		if (threads_count > 1) {
			/* Lock the command variable to ensure that threads don't shutdown until both command and active_threads are updated */
			chromium_mutex_lock(threadpool->command_mutex);

			pthreadpool_store_relaxed_size_t(&threadpool->active_threads, threads_count - 1 /* caller thread */);

			/*
				* Store the command with release semantics to guarantee that if a worker thread observes
				* the new command value, it also observes the updated active_threads value.
				*
				* Note: the release fence inside pthread_mutex_unlock is insufficient,
				* because the workers might be waiting in a spin-loop rather than the conditional variable.
				*/
			pthreadpool_store_release_uint32_t(&threadpool->command, threadpool_command_shutdown);

			/* Wake up worker threads */
			chromium_cond_broadcast(threadpool->command_condvar);

			/* Commit the state changes and let workers start processing */
			chromium_mutex_unlock(threadpool->command_mutex);

			/* Wait until all threads return */
			// Call JobHandle::Join()
			// for (size_t thread = 1; thread < threads_count; thread++) {
			// 	pthread_join(threadpool->threads[thread].thread_object, NULL);
			// }

			/* Release resources */
			chromium_mutex_destroy(threadpool->execution_mutex);
			chromium_mutex_destroy(threadpool->execution_mutex);
			chromium_cond_destroy(threadpool->completion_condvar);
			chromium_mutex_destroy(threadpool->command_mutex);
			chromium_cond_destroy(&threadpool->command_condvar);
		}
		pthreadpool_deallocate(threadpool);
	}
}
