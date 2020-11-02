/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2008, Google Inc.
Copyright (c) 2013, 2020, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file sync/sync0arr.cc
The wait array used in synchronization primitives

Created 9/5/1995 Heikki Tuuri
*******************************************************/

#include "sync0arr.h"
#include <mysqld_error.h>
#include <mysql/plugin.h>
#include <hash.h>
#include <myisampack.h>
#include <sql_acl.h>
#include <mysys_err.h>
#include <my_sys.h>
#include "srv0srv.h"
#include "srv0start.h"
#include "i_s.h"
#include <sql_plugin.h>
#include <innodb_priv.h>

#include "lock0lock.h"
#include "sync0rw.h"

/*
			WAIT ARRAY
			==========

The wait array consists of cells each of which has an an event object created
for it. The threads waiting for a mutex, for example, can reserve a cell
in the array and suspend themselves to wait for the event to become signaled.
When using the wait array, remember to make sure that some thread holding
the synchronization object will eventually know that there is a waiter in
the array and signal the object, to prevent infinite wait.  Why we chose
to implement a wait array? First, to make mutexes fast, we had to code
our own implementation of them, which only in usually uncommon cases
resorts to using slow operating system primitives. Then we had the choice of
assigning a unique OS event for each mutex, which would be simpler, or
using a global wait array. In some operating systems, the global wait
array solution is more efficient and flexible, because we can do with
a very small number of OS events, say 200. In NT 3.51, allocating events
seems to be a quadratic algorithm, because 10 000 events are created fast,
but 100 000 events takes a couple of minutes to create.

As of 5.0.30 the above mentioned design is changed. Since now OS can handle
millions of wait events efficiently, we no longer have this concept of each
cell of wait array having one event.  Instead, now the event that a thread
wants to wait on is embedded in the wait object (mutex or rw_lock). We still
keep the global wait array for the sake of diagnostics and also to avoid
infinite wait The error_monitor thread scans the global wait array to signal
any waiting threads who have missed the signal. */

/** A cell where an individual thread may wait suspended until a resource
is released. The suspending is implemented using an operating system
event semaphore. */

struct sync_cell_t {
	rw_lock_t*	latch;		/*!< pointer to the object the
					thread is waiting for; if NULL
					the cell is free for use */
	ulint		request_type;	/*!< lock type requested on the
					object */
	const char*	file;		/*!< in debug version file where
					requested */
	ulint		line;		/*!< in debug version line where
					requested, or ULINT_UNDEFINED */
	os_thread_id_t	thread_id;	/*!< thread id of this waiting
					thread */
	bool		waiting;	/*!< TRUE if the thread has already
					called sync_array_event_wait
					on this cell */
	/** time(NULL) when the wait cell was reserved.
	FIXME: sync_array_print_long_waits_low() may display bogus
	warnings when the system time is adjusted to the past! */
	time_t		reservation_time;
};

/* NOTE: It is allowed for a thread to wait for an event allocated for
the array without owning the protecting mutex (depending on the case:
OS or database mutex), but all changes (set or reset) to the state of
the event must be made while owning the mutex. */

/** Synchronization array */
struct sync_array_t {

	/** Constructor
	Creates a synchronization wait array. It is protected by a mutex
	which is automatically reserved when the functions operating on it
	are called.
	@param[in]	num_cells	Number of cells to create */
	sync_array_t(ulint num_cells)
		UNIV_NOTHROW;

	/** Destructor */
	~sync_array_t()
		UNIV_NOTHROW;

	ulint		n_reserved;	/*!< number of currently reserved
					cells in the wait array */
	ulint		n_cells;	/*!< number of cells in the
					wait array */
	sync_cell_t*	array;		/*!< pointer to wait array */
	mysql_mutex_t	mutex;		/*!< System mutex protecting the
					data structure.  As this data
					structure is used in constructing
					the database mutex, to prevent
					infinite recursion in implementation,
					we fall back to an OS mutex. */
	ulint		res_count;	/*!< count of cell reservations
					since creation of the array */
	ulint           next_free_slot; /*!< the next free cell in the array */
	ulint           first_free_slot;/*!< the last slot that was freed */
};

/** User configured sync array size */
ulong	srv_sync_array_size = 1;

/** Locally stored copy of srv_sync_array_size */
ulint	sync_array_size;

/** The global array of wait cells for implementation of the database's own
mutexes and read-write locks */
static sync_array_t**	sync_wait_array;

/** count of how many times an object has been signalled */
ulint sg_count;

#define sync_array_exit(a)	mysql_mutex_unlock(&(a)->mutex)
#define sync_array_enter(a)	mysql_mutex_lock(&(a)->mutex)

#include "ut0counter.h"
sync_array_t* sync_array_get()
{
  return sync_wait_array[sync_array_size <= 1 ? 0
			 : get_rnd_value() % sync_array_size];
}


#ifdef UNIV_DEBUG
/******************************************************************//**
This function is called only in the debug version. Detects a deadlock
of one or more threads because of waits of semaphores.
@return TRUE if deadlock detected */
static
bool
sync_array_detect_deadlock(
/*=======================*/
	sync_array_t*	arr,	/*!< in: wait array; NOTE! the caller must
				own the mutex to array */
	sync_cell_t*	start,	/*!< in: cell where recursive search started */
	sync_cell_t*	cell,	/*!< in: cell to search */
	ulint		depth);	/*!< in: recursion depth */
#endif /* UNIV_DEBUG */

/** Constructor
Creates a synchronization wait array. It is protected by a mutex
which is automatically reserved when the functions operating on it
are called.
@param[in]	num_cells		Number of cells to create */
sync_array_t::sync_array_t(ulint num_cells)
	UNIV_NOTHROW
	:
	n_reserved(),
	n_cells(num_cells),
	array(UT_NEW_ARRAY_NOKEY(sync_cell_t, num_cells)),
	mutex(),
	res_count(),
	next_free_slot(),
	first_free_slot(ULINT_UNDEFINED)
{
	ut_a(num_cells > 0);

	memset(array, 0x0, sizeof(sync_cell_t) * n_cells);

	/* Then create the mutex to protect the wait array */
	mysql_mutex_init(0, &mutex, nullptr);
}

/** Validate the integrity of the wait array. Check
that the number of reserved cells equals the count variable.
@param[in,out]	arr	sync wait array */
static
void
sync_array_validate(sync_array_t* arr)
{
	ulint		i;
	ulint		count		= 0;

	sync_array_enter(arr);

	for (i = 0; i < arr->n_cells; i++) {
		sync_cell_t*	cell;

		cell = sync_array_get_nth_cell(arr, i);

		if (cell->latch) {
			count++;
		}
	}

	ut_a(count == arr->n_reserved);

	sync_array_exit(arr);
}

/** Destructor */
sync_array_t::~sync_array_t()
	UNIV_NOTHROW
{
	ut_a(n_reserved == 0);

	sync_array_validate(this);

	/* Release the mutex protecting the wait array */

	mysql_mutex_destroy(&mutex);

	UT_DELETE_ARRAY(array);
}

/*****************************************************************//**
Gets the nth cell in array.
@return cell */
UNIV_INTERN
sync_cell_t*
sync_array_get_nth_cell(
/*====================*/
	sync_array_t*	arr,	/*!< in: sync array */
	ulint		n)	/*!< in: index */
{
	ut_a(n < arr->n_cells);

	return(arr->array + n);
}

/******************************************************************//**
Frees the resources in a wait array. */
static
void
sync_array_free(
/*============*/
	sync_array_t*	arr)	/*!< in, own: sync wait array */
{
	UT_DELETE(arr);
}

/******************************************************************//**
Reserves a wait array cell for waiting for an object.
The event of the cell is reset to nonsignalled state.
@return sync cell to wait on */
sync_cell_t*
sync_array_reserve_cell(
/*====================*/
	sync_array_t*	arr,	/*!< in: wait array */
	rw_lock_t*	latch, /*!< in: latch to wait for */
	ulint		type,	/*!< in: lock request type */
	const char*	file,	/*!< in: file where requested */
	unsigned	line)	/*!< in: line where requested */
{
	sync_cell_t*	cell;

	sync_array_enter(arr);

	if (arr->first_free_slot != ULINT_UNDEFINED) {
		/* Try and find a slot in the free list */
		ut_ad(arr->first_free_slot < arr->next_free_slot);
		cell = sync_array_get_nth_cell(arr, arr->first_free_slot);
		arr->first_free_slot = cell->line;
	} else if (arr->next_free_slot < arr->n_cells) {
		/* Try and find a slot after the currently allocated slots */
		cell = sync_array_get_nth_cell(arr, arr->next_free_slot);
		++arr->next_free_slot;
	} else {
		sync_array_exit(arr);

		// We should return NULL and if there is more than
		// one sync array, try another sync array instance.
		return(NULL);
	}

	++arr->res_count;

	ut_ad(arr->n_reserved < arr->n_cells);
	ut_ad(arr->next_free_slot <= arr->n_cells);

	++arr->n_reserved;

	/* Reserve the cell. */
	ut_ad(!cell->latch);

	cell->request_type = type;
	cell->latch = latch;
	cell->waiting = false;

	cell->file = file;
	cell->line = line;

	sync_array_exit(arr);

	cell->thread_id = os_thread_get_curr_id();

	cell->reservation_time = time(NULL);

	return(cell);
}

/******************************************************************//**
Frees the cell. NOTE! sync_array_wait_event frees the cell
automatically! */
void
sync_array_free_cell(
/*=================*/
	sync_array_t*	arr,	/*!< in: wait array */
	sync_cell_t*&	cell)	/*!< in/out: the cell in the array */
{
	sync_array_enter(arr);

	ut_a(cell->latch);

	cell->waiting = false;
	cell->latch = nullptr;

	/* Setup the list of free slots in the array */
	cell->line = arr->first_free_slot;

	arr->first_free_slot = cell - arr->array;

	ut_a(arr->n_reserved > 0);
	arr->n_reserved--;

	if (arr->next_free_slot > arr->n_cells / 2 && arr->n_reserved == 0) {
#ifdef UNIV_DEBUG
		for (ulint i = 0; i < arr->next_free_slot; ++i) {
			cell = sync_array_get_nth_cell(arr, i);

			ut_ad(!cell->waiting);
			ut_ad(!cell->latch);
		}
#endif /* UNIV_DEBUG */
		arr->next_free_slot = 0;
		arr->first_free_slot = ULINT_UNDEFINED;
	}
	sync_array_exit(arr);

	cell = 0;
}

/******************************************************************//**
This function should be called when a thread starts to wait on
a wait array cell. In the debug version this function checks
if the wait for a semaphore will result in a deadlock, in which
case prints info and asserts. */
void
sync_array_wait_event(
/*==================*/
	sync_array_t*	arr,	/*!< in: wait array */
	sync_cell_t*&	cell)	/*!< in: index of the reserved cell */
{
	sync_array_enter(arr);

	ut_ad(!cell->waiting);
	ut_ad(cell->latch);
	ut_ad(os_thread_get_curr_id() == cell->thread_id);

	cell->waiting = true;

#ifdef UNIV_DEBUG

	/* We use simple enter to the mutex below, because if
	we cannot acquire it at once, mutex_enter would call
	recursively sync_array routines, leading to trouble.
	rw_lock_debug_mutex freezes the debug lists. */

	rw_lock_debug_mutex_enter();

	if (sync_array_detect_deadlock(arr, cell, cell, 0)) {

		ib::fatal() << "########################################"
                        " Deadlock Detected!";
	}

	rw_lock_debug_mutex_exit();
#endif /* UNIV_DEBUG */
	sync_array_exit(arr);

	tpool::tpool_wait_begin();

	rw_lock_t *lock= cell->latch;
	const int32_t lock_word = lock->lock_word;
	if (cell->request_type == RW_LOCK_X_WAIT) {
		if (lock_word) {
			mysql_mutex_lock(&lock->wait_mutex);
			while (lock->lock_word) {
				mysql_cond_wait(&lock->wait_ex_cond,
						&lock->wait_mutex);
			}
			mysql_mutex_unlock(&lock->wait_mutex);
		}
	} else if (lock_word <= 0) {
		mysql_mutex_lock(&lock->wait_mutex);
		for (;;) {
			/* Ensure that we will be woken up. */
			lock->waiters.exchange(
				1, std::memory_order_acquire);
			int32_t l = lock->lock_word.load(
				std::memory_order_acquire);
			if (l > 0) {
				break;
			} else if (l == 0) {
				mysql_cond_signal(&lock->wait_ex_cond);
			}

			mysql_cond_wait(&lock->wait_cond,
					&lock->wait_mutex);
		}
		mysql_mutex_unlock(&lock->wait_mutex);
	}

	tpool::tpool_wait_end();

	sync_array_free_cell(arr, cell);

	cell = 0;
}

/******************************************************************//**
Reports info of a wait array cell. */
static
void
sync_array_cell_print(
/*==================*/
	FILE*		file,		/*!< in: file where to print */
	sync_cell_t*	cell)		/*!< in: sync cell */
{
	fprintf(file,
		"--Thread %lu has waited at %s line %lu"
		" for %.2f seconds the semaphore:\n",
		(ulong) os_thread_pf(cell->thread_id),
		innobase_basename(cell->file), (ulong) cell->line,
		difftime(time(NULL), cell->reservation_time));

	const auto type = cell->request_type;

	fputs(type == RW_LOCK_X ? "X-lock on"
	      : type == RW_LOCK_X_WAIT ? "X-lock (wait_ex) on"
	      : type == RW_LOCK_SX ? "SX-lock on"
	      : "S-lock on", file);

	if (rw_lock_t *rwlock = cell->latch) {
		fprintf(file,
			" RW-latch at %p created in file %s line %u\n",
			(void*) rwlock, innobase_basename(rwlock->cfile_name),
			rwlock->cline);

		const ulint writer = rw_lock_get_writer(rwlock);

		if (writer != RW_LOCK_NOT_LOCKED) {
			fprintf(file,
				"a writer (thread id " ULINTPF ") has"
				" reserved it in mode %s",
				os_thread_pf(rwlock->writer_thread),
				writer == RW_LOCK_X ? " exclusive\n"
				: writer == RW_LOCK_SX ? " SX\n"
				: " wait exclusive\n");
		}

		fprintf(file,
			"number of readers " ULINTPF
			", waiters flag %d, "
			"lock_word: %x\n"
			"Last time write locked in file %s line %u"
#if 0 /* JAN: TODO: FIX LATER */
			"\nHolder thread " ULINTPF
			" file %s line " ULINTPF
#endif
			"\n",
			rw_lock_get_reader_count(rwlock),
			uint32_t{rwlock->waiters},
			int32_t{rwlock->lock_word},
			innobase_basename(rwlock->last_x_file_name),
			rwlock->last_x_line
#if 0 /* JAN: TODO: FIX LATER */
			, os_thread_pf(rwlock->thread_id),
			innobase_basename(rwlock->file_name),
			rwlock->line
#endif
			);
	}

	if (!cell->waiting) {
		fputs("wait has ended\n", file);
	}
}

#ifdef UNIV_DEBUG
/******************************************************************//**
Looks for a cell with the given thread id.
@return pointer to cell or NULL if not found */
static
sync_cell_t*
sync_array_find_thread(
/*===================*/
	sync_array_t*	arr,	/*!< in: wait array */
	os_thread_id_t	thread)	/*!< in: thread id */
{
	ulint		i;

	for (i = 0; i < arr->n_cells; i++) {
		sync_cell_t*	cell;

		cell = sync_array_get_nth_cell(arr, i);

		if (cell->latch && os_thread_eq(cell->thread_id, thread)) {
			return(cell);	/* Found */
		}
	}

	return(NULL);	/* Not found */
}

/******************************************************************//**
Recursion step for deadlock detection.
@return TRUE if deadlock detected */
static
ibool
sync_array_deadlock_step(
/*=====================*/
	sync_array_t*	arr,	/*!< in: wait array; NOTE! the caller must
				own the mutex to array */
	sync_cell_t*	start,	/*!< in: cell where recursive search
				started */
	os_thread_id_t	thread,	/*!< in: thread to look at */
	ulint		pass,	/*!< in: pass value */
	ulint		depth)	/*!< in: recursion depth */
{
	sync_cell_t*	new_cell;

	if (pass != 0) {
		/* If pass != 0, then we do not know which threads are
		responsible of releasing the lock, and no deadlock can
		be detected. */

		return(FALSE);
	}

	new_cell = sync_array_find_thread(arr, thread);

	if (new_cell == start) {
		/* Deadlock */
		fputs("########################################\n"
		      "DEADLOCK of threads detected!\n", stderr);

		return(TRUE);

	} else if (new_cell) {
		return(sync_array_detect_deadlock(
			arr, start, new_cell, depth + 1));
	}
	return(FALSE);
}

/**
Report an error to stderr.
@param lock		rw-lock instance
@param debug		rw-lock debug information
@param cell		thread context */
static
void
sync_array_report_error(
	rw_lock_t*		lock,
	rw_lock_debug_t*	debug,
	sync_cell_t* 		cell)
{
	fprintf(stderr, "rw-lock %p ", (void*) lock);
	sync_array_cell_print(stderr, cell);
	rw_lock_debug_print(stderr, debug);
}

/******************************************************************//**
This function is called only in the debug version. Detects a deadlock
of one or more threads because of waits of semaphores.
@return TRUE if deadlock detected */
static
bool
sync_array_detect_deadlock(
/*=======================*/
	sync_array_t*	arr,	/*!< in: wait array; NOTE! the caller must
				own the mutex to array */
	sync_cell_t*	start,	/*!< in: cell where recursive search started */
	sync_cell_t*	cell,	/*!< in: cell to search */
	ulint		depth)	/*!< in: recursion depth */
{
	os_thread_id_t	thread;
	ibool		ret;
	rw_lock_debug_t*debug;

	ut_a(arr);
	ut_a(start);
	ut_a(cell);
	ut_ad(cell->latch);
	ut_ad(os_thread_get_curr_id() == start->thread_id);
	ut_ad(depth < 100);

	depth++;

	if (!cell->waiting) {
		/* No deadlock here */
		return(false);
	}

	rw_lock_t* lock = cell->latch;
	switch (cell->request_type) {
	case RW_LOCK_X:
	case RW_LOCK_X_WAIT:
		for (debug = UT_LIST_GET_FIRST(lock->debug_list);
		     debug != NULL;
		     debug = UT_LIST_GET_NEXT(list, debug)) {

			thread = debug->thread_id;

			switch (debug->lock_type) {
			case RW_LOCK_X:
			case RW_LOCK_SX:
			case RW_LOCK_X_WAIT:
				if (os_thread_eq(thread, cell->thread_id)) {
					break;
				}
				/* fall through */
			case RW_LOCK_S:

				/* The (wait) x-lock request can block
				infinitely only if someone (can be also cell
				thread) is holding s-lock, or someone
				(cannot be cell thread) (wait) x-lock or
				sx-lock, and he is blocked by start thread */

				ret = sync_array_deadlock_step(
					arr, start, thread, debug->pass,
					depth);

				if (ret) {
					sync_array_report_error(
						lock, debug, cell);
					rw_lock_debug_print(stderr, debug);
					return(TRUE);
				}
			}
		}

		return(false);

	case RW_LOCK_SX:
		for (debug = UT_LIST_GET_FIRST(lock->debug_list);
		     debug != 0;
		     debug = UT_LIST_GET_NEXT(list, debug)) {

			thread = debug->thread_id;

			switch (debug->lock_type) {
			case RW_LOCK_X:
			case RW_LOCK_SX:
			case RW_LOCK_X_WAIT:

				if (os_thread_eq(thread, cell->thread_id)) {
					break;
				}

				/* The sx-lock request can block infinitely
				only if someone (can be also cell thread) is
				holding (wait) x-lock or sx-lock, and he is
				blocked by start thread */

				ret = sync_array_deadlock_step(
					arr, start, thread, debug->pass,
					depth);

				if (ret) {
					sync_array_report_error(
						lock, debug, cell);
					return(TRUE);
				}
			}
		}

		return(false);

	case RW_LOCK_S:
		for (debug = UT_LIST_GET_FIRST(lock->debug_list);
		     debug != 0;
		     debug = UT_LIST_GET_NEXT(list, debug)) {

			thread = debug->thread_id;

			if (debug->lock_type == RW_LOCK_X
			    || debug->lock_type == RW_LOCK_X_WAIT) {

				/* The s-lock request can block infinitely
				only if someone (can also be cell thread) is
				holding (wait) x-lock, and he is blocked by
				start thread */

				ret = sync_array_deadlock_step(
					arr, start, thread, debug->pass,
					depth);

				if (ret) {
					sync_array_report_error(
						lock, debug, cell);
					return(TRUE);
				}
			}
		}

		return(false);

	default:
		ut_error;
	}

	return(true);
}
#endif /* UNIV_DEBUG */

/**********************************************************************//**
Prints warnings of long semaphore waits to stderr.
@return TRUE if fatal semaphore wait threshold was exceeded */
static
bool
sync_array_print_long_waits_low(
/*============================*/
	sync_array_t*	arr,	/*!< in: sync array instance */
	os_thread_id_t*	waiter,	/*!< out: longest waiting thread */
	const rw_lock_t**latch,	/*!< out: longest-waited-for latch */
	ibool*		noticed)/*!< out: TRUE if long wait noticed */
{
	double		fatal_timeout = static_cast<double>(
		srv_fatal_semaphore_wait_threshold);
	ibool		fatal = FALSE;
	double		longest_diff = 0;
	ulint		i;

	/* For huge tables, skip the check during CHECK TABLE etc... */
	if (btr_validate_index_running) {
		return(false);
	}

#if defined HAVE_valgrind && !__has_feature(memory_sanitizer)
	/* Increase the timeouts if running under valgrind because it executes
	extremely slowly. HAVE_valgrind does not necessary mean that
	we are running under valgrind but we have no better way to tell.
	See Bug#58432 innodb.innodb_bug56143 fails under valgrind
	for an example */
# define SYNC_ARRAY_TIMEOUT	2400
	fatal_timeout *= 10;
#else
# define SYNC_ARRAY_TIMEOUT	240
#endif

	for (ulint i = 0; i < arr->n_cells; i++) {

		sync_cell_t*	cell;

		cell = sync_array_get_nth_cell(arr, i);

		if (!cell->latch || !cell->waiting) {
			continue;
		}

		double	diff = difftime(time(NULL), cell->reservation_time);

		if (diff > SYNC_ARRAY_TIMEOUT) {
			ib::warn() << "A long semaphore wait:";
			sync_array_cell_print(stderr, cell);
			*noticed = TRUE;
		}

		if (diff > fatal_timeout) {
			fatal = TRUE;
		}

		if (diff > longest_diff) {
			longest_diff = diff;
			*latch = cell->latch;
			*waiter = cell->thread_id;
		}
	}

	/* We found a long semaphore wait, print all threads that are
	waiting for a semaphore. */
	if (*noticed) {
		for (i = 0; i < arr->n_cells; i++) {
			sync_cell_t*	cell;

			cell = sync_array_get_nth_cell(arr, i);

			if (!cell->latch || !cell->waiting) {
				continue;
			}

			ib::info() << "A semaphore wait:";
			sync_array_cell_print(stderr, cell);
		}
	}

#undef SYNC_ARRAY_TIMEOUT

	return(fatal);
}

/** Log warnings of long semaphore waits to stderr.
@param[out] waiter  longest-waiting thread
@param[out] l       longest-waited-for latch
@return whether fatal semaphore wait threshold was exceeded */
bool sync_array_print_long_waits(os_thread_id_t *waiter, const rw_lock_t **l)
{
	ulint		i;
	ibool		fatal = FALSE;
	ibool		noticed = FALSE;

	for (i = 0; i < sync_array_size; ++i) {
		sync_array_t*	arr = sync_wait_array[i];

		sync_array_enter(arr);

		if (sync_array_print_long_waits_low(
			    arr, waiter, l, &noticed)) {

			fatal = TRUE;
		}

		sync_array_exit(arr);
	}

	if (noticed) {
		fprintf(stderr,
			"InnoDB: ###### Starts InnoDB Monitor"
			" for 30 secs to print diagnostic info:\n");

		my_bool old_val = srv_print_innodb_monitor;

		/* If some crucial semaphore is reserved, then also the InnoDB
		Monitor can hang, and we do not get diagnostics. Since in
		many cases an InnoDB hang is caused by a pwrite() or a pread()
		call hanging inside the operating system, let us print right
		now the values of pending calls of these. */

		fprintf(stderr,
			"InnoDB: Pending reads " UINT64PF
			", writes " UINT64PF "\n",
			MONITOR_VALUE(MONITOR_OS_PENDING_READS),
			MONITOR_VALUE(MONITOR_OS_PENDING_WRITES));

		srv_print_innodb_monitor = TRUE;

		lock_wait_timeout_task(nullptr);

		srv_print_innodb_monitor = static_cast<my_bool>(old_val);
		fprintf(stderr,
			"InnoDB: ###### Diagnostic info printed"
			" to the standard error stream\n");
	}

	return(fatal);
}

/**********************************************************************//**
Prints info of the wait array. */
static
void
sync_array_print_info_low(
/*======================*/
	FILE*		file,	/*!< in: file where to print */
	sync_array_t*	arr)	/*!< in: wait array */
{
	ulint		i;
	ulint		count = 0;

	fprintf(file,
		"OS WAIT ARRAY INFO: reservation count " ULINTPF "\n",
		arr->res_count);

	for (i = 0; count < arr->n_reserved; ++i) {
		sync_cell_t*	cell;

		cell = sync_array_get_nth_cell(arr, i);

		if (cell->latch) {
			count++;
			sync_array_cell_print(file, cell);
		}
	}
}

/**********************************************************************//**
Prints info of the wait array. */
static
void
sync_array_print_info(
/*==================*/
	FILE*		file,	/*!< in: file where to print */
	sync_array_t*	arr)	/*!< in: wait array */
{
	sync_array_enter(arr);

	sync_array_print_info_low(file, arr);

	sync_array_exit(arr);
}

/** Create the primary system wait arrays */
void sync_array_init()
{
	ut_a(sync_wait_array == NULL);
	ut_a(srv_sync_array_size > 0);
	ut_a(srv_max_n_threads > 0);

	sync_array_size = srv_sync_array_size;

	sync_wait_array = UT_NEW_ARRAY_NOKEY(sync_array_t*, sync_array_size);

	ulint	n_slots = 1 + (srv_max_n_threads - 1) / sync_array_size;

	for (ulint i = 0; i < sync_array_size; ++i) {

		sync_wait_array[i] = UT_NEW_NOKEY(sync_array_t(n_slots));
	}
}

/** Destroy the sync array wait sub-system. */
void sync_array_close()
{
	for (ulint i = 0; i < sync_array_size; ++i) {
		sync_array_free(sync_wait_array[i]);
	}

	UT_DELETE_ARRAY(sync_wait_array);
	sync_wait_array = NULL;
}

/**********************************************************************//**
Print info about the sync array(s). */
void
sync_array_print(
/*=============*/
	FILE*		file)		/*!< in/out: Print to this stream */
{
	for (ulint i = 0; i < sync_array_size; ++i) {
		sync_array_print_info(file, sync_wait_array[i]);
	}

	fprintf(file,
		"OS WAIT ARRAY INFO: signal count " ULINTPF "\n", sg_count);

}

/**********************************************************************//**
Prints info of the wait array without using any mutexes/semaphores. */
UNIV_INTERN
void
sync_array_print_innodb(void)
/*=========================*/
{
	ulint i;
	sync_array_t*	arr = sync_array_get();

	fputs("InnoDB: Semaphore wait debug output started for InnoDB:\n", stderr);

	for (i = 0; i < arr->n_cells; i++) {
		sync_cell_t*	cell;

		cell = sync_array_get_nth_cell(arr, i);

		if (!cell->latch || !cell->waiting) {

			continue;
		}

		fputs("InnoDB: Warning: semaphore wait:\n",
			      stderr);
		sync_array_cell_print(stderr, cell);
	}

	fputs("InnoDB: Semaphore wait debug output ended:\n", stderr);

}

/**********************************************************************//**
Get number of items on sync array. */
UNIV_INTERN
ulint
sync_arr_get_n_items(void)
/*======================*/
{
	sync_array_t*	sync_arr = sync_array_get();
	return (ulint) sync_arr->n_cells;
}

/******************************************************************//**
Get specified item from sync array if it is reserved. Set given
pointer to array item if it is reserved.
@return true if item is reserved, false othervise */
UNIV_INTERN
ibool
sync_arr_get_item(
/*==============*/
	ulint		i,		/*!< in: requested item */
	sync_cell_t	**cell)		/*!< out: cell contents if item
					reserved */
{
	sync_array_t*	sync_arr;
	sync_cell_t*	wait_cell;
	ibool		found = FALSE;

	sync_arr = sync_array_get();
	wait_cell = sync_array_get_nth_cell(sync_arr, i);

	if (wait_cell && wait_cell->latch && wait_cell->waiting) {
		found = TRUE;
		*cell = wait_cell;
	}

	return found;
}

/*******************************************************************//**
Function to populate INFORMATION_SCHEMA.INNODB_SYS_SEMAPHORE_WAITS table.
Loop through each item on sync array, and extract the column
information and fill the INFORMATION_SCHEMA.INNODB_SYS_SEMAPHORE_WAITS table.
@return 0 on success */
UNIV_INTERN
int
sync_arr_fill_sys_semphore_waits_table(
/*===================================*/
	THD*		thd,	/*!< in: thread */
	TABLE_LIST*	tables,	/*!< in/out: tables to fill */
	Item*		)	/*!< in: condition (not used) */
{
	Field**		fields;
	ulint		n_items;

	DBUG_ENTER("i_s_sys_semaphore_waits_fill_table");
	RETURN_IF_INNODB_NOT_STARTED(tables->schema_table_name.str);

	/* deny access to user without PROCESS_ACL privilege */
	if (check_global_access(thd, PROCESS_ACL)) {
		DBUG_RETURN(0);
	}

	fields = tables->table->field;
	n_items = sync_arr_get_n_items();
	ulint type;

	for(ulint i=0; i < n_items;i++) {
		sync_cell_t *cell=NULL;
		if (sync_arr_get_item(i, &cell)) {
			type = cell->request_type;
			/* JAN: FIXME
			OK(fields[SYS_SEMAPHORE_WAITS_THREAD_ID]->store(,
			(longlong)os_thread_pf(cell->thread), true));
			*/
			OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_FILE], innobase_basename(cell->file)));
			OK(fields[SYS_SEMAPHORE_WAITS_LINE]->store(cell->line, true));
			fields[SYS_SEMAPHORE_WAITS_LINE]->set_notnull();
			OK(fields[SYS_SEMAPHORE_WAITS_WAIT_TIME]->store(
				   difftime(time(NULL),
					    cell->reservation_time)));

			if (type == RW_LOCK_X_WAIT
				|| type == RW_LOCK_X
				|| type == RW_LOCK_SX
			        || type == RW_LOCK_S) {
				if (rw_lock_t* rwlock= cell->latch) {
					ulint writer = rw_lock_get_writer(rwlock);

					OK(fields[SYS_SEMAPHORE_WAITS_WAIT_OBJECT]->store((longlong)rwlock, true));
					if (type == RW_LOCK_X) {
						OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_WAIT_TYPE], "RW_LOCK_X"));
					} else if (type == RW_LOCK_X_WAIT) {
						OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_WAIT_TYPE], "RW_LOCK_X_WAIT"));
					} else if (type == RW_LOCK_S) {
						OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_WAIT_TYPE], "RW_LOCK_S"));
					} else if (type == RW_LOCK_SX) {
						OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_WAIT_TYPE], "RW_LOCK_SX"));
					}

					if (writer != RW_LOCK_NOT_LOCKED) {
						// JAN: FIXME
						// OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_OBJECT_NAME], rwlock->lock_name));
						OK(fields[SYS_SEMAPHORE_WAITS_WRITER_THREAD]->store(os_thread_pf(rwlock->writer_thread), true));

						if (writer == RW_LOCK_X) {
							OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_RESERVATION_MODE], "RW_LOCK_X"));
						} else if (writer == RW_LOCK_X_WAIT) {
							OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_RESERVATION_MODE], "RW_LOCK_X_WAIT"));
						} else if (type == RW_LOCK_SX) {
							OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_RESERVATION_MODE], "RW_LOCK_SX"));
						}

						//OK(fields[SYS_SEMAPHORE_WAITS_HOLDER_THREAD_ID]->store(rwlock->thread_id, true));
						//OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_HOLDER_FILE], innobase_basename(rwlock->file_name)));
						//OK(fields[SYS_SEMAPHORE_WAITS_HOLDER_LINE]->store(rwlock->line, true));
						//fields[SYS_SEMAPHORE_WAITS_HOLDER_LINE]->set_notnull();
						OK(fields[SYS_SEMAPHORE_WAITS_READERS]->store(rw_lock_get_reader_count(rwlock), true));
						OK(fields[SYS_SEMAPHORE_WAITS_WAITERS_FLAG]->store(
							   rwlock->waiters,
							   true));
						OK(fields[SYS_SEMAPHORE_WAITS_LOCK_WORD]->store(
							   rwlock->lock_word,
							   true));
						OK(field_store_string(fields[SYS_SEMAPHORE_WAITS_LAST_WRITER_FILE], innobase_basename(rwlock->last_x_file_name)));
						OK(fields[SYS_SEMAPHORE_WAITS_LAST_WRITER_LINE]->store(rwlock->last_x_line, true));
						fields[SYS_SEMAPHORE_WAITS_LAST_WRITER_LINE]->set_notnull();
						OK(fields[SYS_SEMAPHORE_WAITS_OS_WAIT_COUNT]->store(rwlock->count_os_wait, true));
					}
				}
			}

			OK(schema_table_store_record(thd, tables->table));
		}
	}

	DBUG_RETURN(0);
}
