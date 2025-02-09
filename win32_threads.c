/*
 * Copyright (c) 1994 by Xerox Corporation.  All rights reserved.
 * Copyright (c) 1996 by Silicon Graphics.  All rights reserved.
 * Copyright (c) 1998 by Fergus Henderson.  All rights reserved.
 * Copyright (c) 2000-2008 by Hewlett-Packard Development Company.
 * All rights reserved.
 * Copyright (c) 2008-2022 Ivan Maidanski
 *
 * THIS MATERIAL IS PROVIDED AS IS, WITH ABSOLUTELY NO WARRANTY EXPRESSED
 * OR IMPLIED.  ANY USE IS AT YOUR OWN RISK.
 *
 * Permission is hereby granted to use or copy this program
 * for any purpose, provided the above notices are retained on all copies.
 * Permission to modify the code and to distribute modified code is granted,
 * provided the above notices are retained, and a notice that the code was
 * modified is included with the above copyright notice.
 */

#include "private/pthread_support.h"

#if defined(GC_WIN32_THREADS)

/* Allocation lock declarations.        */
#if !defined(USE_PTHREAD_LOCKS)
  GC_INNER CRITICAL_SECTION GC_allocate_ml;
#else
  GC_INNER pthread_mutex_t GC_allocate_ml = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifdef GC_ASSERTIONS
    GC_INNER unsigned long GC_lock_holder = NO_THREAD;
        /* Thread id for current holder of allocation lock */
#endif

#undef CreateThread
#undef ExitThread
#undef _beginthreadex
#undef _endthreadex

#ifdef GC_PTHREADS
# include <errno.h> /* for EAGAIN */

 /* Cygwin-specific forward decls */
# undef pthread_create
# undef pthread_join
# undef pthread_detach

# ifndef GC_NO_PTHREAD_SIGMASK
#   undef pthread_sigmask
# endif

  STATIC void * GC_pthread_start(void * arg);

# ifdef CAN_CALL_ATFORK
#   include <unistd.h>
# endif

#elif !defined(MSWINCE)
# include <process.h>  /* For _beginthreadex, _endthreadex */
# include <errno.h> /* for errno, EAGAIN */

#endif /* !GC_PTHREADS && !MSWINCE */

static ptr_t copy_ptr_regs(word *regs, const CONTEXT *pcontext);

#ifndef GC_NO_THREADS_DISCOVERY
  /* This code operates in two distinct modes, depending on     */
  /* the setting of GC_win32_dll_threads.                       */
  /* If GC_win32_dll_threads is set, all threads in the process */
  /* are implicitly registered with the GC by DllMain.          */
  /* No explicit registration is required, and attempts at      */
  /* explicit registration are ignored.  This mode is           */
  /* very different from the Posix operation of the collector.  */
  /* In this mode access to the thread table is lock-free.      */
  /* Hence there is a static limit on the number of threads.    */

# ifdef GC_DISCOVER_TASK_THREADS
    /* GC_DISCOVER_TASK_THREADS should be used if DllMain-based */
    /* thread registration is required but it is impossible to  */
    /* call GC_use_threads_discovery before other GC routines.  */
#   define GC_win32_dll_threads TRUE
# else
    STATIC GC_bool GC_win32_dll_threads = FALSE;
    /* GC_win32_dll_threads must be set (if needed) at the      */
    /* application initialization time, i.e. before any         */
    /* collector or thread calls.  We make it a "dynamic"       */
    /* option only to avoid multiple library versions.          */
# endif

#else
  /* If GC_win32_dll_threads is FALSE (or the collector is      */
  /* built without GC_DLL defined), things operate in a way     */
  /* that is very similar to Posix platforms, and new threads   */
  /* must be registered with the collector, e.g. by using       */
  /* preprocessor-based interception of the thread primitives.  */
  /* In this case, we use a real data structure for the thread  */
  /* table.  Note that there is no equivalent of linker-based   */
  /* call interception, since we don't have ELF-like            */
  /* facilities.  The Windows analog appears to be "API         */
  /* hooking", which really seems to be a standard way to       */
  /* do minor binary rewriting (?).  I'd prefer not to have     */
  /* the basic collector rely on such facilities, but an        */
  /* optional package that intercepts thread calls this way     */
  /* would probably be nice.                                    */
# define GC_win32_dll_threads FALSE
# undef MAX_THREADS
# define MAX_THREADS 1 /* dll_thread_table[] is always empty.   */
#endif /* GC_NO_THREADS_DISCOVERY */

/* We have two versions of the thread table.  Which one */
/* we use depends on whether GC_win32_dll_threads       */
/* is set.  Note that before initialization, we don't   */
/* add any entries to either table, even if DllMain is  */
/* called.  The main thread will be added on            */
/* initialization.                                      */

/* The type of the first argument to InterlockedExchange.       */
/* Documented to be LONG volatile *, but at least gcc likes     */
/* this better.                                                 */
typedef LONG * IE_t;

#ifdef GC_ASSERTIONS
  GC_INNER GC_bool GC_thr_initialized = FALSE;
#endif

#ifndef GC_ALWAYS_MULTITHREADED
  GC_INNER GC_bool GC_need_to_lock = FALSE;
#endif

/* GC_use_threads_discovery() is currently incompatible with pthreads   */
/* and WinCE.  It might be possible to get DllMain-based thread         */
/* registration to work with Cygwin, but if you try it then you are on  */
/* your own.                                                            */
GC_API void GC_CALL GC_use_threads_discovery(void)
{
# ifdef GC_NO_THREADS_DISCOVERY
    ABORT("GC DllMain-based thread registration unsupported");
# else
    /* Turn on GC_win32_dll_threads. */
    GC_ASSERT(!GC_is_initialized);
    /* Note that GC_use_threads_discovery is expected to be called by   */
    /* the client application (not from DllMain) at start-up.           */
#   ifndef GC_DISCOVER_TASK_THREADS
      GC_win32_dll_threads = TRUE;
#   endif
    GC_init();
# endif
}

#define ADDR_LIMIT ((ptr_t)GC_WORD_MAX)

typedef struct GC_Thread_Rep * GC_thread;
typedef volatile struct GC_Thread_Rep * GC_vthread;

#ifndef GC_NO_THREADS_DISCOVERY
  static thread_id_t main_thread_id;

  /* We track thread attachments while the world is supposed to be      */
  /* stopped.  Unfortunately, we cannot stop them from starting, since  */
  /* blocking in DllMain seems to cause the world to deadlock.  Thus,   */
  /* we have to recover if we notice this in the middle of marking.     */
  STATIC volatile AO_t GC_attached_thread = FALSE;

  /* We assumed that volatile ==> memory ordering, at least among       */
  /* volatiles.  This code should consistently use atomic_ops.          */
  STATIC volatile GC_bool GC_please_stop = FALSE;
#elif defined(GC_ASSERTIONS)
  STATIC GC_bool GC_please_stop = FALSE;
#endif /* GC_NO_THREADS_DISCOVERY && GC_ASSERTIONS */

#if defined(WRAP_MARK_SOME) && !defined(GC_PTHREADS)
  /* Return TRUE if an thread was attached since we last asked or */
  /* since GC_attached_thread was explicitly reset.               */
  GC_INNER GC_bool GC_started_thread_while_stopped(void)
  {
#   ifndef GC_NO_THREADS_DISCOVERY
      if (GC_win32_dll_threads) {
#       ifdef AO_HAVE_compare_and_swap_release
          if (AO_compare_and_swap_release(&GC_attached_thread, TRUE,
                                          FALSE /* stored */))
            return TRUE;
#       else
          AO_nop_full(); /* Prior heap reads need to complete earlier. */
          if (AO_load(&GC_attached_thread)) {
            AO_store(&GC_attached_thread, FALSE);
            return TRUE;
          }
#       endif
      }
#   endif
    return FALSE;
  }
#endif /* WRAP_MARK_SOME */

/* Thread table used if GC_win32_dll_threads is set.    */
/* This is a fixed size array.                          */
/* Since we use runtime conditionals, both versions     */
/* are always defined.                                  */
# ifndef MAX_THREADS
#   define MAX_THREADS 512
# endif

/* Things may get quite slow for large numbers of threads,      */
/* since we look them up with sequential search.                */
volatile struct GC_Thread_Rep dll_thread_table[MAX_THREADS];

STATIC volatile LONG GC_max_thread_index = 0;
                        /* Largest index in dll_thread_table    */
                        /* that was ever used.                  */

/* And now the version used if GC_win32_dll_threads is not set. */
/* This is a chained hash table, with much of the code borrowed */
/* from the Posix implementation.                               */
GC_INNER GC_thread GC_threads[THREAD_TABLE_SZ] = {0};

/* It may not be safe to allocate when we register the first thread.    */
/* Thus we allocated one statically.  It does not contain any pointer   */
/* field we need to push ("next" and "status" fields are unused).       */
static struct GC_Thread_Rep first_thread;
static GC_bool first_thread_used = FALSE;

/* Add a thread to GC_threads.  We assume it wasn't already there.      */
STATIC GC_thread GC_new_thread(thread_id_t id)
{
  int hv = THREAD_TABLE_INDEX(id);
  GC_thread result;

  GC_ASSERT(I_HOLD_LOCK());
# ifdef DEBUG_THREADS
    GC_log_printf("Creating thread 0x%lx\n", (long)id);
    if (GC_threads[hv] != NULL)
      GC_log_printf("Hash collision at GC_threads[%d]\n", hv);
# endif
  if (EXPECT(!first_thread_used, FALSE)) {
    result = &first_thread;
    first_thread_used = TRUE;
    GC_ASSERT(NULL == GC_threads[hv]);
#   if defined(GC_NO_FINALIZATION) && defined(CPPCHECK)
      GC_noop1(result -> no_fnlz_pad[0]);
#   endif
  } else {
    GC_ASSERT(!GC_win32_dll_threads);
    result = (struct GC_Thread_Rep *)
                GC_INTERNAL_MALLOC(sizeof(struct GC_Thread_Rep), NORMAL);
    if (EXPECT(NULL == result, FALSE)) return NULL;
  }
  /* The id field is set by the caller. */
  result -> tm.next = GC_threads[hv];
  GC_threads[hv] = result;
  GC_ASSERT(0 == result -> flags);
  if (EXPECT(result != &first_thread, TRUE))
    GC_dirty(result);
  return result;
}

GC_INNER GC_bool GC_in_thread_creation = FALSE;
                                /* Protected by allocation lock. */

GC_INLINE void GC_record_stack_base(GC_thread me,
                                    const struct GC_stack_base *sb)
{
  me -> stack_end = (ptr_t)sb->mem_base;
# ifdef IA64
    me -> backing_store_end = (ptr_t)sb->reg_base;
# elif defined(I386)
    me -> initial_stack_base = (ptr_t)sb->mem_base;
# endif
  if (NULL == me -> stack_end)
    ABORT("Bad stack base in GC_register_my_thread");
}

/* This may be called from DllMain, and hence operates under unusual    */
/* constraints.  In particular, it must be lock-free if                 */
/* GC_win32_dll_threads is set.  Always called from the thread being    */
/* added.  If GC_win32_dll_threads is not set, we already hold the      */
/* allocation lock except possibly during single-threaded startup code. */
/* Does not initialize thread local free lists.                         */
STATIC GC_thread GC_register_my_thread_inner(const struct GC_stack_base *sb,
                                             thread_id_t id)
{
  GC_vthread me;

  /* The following should be a no-op according to the Win32     */
  /* documentation.  There is empirical evidence that it        */
  /* isn't.             - HB                                    */
# if defined(MPROTECT_VDB) && !defined(CYGWIN32)
    if (GC_auto_incremental
#       ifdef GWW_VDB
          && !GC_gww_dirty_init()
#       endif
        )
      GC_set_write_fault_handler();
# endif

# ifndef GC_NO_THREADS_DISCOVERY
    if (GC_win32_dll_threads) {
      int i;
      /* It appears to be unsafe to acquire a lock here, since this     */
      /* code is apparently not preemptible on some systems.            */
      /* (This is based on complaints, not on Microsoft's official      */
      /* documentation, which says this should perform "only simple     */
      /* initialization tasks".)                                        */
      /* Hence we make do with nonblocking synchronization.             */
      /* It has been claimed that DllMain is really only executed with  */
      /* a particular system lock held, and thus careful use of locking */
      /* around code that doesn't call back into the system libraries   */
      /* might be OK.  But this has not been tested across all Win32    */
      /* variants.                                                      */
      for (i = 0;
           InterlockedExchange(&dll_thread_table[i].tm.long_in_use, 1) != 0;
           i++) {
        /* Compare-and-swap would make this cleaner, but that's not     */
        /* supported before Windows 98 and NT 4.0.  In Windows 2000,    */
        /* InterlockedExchange is supposed to be replaced by            */
        /* InterlockedExchangePointer, but that's not really what I     */
        /* want here.                                                   */
        /* FIXME: We should eventually declare Windows 95 dead and use  */
        /* AO_ primitives here.                                         */
        if (i == MAX_THREADS - 1)
          ABORT("Too many threads");
      }
      /* Update GC_max_thread_index if necessary.  The following is     */
      /* safe, and unlike CompareExchange-based solutions seems to work */
      /* on all Windows95 and later platforms.                          */
      /* Unfortunately, GC_max_thread_index may be temporarily out of   */
      /* bounds, so readers have to compensate.                         */
      while (i > GC_max_thread_index) {
        InterlockedIncrement((IE_t)&GC_max_thread_index);
      }
      if (EXPECT(GC_max_thread_index >= MAX_THREADS, FALSE)) {
        /* We overshot due to simultaneous increments.  */
        /* Setting it to MAX_THREADS-1 is always safe.  */
        GC_max_thread_index = MAX_THREADS - 1;
      }
      me = dll_thread_table + i;
    } else
# endif
  /* else */ /* Not using DllMain */ {
    GC_ASSERT(I_HOLD_LOCK());
    GC_in_thread_creation = TRUE; /* OK to collect from unknown thread. */
    me = GC_new_thread(id);
    GC_in_thread_creation = FALSE;
    if (NULL == me)
      ABORT("Failed to allocate memory for thread registering");
  }
# ifdef GC_PTHREADS
    me -> pthread_id = pthread_self();
# endif
# ifndef MSWINCE
    /* GetCurrentThread() returns a pseudohandle (a const value).       */
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(),
                        (HANDLE*)&(me -> handle),
                        0 /* dwDesiredAccess */, FALSE /* bInheritHandle */,
                        DUPLICATE_SAME_ACCESS)) {
        ABORT_ARG1("DuplicateHandle failed",
                   ": errcode= 0x%X", (unsigned)GetLastError());
    }
# endif
  me -> last_stack_min = ADDR_LIMIT;
  GC_record_stack_base((GC_thread)me, sb);
  /* Up until this point, GC_push_all_stacks considers this thread      */
  /* invalid.                                                           */
  /* Up until this point, this entry is viewed as reserved but invalid  */
  /* by GC_delete_thread.                                               */
  me -> id = id;
# ifndef GC_NO_THREADS_DISCOVERY
    if (GC_win32_dll_threads) {
      if (GC_please_stop) {
        AO_store(&GC_attached_thread, TRUE);
        AO_nop_full(); /* Later updates must become visible after this. */
      }
      /* We'd like to wait here, but can't, since waiting in DllMain    */
      /* provokes deadlocks.                                            */
      /* Thus we force marking to be restarted instead.                 */
    } else
# endif
  /* else */ {
    GC_ASSERT(!GC_please_stop);
        /* Otherwise both we and the thread stopping code would be      */
        /* holding the allocation lock.                                 */
  }
  return (GC_thread)me;
}

/*
 * GC_max_thread_index may temporarily be larger than MAX_THREADS.
 * To avoid subscript errors, we check on access.
 */
GC_INLINE LONG GC_get_max_thread_index(void)
{
  LONG my_max = GC_max_thread_index;
  if (EXPECT(my_max >= MAX_THREADS, FALSE)) return MAX_THREADS - 1;
  return my_max;
}

/* Return the GC_thread corresponding to a thread id.                   */
/* May be called without a lock, but should be called in contexts in    */
/* which the requested thread cannot be asynchronously deleted, e.g.    */
/* from the thread itself.                                              */
GC_INNER GC_thread GC_lookup_thread(thread_id_t id)
{
# ifndef GC_NO_THREADS_DISCOVERY
    if (GC_win32_dll_threads) {
      int i;
      LONG my_max = GC_get_max_thread_index();

      for (i = 0; i <= my_max; i++) {
        GC_vthread t = dll_thread_table + i;
        if (AO_load_acquire(&(t -> tm.in_use)) && t -> id == id)
          break;    /* Must still be in use, since nobody else can      */
                    /* store our thread id.                             */
      }
      return i <= my_max ? (GC_thread)(dll_thread_table + i) : NULL;
    } else
# endif
  /* else */ {
    GC_thread p;

    GC_ASSERT(I_HOLD_LOCK());
    for (p = GC_threads[THREAD_TABLE_INDEX(id)];
         p != NULL; p = p -> tm.next) {
      if (p -> id == id) break;
    }
    return p;
  }
}

#ifdef LINT2
# define CHECK_LOOKUP_MY_THREAD(me) \
        if (!(me)) ABORT("GC_lookup_thread(GetCurrentThreadId) failed")
#else
# define CHECK_LOOKUP_MY_THREAD(me) /* empty */
#endif

#ifndef GC_NO_FINALIZATION
  /* Called by GC_finalize() (in case of an allocation failure observed). */
  /* GC_reset_finalizer_nested() is the same as in pthread_support.c.     */
  GC_INNER void GC_reset_finalizer_nested(void)
  {
    GC_thread me = GC_lookup_thread(GetCurrentThreadId());

    CHECK_LOOKUP_MY_THREAD(me);
    me->finalizer_nested = 0;
  }

  /* GC_check_finalizer_nested() is the same as in pthread_support.c.   */
  GC_INNER unsigned char *GC_check_finalizer_nested(void)
  {
    GC_thread me;
    unsigned nesting_level;

    GC_ASSERT(I_HOLD_LOCK());
    me = GC_lookup_thread(GetCurrentThreadId());
    CHECK_LOOKUP_MY_THREAD(me);
    nesting_level = me->finalizer_nested;
    if (nesting_level) {
      /* We are inside another GC_invoke_finalizers().          */
      /* Skip some implicitly-called GC_invoke_finalizers()     */
      /* depending on the nesting (recursion) level.            */
      if (++me->finalizer_skipped < (1U << nesting_level)) return NULL;
      me->finalizer_skipped = 0;
    }
    me->finalizer_nested = (unsigned char)(nesting_level + 1);
    return &me->finalizer_nested;
  }
#endif /* !GC_NO_FINALIZATION */

#if defined(GC_ASSERTIONS) && defined(THREAD_LOCAL_ALLOC)
  /* This is called from thread-local GC_malloc(). */
  GC_bool GC_is_thread_tsd_valid(void *tsd)
  {
    GC_thread me;
    DCL_LOCK_STATE;

    LOCK();
    me = GC_lookup_thread(GetCurrentThreadId());
    UNLOCK();
    return (word)tsd >= (word)(&me->tlfs)
            && (word)tsd < (word)(&me->tlfs) + sizeof(me->tlfs);
  }
#endif /* GC_ASSERTIONS && THREAD_LOCAL_ALLOC */

GC_API int GC_CALL GC_thread_is_registered(void)
{
    thread_id_t self_id = GetCurrentThreadId();
    GC_thread me;
    DCL_LOCK_STATE;

    LOCK();
    me = GC_lookup_thread(self_id);
    UNLOCK();
    return me != NULL;
}

GC_API void GC_CALL GC_register_altstack(void *normstack,
                GC_word normstack_size, void *altstack, GC_word altstack_size)
{
  /* TODO: Implement */
  UNUSED_ARG(normstack);
  UNUSED_ARG(normstack_size);
  UNUSED_ARG(altstack);
  UNUSED_ARG(altstack_size);
}

/* Make sure thread descriptor t is not protected by the VDB            */
/* implementation.                                                      */
/* Used to prevent write faults when the world is (partially) stopped,  */
/* since it may have been stopped with a system lock held, and that     */
/* lock may be required for fault handling.                             */
#if defined(MPROTECT_VDB)
# define UNPROTECT_THREAD(t) \
    if (!GC_win32_dll_threads && GC_auto_incremental \
        && t != &first_thread) { \
      GC_ASSERT(SMALL_OBJ(GC_size(t))); \
      GC_remove_protection(HBLKPTR(t), 1, FALSE); \
    } else (void)0
#else
# define UNPROTECT_THREAD(t) (void)0
#endif

#ifdef CYGWIN32
# define GC_PTHREAD_PTRVAL(pthread_id) pthread_id
#elif defined(GC_WIN32_PTHREADS) || defined(GC_PTHREADS_PARAMARK)
# if defined(__WINPTHREADS_VERSION_MAJOR)
#   define GC_PTHREAD_PTRVAL(pthread_id) pthread_id
# else
#   define GC_PTHREAD_PTRVAL(pthread_id) pthread_id.p
# endif
#endif

/* If a thread has been joined, but we have not yet             */
/* been notified, then there may be more than one thread        */
/* in the table with the same Win32 thread id.                  */
/* This is OK, but we need a way to delete a specific one.      */
/* Does not actually free GC_thread entry, only unlinks it.     */
/* If GC_win32_dll_threads is set it should be called from the  */
/* thread being deleted.                                        */
STATIC void GC_delete_gc_thread_no_free(GC_vthread t)
{
# ifndef MSWINCE
    CloseHandle(t->handle);
# endif
# ifndef GC_NO_THREADS_DISCOVERY
    if (GC_win32_dll_threads) {
      /* This is intended to be lock-free.                              */
      /* It is either called synchronously from the thread being        */
      /* deleted, or by the joining thread.                             */
      /* In this branch asynchronous changes to (*t) are possible.      */
      /* It's not allowed to call GC_printf (and the friends) here,     */
      /* see GC_stop_world() for the information.                       */
      t -> stack_end = NULL;
      t -> id = 0;
      t -> flags = 0; /* !IS_SUSPENDED */
#     ifdef RETRY_GET_THREAD_CONTEXT
        t -> context_sp = NULL;
#     endif
      AO_store_release(&t->tm.in_use, FALSE);
    } else
# endif
  /* else */ {
                /* Cast away volatile qualifier, since we have lock.    */
    int hv = THREAD_TABLE_INDEX(((GC_thread)t) -> id);
    GC_thread p = GC_threads[hv];
    GC_thread prev = NULL;

    GC_ASSERT(I_HOLD_LOCK());
    while (p != (GC_thread)t) {
      prev = p;
      p = p -> tm.next;
    }
    if (prev == 0) {
      GC_threads[hv] = p -> tm.next;
    } else {
      GC_ASSERT(prev != &first_thread);
      prev -> tm.next = p -> tm.next;
      GC_dirty(prev);
    }
  }
}

/* Delete a thread from GC_threads.  We assume it is there.     */
/* (The code intentionally traps if it wasn't.)                 */
/* If GC_win32_dll_threads is set then it should be called from */
/* the thread being deleted.  It is also safe to delete the     */
/* main thread (unless GC_win32_dll_threads).                   */
STATIC void GC_delete_thread(thread_id_t id)
{
  if (GC_win32_dll_threads) {
    GC_vthread t = GC_lookup_thread(id);

    if (EXPECT(NULL == t, FALSE)) {
      WARN("Removing nonexistent thread, id= %" WARN_PRIuPTR "\n", id);
    } else {
      GC_delete_gc_thread_no_free(t);
    }
  } else {
    int hv = THREAD_TABLE_INDEX(id);
    GC_thread p;
    GC_thread prev = NULL;

    GC_ASSERT(I_HOLD_LOCK());
    for (p = GC_threads[hv]; ; p = p -> tm.next) {
      if (p -> id == id) break;
      prev = p;
    }
#   ifndef MSWINCE
      CloseHandle(p->handle);
#   endif
    if (NULL == prev) {
      GC_threads[hv] = p -> tm.next;
    } else {
      GC_ASSERT(prev != &first_thread);
      prev -> tm.next = p -> tm.next;
      GC_dirty(prev);
    }
    if (EXPECT(p != &first_thread, TRUE)) {
      GC_INTERNAL_FREE(p);
    }
  }
}

GC_API void GC_CALL GC_allow_register_threads(void)
{
# ifdef GC_ASSERTIONS
    DCL_LOCK_STATE;

    /* Check GC is initialized and the current thread is registered. */
    LOCK();
    GC_ASSERT(GC_lookup_thread(GetCurrentThreadId()) != 0);
    UNLOCK();
# endif
  GC_start_mark_threads();
  set_need_to_lock();
}

GC_API int GC_CALL GC_register_my_thread(const struct GC_stack_base *sb)
{
  GC_thread me;
  thread_id_t self_id = GetCurrentThreadId();
  DCL_LOCK_STATE;

  if (GC_need_to_lock == FALSE)
    ABORT("Threads explicit registering is not previously enabled");

  /* We lock here, since we want to wait for an ongoing GC.     */
  LOCK();
  me = GC_lookup_thread(self_id);
  if (EXPECT(NULL == me, TRUE)) {
    me = GC_register_my_thread_inner(sb, self_id);
#   ifdef GC_PTHREADS
#     if defined(CPPCHECK)
        GC_noop1(me->flags);
#     endif
      me -> flags |= DETACHED;
          /* Treat as detached, since we do not need to worry about     */
          /* pointer results.                                           */
#   else
      (void)me;
#   endif
  } else
#   ifdef GC_PTHREADS
      /* else */ if (KNOWN_FINISHED(me)) {
        GC_record_stack_base(me, sb);
        me -> flags &= ~FINISHED; /* but not DETACHED */
      } else
#   endif
  /* else */ {
    UNLOCK();
    return GC_DUPLICATE;
  }

# ifdef THREAD_LOCAL_ALLOC
    GC_init_thread_local(&me->tlfs);
# endif
  UNLOCK();
  return GC_SUCCESS;
}

#ifdef GC_DISABLE_INCREMENTAL
# define GC_wait_for_gc_completion(wait_for_all) (void)(wait_for_all)
#else
/* Similar to that in pthread_support.c.        */
STATIC void GC_wait_for_gc_completion(GC_bool wait_for_all)
{
  GC_ASSERT(I_HOLD_LOCK());
  if (GC_incremental && GC_collection_in_progress()) {
    word old_gc_no = GC_gc_no;

    /* Make sure that no part of our stack is still on the mark stack,  */
    /* since it's about to be unmapped.                                 */
    do {
      ENTER_GC();
      GC_in_thread_creation = TRUE;
      GC_collect_a_little_inner(1);
      GC_in_thread_creation = FALSE;
      EXIT_GC();

      UNLOCK();
      Sleep(0); /* yield */
      LOCK();
    } while (GC_incremental && GC_collection_in_progress()
             && (wait_for_all || old_gc_no == GC_gc_no));
  }
}
#endif /* !GC_DISABLE_INCREMENTAL */

GC_API int GC_CALL GC_unregister_my_thread(void)
{
  DCL_LOCK_STATE;

# ifdef DEBUG_THREADS
    GC_log_printf("Unregistering thread 0x%lx\n", (long)GetCurrentThreadId());
# endif

  if (GC_win32_dll_threads) {
#   if defined(THREAD_LOCAL_ALLOC)
      /* Can't happen: see GC_use_threads_discovery(). */
      GC_ASSERT(FALSE);
#   else
      /* FIXME: Should we just ignore this? */
      GC_delete_thread(GetCurrentThreadId());
#   endif
  } else {
#   if defined(THREAD_LOCAL_ALLOC) || defined(GC_PTHREADS)
      GC_thread me;
#   endif
    thread_id_t self_id = GetCurrentThreadId();

    LOCK();
    GC_wait_for_gc_completion(FALSE);
#   if defined(THREAD_LOCAL_ALLOC) || defined(GC_PTHREADS)
      me = GC_lookup_thread(self_id);
      CHECK_LOOKUP_MY_THREAD(me);
      GC_ASSERT(!KNOWN_FINISHED(me));
#   endif
#   if defined(THREAD_LOCAL_ALLOC)
      GC_ASSERT(GC_getspecific(GC_thread_key) == &me->tlfs);
      GC_destroy_thread_local(&me->tlfs);
#   endif
#   ifdef GC_PTHREADS
      if ((me -> flags & DETACHED) == 0) {
        me -> flags |= FINISHED;
      } else
#   endif
    /* else */ {
      GC_delete_thread(self_id);
    }
#   if defined(THREAD_LOCAL_ALLOC)
      /* It is required to call remove_specific defined in specific.c. */
      GC_remove_specific(GC_thread_key);
#   endif
    UNLOCK();
  }
  return GC_SUCCESS;
}

/* Wrapper for functions that are likely to block for an appreciable    */
/* length of time.                                                      */

/* GC_do_blocking_inner() is nearly the same as in pthread_support.c    */
GC_INNER void GC_do_blocking_inner(ptr_t data, void *context)
{
  struct blocking_data * d = (struct blocking_data *)data;
  thread_id_t self_id = GetCurrentThreadId();
  GC_thread me;
# ifdef IA64
    ptr_t bs_hi = GC_save_regs_in_stack();
# endif
  DCL_LOCK_STATE;

  UNUSED_ARG(context);
  LOCK();
  me = GC_lookup_thread(self_id);
  CHECK_LOOKUP_MY_THREAD(me);
  GC_ASSERT((me -> flags & DO_BLOCKING) == 0);
# ifdef IA64
    me -> backing_store_ptr = bs_hi;
# endif
  me -> stack_ptr = (ptr_t)(&d); /* save approx. sp */
  /* Save context here if we want to support precise stack marking */
  me -> flags |= DO_BLOCKING;
  UNLOCK();
  d -> client_data = (d -> fn)(d -> client_data);
  LOCK();   /* This will block if the world is stopped. */
  me -> flags &= ~DO_BLOCKING;
  UNLOCK();
}

/* GC_call_with_gc_active() has the opposite to GC_do_blocking()        */
/* functionality.  It might be called from a user function invoked by   */
/* GC_do_blocking() to temporarily back allow calling any GC function   */
/* and/or manipulating pointers to the garbage collected heap.          */
GC_API void * GC_CALL GC_call_with_gc_active(GC_fn_type fn,
                                             void * client_data)
{
  struct GC_traced_stack_sect_s stacksect;
  thread_id_t self_id = GetCurrentThreadId();
  GC_thread me;
  DCL_LOCK_STATE;

  LOCK();   /* This will block if the world is stopped.         */
  me = GC_lookup_thread(self_id);
  CHECK_LOOKUP_MY_THREAD(me);
  /* Adjust our stack bottom pointer (this could happen unless  */
  /* GC_get_stack_base() was used which returned GC_SUCCESS).   */
  GC_ASSERT(me -> stack_end != NULL);
  if ((word)me -> stack_end < (word)(&stacksect)) {
    me -> stack_end = (ptr_t)(&stacksect);
#   if defined(I386)
      me -> initial_stack_base = me -> stack_end;
#   endif
  }

  if ((me -> flags & DO_BLOCKING) == 0) {
    /* We are not inside GC_do_blocking() - do nothing more.    */
    UNLOCK();
    client_data = fn(client_data);
    /* Prevent treating the above as a tail call.       */
    GC_noop1(COVERT_DATAFLOW(&stacksect));
    return client_data; /* result */
  }

  /* Setup new "stack section". */
  stacksect.saved_stack_ptr = me -> stack_ptr;
# ifdef IA64
    /* This is the same as in GC_call_with_stack_base().        */
    stacksect.backing_store_end = GC_save_regs_in_stack();
    /* Unnecessarily flushes register stack,    */
    /* but that probably doesn't hurt.          */
    stacksect.saved_backing_store_ptr = me -> backing_store_ptr;
# endif
  stacksect.prev = me -> traced_stack_sect;
  me -> flags &= ~DO_BLOCKING;
  me -> traced_stack_sect = &stacksect;

  UNLOCK();
  client_data = fn(client_data);
  GC_ASSERT((me -> flags & DO_BLOCKING) == 0);
  GC_ASSERT(me -> traced_stack_sect == &stacksect);

  /* Restore original "stack section".  */
  LOCK();
# if defined(CPPCHECK)
    GC_noop1((word)me->traced_stack_sect);
# endif
  me -> traced_stack_sect = stacksect.prev;
# ifdef IA64
    me -> backing_store_ptr = stacksect.saved_backing_store_ptr;
# endif
  me -> flags |= DO_BLOCKING;
  me -> stack_ptr = stacksect.saved_stack_ptr;
  UNLOCK();

  return client_data; /* result */
}

GC_API void GC_CALL GC_set_stackbottom(void *gc_thread_handle,
                                       const struct GC_stack_base *sb)
{
  GC_thread t = (GC_thread)gc_thread_handle;

  GC_ASSERT(sb -> mem_base != NULL);
  if (!EXPECT(GC_is_initialized, TRUE)) {
    GC_ASSERT(NULL == t);
    GC_stackbottom = (char *)sb->mem_base;
#   ifdef IA64
      GC_register_stackbottom = (ptr_t)sb->reg_base;
#   endif
    return;
  }

  GC_ASSERT(I_HOLD_LOCK());
  if (NULL == t) { /* current thread? */
    t = GC_lookup_thread(GetCurrentThreadId());
    CHECK_LOOKUP_MY_THREAD(t);
  }
  GC_ASSERT(!KNOWN_FINISHED(t));
  GC_ASSERT((t -> flags & DO_BLOCKING) == 0
            && NULL == t -> traced_stack_sect); /* for now */
  t -> stack_end = (ptr_t)sb->mem_base;
  t -> last_stack_min = ADDR_LIMIT; /* reset the known minimum */
# ifdef IA64
    t -> backing_store_end = (ptr_t)sb->reg_base;
# endif
}

GC_API void * GC_CALL GC_get_my_stackbottom(struct GC_stack_base *sb)
{
  thread_id_t self_id = GetCurrentThreadId();
  GC_thread me;
  DCL_LOCK_STATE;

  LOCK();
  me = GC_lookup_thread(self_id);
  CHECK_LOOKUP_MY_THREAD(me); /* the thread is assumed to be registered */
  sb -> mem_base = me -> stack_end;
# ifdef IA64
    sb -> reg_base = me -> backing_store_end;
# endif
  UNLOCK();
  return (void *)me; /* gc_thread_handle */
}

#ifdef GC_PTHREADS

  /* A quick-and-dirty cache of the mapping between pthread_t   */
  /* and Win32 thread id.                                       */
# define PTHREAD_MAP_SIZE 512
  thread_id_t GC_pthread_map_cache[PTHREAD_MAP_SIZE] = {0};
# define PTHREAD_MAP_INDEX(pthread_id) \
                ((NUMERIC_THREAD_ID(pthread_id) >> 5) % PTHREAD_MAP_SIZE)
        /* It appears pthread_t is really a pointer type ... */
# define SET_PTHREAD_MAP_CACHE(pthread_id, win32_id) \
      (void)(GC_pthread_map_cache[PTHREAD_MAP_INDEX(pthread_id)] = (win32_id))
# define GET_PTHREAD_MAP_CACHE(pthread_id) \
          GC_pthread_map_cache[PTHREAD_MAP_INDEX(pthread_id)]

  /* Return a GC_thread corresponding to a given pthread_t.     */
  /* Returns NULL if it is not there.                           */
  /* We assume that this is only called for pthread ids that    */
  /* have not yet terminated or are still joinable, and         */
  /* cannot be terminated concurrently.                         */
  STATIC GC_thread GC_lookup_by_pthread(pthread_t thread)
  {
      /* TODO: search in dll_thread_table instead when DllMain-based    */
      /* thread registration is made compatible with pthreads (and      */
      /* turned on).                                                    */

      /* We first try the cache.  If that fails, we use a very slow     */
      /* approach.                                                      */
      int hv_guess = THREAD_TABLE_INDEX(GET_PTHREAD_MAP_CACHE(thread));
      GC_thread p;
      DCL_LOCK_STATE;

      LOCK();
      for (p = GC_threads[hv_guess]; p != NULL; p = p -> tm.next) {
        if (THREAD_EQUAL(p -> pthread_id, thread))
          break;
      }

      if (EXPECT(NULL == p, FALSE)) {
        int hv;

        for (hv = 0; hv < THREAD_TABLE_SZ; ++hv) {
          for (p = GC_threads[hv]; p != NULL; p = p -> tm.next) {
            if (THREAD_EQUAL(p -> pthread_id, thread))
              break;
          }
          if (p != NULL) break;
        }
      }
      UNLOCK();
      return p;
  }

#endif /* GC_PTHREADS */

#ifdef CAN_HANDLE_FORK
    /* Similar to that in pthread_support.c but also rehashes the table */
    /* since hash map key (thread id) differs from that in the parent.  */
    STATIC void GC_remove_all_threads_but_me(void)
    {
      int hv;
      GC_thread me = NULL;
      thread_id_t self_id;
      pthread_t self = pthread_self(); /* same as in parent */

      GC_ASSERT(!GC_win32_dll_threads);
      for (hv = 0; hv < THREAD_TABLE_SZ; ++hv) {
        GC_thread p, next;

        for (p = GC_threads[hv]; p != NULL; p = next) {
          next = p -> tm.next;
          if (THREAD_EQUAL(p -> pthread_id, self)
              && me == NULL) { /* ignore dead threads with the same id */
            me = p;
            p -> tm.next = 0;
          } else {
#           ifdef THREAD_LOCAL_ALLOC
              if (!KNOWN_FINISHED(p)) {
                /* Cannot call GC_destroy_thread_local here (see the    */
                /* corresponding comment in pthread_support.c).         */
                GC_remove_specific_after_fork(GC_thread_key, p -> pthread_id);
              }
#           endif
            if (&first_thread != p)
              GC_INTERNAL_FREE(p);
          }
        }
        GC_threads[hv] = NULL;
      }

      /* Put "me" back to GC_threads.   */
      GC_ASSERT(me != NULL);
      self_id = GetCurrentThreadId(); /* differs from that in parent */
      GC_threads[THREAD_TABLE_INDEX(self_id)] = me;

      /* Update Win32 thread Id and handle.     */
      me -> id = self_id;
#     ifndef MSWINCE
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), (HANDLE *)&me->handle,
                        0 /* dwDesiredAccess */, FALSE /* bInheritHandle */,
                        DUPLICATE_SAME_ACCESS))
          ABORT("DuplicateHandle failed");
#     endif

#     if defined(THREAD_LOCAL_ALLOC) && !defined(USE_CUSTOM_SPECIFIC)
        /* For Cygwin, we need to re-assign thread-local pointer to     */
        /* 'tlfs' (it is OK to call GC_destroy_thread_local and         */
        /* GC_free_internal before this action).                        */
        if (GC_setspecific(GC_thread_key, &me->tlfs) != 0)
          ABORT("GC_setspecific failed (in child)");
#     endif
    }

    static void fork_prepare_proc(void)
    {
      LOCK();
#     ifdef PARALLEL_MARK
        if (GC_parallel)
          GC_wait_for_reclaim();
#     endif
      GC_wait_for_gc_completion(TRUE);
#     ifdef PARALLEL_MARK
        if (GC_parallel)
          GC_acquire_mark_lock();
#     endif
    }

    static void fork_parent_proc(void)
    {
#     ifdef PARALLEL_MARK
        if (GC_parallel)
          GC_release_mark_lock();
#     endif
      UNLOCK();
    }

    static void fork_child_proc(void)
    {
#     ifdef PARALLEL_MARK
        if (GC_parallel) {
          GC_release_mark_lock();
          GC_parallel = FALSE; /* or GC_markers_m1 = 0 */
                /* Turn off parallel marking in the child, since we are */
                /* probably just going to exec, and we would have to    */
                /* restart mark threads.                                */
        }
#     endif
      GC_remove_all_threads_but_me();
      UNLOCK();
    }

  /* Routines for fork handling by client (no-op if pthread_atfork works). */
  GC_API void GC_CALL GC_atfork_prepare(void)
  {
    if (!EXPECT(GC_is_initialized, TRUE)) GC_init();
    if (GC_handle_fork <= 0)
      fork_prepare_proc();
  }

  GC_API void GC_CALL GC_atfork_parent(void)
  {
    if (GC_handle_fork <= 0)
      fork_parent_proc();
  }

  GC_API void GC_CALL GC_atfork_child(void)
  {
    if (GC_handle_fork <= 0)
      fork_child_proc();
  }

  /* Prepare for forks if requested.    */
  STATIC void GC_setup_atfork(void)
  {
    if (GC_handle_fork) {
#     ifdef CAN_CALL_ATFORK
        if (pthread_atfork(fork_prepare_proc, fork_parent_proc,
                           fork_child_proc) == 0) {
          /* Handlers successfully registered.  */
          GC_handle_fork = 1;
        } else
#     endif
      /* else */ if (GC_handle_fork != -1)
        ABORT("pthread_atfork failed");
    }
  }
#endif /* CAN_HANDLE_FORK */

void GC_push_thread_structures(void)
{
  GC_ASSERT(I_HOLD_LOCK());
# ifndef GC_NO_THREADS_DISCOVERY
    if (GC_win32_dll_threads) {
      /* Unlike the other threads implementations, the thread table     */
      /* here contains no pointers to the collectible heap (note also   */
      /* that GC_PTHREADS is incompatible with DllMain-based thread     */
      /* registration).  Thus we have no private structures we need     */
      /* to preserve.                                                   */
    } else
# endif
  /* else */ {
    GC_PUSH_ALL_SYM(GC_threads);
  }
# if defined(THREAD_LOCAL_ALLOC) && defined(USE_CUSTOM_SPECIFIC)
    GC_PUSH_ALL_SYM(GC_thread_key);
    /* Just in case we ever use our own TLS implementation.     */
# endif
}

#ifdef WOW64_THREAD_CONTEXT_WORKAROUND
# ifndef CONTEXT_EXCEPTION_ACTIVE
#   define CONTEXT_EXCEPTION_ACTIVE    0x08000000
#   define CONTEXT_EXCEPTION_REQUEST   0x40000000
#   define CONTEXT_EXCEPTION_REPORTING 0x80000000
# endif
  static BOOL isWow64;  /* Is running 32-bit code on Win64?     */
# define GET_THREAD_CONTEXT_FLAGS (isWow64 \
                        ? CONTEXT_INTEGER | CONTEXT_CONTROL \
                          | CONTEXT_EXCEPTION_REQUEST | CONTEXT_SEGMENTS \
                        : CONTEXT_INTEGER | CONTEXT_CONTROL)
#else
# define GET_THREAD_CONTEXT_FLAGS (CONTEXT_INTEGER | CONTEXT_CONTROL)
#endif /* !WOW64_THREAD_CONTEXT_WORKAROUND */

/* Suspend the given thread, if it's still active.      */
STATIC void GC_suspend(GC_thread t)
{
# ifndef MSWINCE
    DWORD exitCode;
#   ifdef RETRY_GET_THREAD_CONTEXT
      int retry_cnt;
#     define MAX_SUSPEND_THREAD_RETRIES (1000 * 1000)
#   endif
# endif

# ifdef DEBUG_THREADS
    GC_log_printf("Suspending 0x%x\n", (int)t->id);
# endif
  UNPROTECT_THREAD(t);
  GC_acquire_dirty_lock();

# ifdef MSWINCE
    /* SuspendThread() will fail if thread is running kernel code.      */
    while (SuspendThread(THREAD_HANDLE(t)) == (DWORD)-1) {
      GC_release_dirty_lock();
      Sleep(10); /* in millis */
      GC_acquire_dirty_lock();
    }
# elif defined(RETRY_GET_THREAD_CONTEXT)
    for (retry_cnt = 0;;) {
      /* Apparently the Windows 95 GetOpenFileName call creates         */
      /* a thread that does not properly get cleaned up, and            */
      /* SuspendThread on its descriptor may provoke a crash.           */
      /* This reduces the probability of that event, though it still    */
      /* appears there is a race here.                                  */
      if (GetExitCodeThread(t -> handle, &exitCode)
          && exitCode != STILL_ACTIVE) {
        GC_release_dirty_lock();
#       ifdef GC_PTHREADS
          t -> stack_end = NULL; /* prevent stack from being pushed */
#       else
          /* This breaks pthread_join on Cygwin, which is guaranteed to */
          /* only see user threads.                                     */
          GC_ASSERT(GC_win32_dll_threads);
          GC_delete_gc_thread_no_free(t);
#       endif
        return;
      }

      if (SuspendThread(t->handle) != (DWORD)-1) {
        CONTEXT context;

        context.ContextFlags = GET_THREAD_CONTEXT_FLAGS;
        if (GetThreadContext(t->handle, &context)) {
          /* TODO: WoW64 extra workaround: if CONTEXT_EXCEPTION_ACTIVE  */
          /* then Sleep(1) and retry.                                   */
          t->context_sp = copy_ptr_regs(t->context_regs, &context);
          break; /* success; the context pointer registers are saved */
        }

        /* Resume the thread, try to suspend it in a better location.   */
        if (ResumeThread(t->handle) == (DWORD)-1)
          ABORT("ResumeThread failed in suspend loop");
      }
      if (retry_cnt > 1) {
        GC_release_dirty_lock();
        Sleep(0); /* yield */
        GC_acquire_dirty_lock();
      }
      if (++retry_cnt >= MAX_SUSPEND_THREAD_RETRIES)
        ABORT("SuspendThread loop failed"); /* something must be wrong */
    }
# else
    if (GetExitCodeThread(t -> handle, &exitCode)
        && exitCode != STILL_ACTIVE) {
      GC_release_dirty_lock();
#     ifdef GC_PTHREADS
        t -> stack_end = NULL; /* prevent stack from being pushed */
#     else
        GC_ASSERT(GC_win32_dll_threads);
        GC_delete_gc_thread_no_free(t);
#     endif
      return;
    }
    if (SuspendThread(t -> handle) == (DWORD)-1)
      ABORT("SuspendThread failed");
# endif
  t -> flags |= IS_SUSPENDED;
  GC_release_dirty_lock();
  if (GC_on_thread_event)
    GC_on_thread_event(GC_EVENT_THREAD_SUSPENDED, THREAD_HANDLE(t));
}

#if defined(GC_ASSERTIONS) \
    && ((defined(MSWIN32) && !defined(CONSOLE_LOG)) || defined(MSWINCE))
  GC_INNER GC_bool GC_write_disabled = FALSE;
                /* TRUE only if GC_stop_world() acquired GC_write_cs.   */
#endif

GC_INNER void GC_stop_world(void)
{
  thread_id_t self_id = GetCurrentThreadId();

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_thr_initialized);

  /* This code is the same as in pthread_stop_world.c */
# ifdef PARALLEL_MARK
    if (GC_parallel) {
      GC_acquire_mark_lock();
      GC_ASSERT(GC_fl_builder_count == 0);
      /* We should have previously waited for it to become zero. */
    }
# endif /* PARALLEL_MARK */

# if !defined(GC_NO_THREADS_DISCOVERY) || defined(GC_ASSERTIONS)
    GC_please_stop = TRUE;
# endif
# if (defined(MSWIN32) && !defined(CONSOLE_LOG)) || defined(MSWINCE)
    GC_ASSERT(!GC_write_disabled);
    EnterCriticalSection(&GC_write_cs);
    /* It's not allowed to call GC_printf() (and friends) here down to  */
    /* LeaveCriticalSection (same applies recursively to GC_suspend,    */
    /* GC_delete_gc_thread_no_free, GC_get_max_thread_index, GC_size    */
    /* and GC_remove_protection).                                       */
#   ifdef GC_ASSERTIONS
      GC_write_disabled = TRUE;
#   endif
# endif
# ifndef GC_NO_THREADS_DISCOVERY
    if (GC_win32_dll_threads) {
      int i;
      int my_max;
      /* Any threads being created during this loop will end up setting */
      /* GC_attached_thread when they start.  This will force marking   */
      /* to restart.  This is not ideal, but hopefully correct.         */
      AO_store(&GC_attached_thread, FALSE);
      my_max = (int)GC_get_max_thread_index();
      for (i = 0; i <= my_max; i++) {
        GC_thread p = (GC_thread)(dll_thread_table + i);

        if (p -> stack_end != NULL && (p -> flags & DO_BLOCKING) == 0
            && p -> id != self_id) {
          GC_suspend(p);
        }
      }
    } else
# endif
  /* else */ {
    GC_thread p;
    int i;

    for (i = 0; i < THREAD_TABLE_SZ; i++) {
      for (p = GC_threads[i]; p != NULL; p = p -> tm.next)
        if (p -> stack_end != NULL && p -> id != self_id
            && (p -> flags & (FINISHED | DO_BLOCKING)) == 0)
          GC_suspend(p);
    }
  }
# if (defined(MSWIN32) && !defined(CONSOLE_LOG)) || defined(MSWINCE)
#   ifdef GC_ASSERTIONS
      GC_write_disabled = FALSE;
#   endif
    LeaveCriticalSection(&GC_write_cs);
# endif
# ifdef PARALLEL_MARK
    if (GC_parallel)
      GC_release_mark_lock();
# endif
}

GC_INNER void GC_start_world(void)
{
# ifdef GC_ASSERTIONS
    thread_id_t self_id = GetCurrentThreadId();
# endif

  GC_ASSERT(I_HOLD_LOCK());
  if (GC_win32_dll_threads) {
    LONG my_max = GC_get_max_thread_index();
    int i;

    for (i = 0; i <= my_max; i++) {
      GC_thread p = (GC_thread)(dll_thread_table + i);

      if ((p -> flags & IS_SUSPENDED) != 0) {
#       ifdef DEBUG_THREADS
          GC_log_printf("Resuming 0x%x\n", (int)p->id);
#       endif
        GC_ASSERT(p -> stack_end != NULL && p -> id != self_id);
        if (ResumeThread(THREAD_HANDLE(p)) == (DWORD)-1)
          ABORT("ResumeThread failed");
        p -> flags &= ~IS_SUSPENDED;
        if (GC_on_thread_event)
          GC_on_thread_event(GC_EVENT_THREAD_UNSUSPENDED, THREAD_HANDLE(p));
      }
      /* Else thread is unregistered or not suspended. */
    }
  } else {
    GC_thread p;
    int i;

    for (i = 0; i < THREAD_TABLE_SZ; i++) {
      for (p = GC_threads[i]; p != NULL; p = p -> tm.next) {
        if ((p -> flags & IS_SUSPENDED) != 0) {
#         ifdef DEBUG_THREADS
            GC_log_printf("Resuming 0x%x\n", (int)p->id);
#         endif
          GC_ASSERT(p -> stack_end != NULL && p -> id != self_id);
          if (ResumeThread(THREAD_HANDLE(p)) == (DWORD)-1)
            ABORT("ResumeThread failed");
          UNPROTECT_THREAD(p);
          p -> flags &= ~IS_SUSPENDED;
          if (GC_on_thread_event)
            GC_on_thread_event(GC_EVENT_THREAD_UNSUSPENDED, THREAD_HANDLE(p));
        } else {
#         ifdef DEBUG_THREADS
            GC_log_printf("Not resuming thread 0x%x as it is not suspended\n",
                          (int)p->id);
#         endif
        }
      }
    }
  }
# if !defined(GC_NO_THREADS_DISCOVERY) || defined(GC_ASSERTIONS)
    GC_please_stop = FALSE;
# endif
}

#ifdef MSWINCE
  /* The VirtualQuery calls below won't work properly on some old WinCE */
  /* versions, but since each stack is restricted to an aligned 64 KiB  */
  /* region of virtual memory we can just take the next lowest multiple */
  /* of 64 KiB.  The result of this macro must not be used as its       */
  /* argument later and must not be used as the lower bound for sp      */
  /* check (since the stack may be bigger than 64 KiB).                 */
# define GC_wince_evaluate_stack_min(s) \
                        (ptr_t)(((word)(s) - 1) & ~(word)0xFFFF)
#elif defined(GC_ASSERTIONS)
# define GC_dont_query_stack_min FALSE
#endif

/* A cache holding the results of the recent VirtualQuery call. */
/* Protected by the allocation lock.                            */
static ptr_t last_address = 0;
static MEMORY_BASIC_INFORMATION last_info;

/* Probe stack memory region (starting at "s") to find out its  */
/* lowest address (i.e. stack top).                             */
/* S must be a mapped address inside the region, NOT the first  */
/* unmapped address.                                            */
STATIC ptr_t GC_get_stack_min(ptr_t s)
{
  ptr_t bottom;

  GC_ASSERT(I_HOLD_LOCK());
  if (s != last_address) {
    VirtualQuery(s, &last_info, sizeof(last_info));
    last_address = s;
  }
  do {
    bottom = (ptr_t)last_info.BaseAddress;
    VirtualQuery(bottom - 1, &last_info, sizeof(last_info));
    last_address = bottom - 1;
  } while ((last_info.Protect & PAGE_READWRITE)
           && !(last_info.Protect & PAGE_GUARD));
  return bottom;
}

/* Return true if the page at s has protections appropriate     */
/* for a stack page.                                            */
static GC_bool may_be_in_stack(ptr_t s)
{
  GC_ASSERT(I_HOLD_LOCK());
  if (s != last_address) {
    VirtualQuery(s, &last_info, sizeof(last_info));
    last_address = s;
  }
  return (last_info.Protect & PAGE_READWRITE)
          && !(last_info.Protect & PAGE_GUARD);
}

/* Copy all registers that might point into the heap.  Frame    */
/* pointer registers are included in case client code was       */
/* compiled with the 'omit frame pointer' optimization.         */
/* The context register values are stored to regs argument      */
/* which is expected to be of PUSHED_REGS_COUNT length exactly. */
/* The functions returns the context stack pointer value.       */
static ptr_t copy_ptr_regs(word *regs, const CONTEXT *pcontext) {
    ptr_t sp;
    int cnt = 0;
#   define context (*pcontext)
#   define PUSH1(reg) (regs[cnt++] = (word)pcontext->reg)
#   define PUSH2(r1,r2) (PUSH1(r1), PUSH1(r2))
#   define PUSH4(r1,r2,r3,r4) (PUSH2(r1,r2), PUSH2(r3,r4))
#   if defined(I386)
#     ifdef WOW64_THREAD_CONTEXT_WORKAROUND
        PUSH2(ContextFlags, SegFs); /* cannot contain pointers */
#     endif
      PUSH4(Edi,Esi,Ebx,Edx), PUSH2(Ecx,Eax), PUSH1(Ebp);
      sp = (ptr_t)context.Esp;
#   elif defined(X86_64)
      PUSH4(Rax,Rcx,Rdx,Rbx); PUSH2(Rbp, Rsi); PUSH1(Rdi);
      PUSH4(R8, R9, R10, R11); PUSH4(R12, R13, R14, R15);
      sp = (ptr_t)context.Rsp;
#   elif defined(ARM32)
      PUSH4(R0,R1,R2,R3),PUSH4(R4,R5,R6,R7),PUSH4(R8,R9,R10,R11);
      PUSH1(R12);
      sp = (ptr_t)context.Sp;
#   elif defined(AARCH64)
      PUSH4(X0,X1,X2,X3),PUSH4(X4,X5,X6,X7),PUSH4(X8,X9,X10,X11);
      PUSH4(X12,X13,X14,X15),PUSH4(X16,X17,X18,X19),PUSH4(X20,X21,X22,X23);
      PUSH4(X24,X25,X26,X27),PUSH1(X28);
      PUSH1(Lr);
      sp = (ptr_t)context.Sp;
#   elif defined(SHx)
      PUSH4(R0,R1,R2,R3), PUSH4(R4,R5,R6,R7), PUSH4(R8,R9,R10,R11);
      PUSH2(R12,R13), PUSH1(R14);
      sp = (ptr_t)context.R15;
#   elif defined(MIPS)
      PUSH4(IntAt,IntV0,IntV1,IntA0), PUSH4(IntA1,IntA2,IntA3,IntT0);
      PUSH4(IntT1,IntT2,IntT3,IntT4), PUSH4(IntT5,IntT6,IntT7,IntS0);
      PUSH4(IntS1,IntS2,IntS3,IntS4), PUSH4(IntS5,IntS6,IntS7,IntT8);
      PUSH4(IntT9,IntK0,IntK1,IntS8);
      sp = (ptr_t)context.IntSp;
#   elif defined(PPC)
      PUSH4(Gpr0, Gpr3, Gpr4, Gpr5),  PUSH4(Gpr6, Gpr7, Gpr8, Gpr9);
      PUSH4(Gpr10,Gpr11,Gpr12,Gpr14), PUSH4(Gpr15,Gpr16,Gpr17,Gpr18);
      PUSH4(Gpr19,Gpr20,Gpr21,Gpr22), PUSH4(Gpr23,Gpr24,Gpr25,Gpr26);
      PUSH4(Gpr27,Gpr28,Gpr29,Gpr30), PUSH1(Gpr31);
      sp = (ptr_t)context.Gpr1;
#   elif defined(ALPHA)
      PUSH4(IntV0,IntT0,IntT1,IntT2), PUSH4(IntT3,IntT4,IntT5,IntT6);
      PUSH4(IntT7,IntS0,IntS1,IntS2), PUSH4(IntS3,IntS4,IntS5,IntFp);
      PUSH4(IntA0,IntA1,IntA2,IntA3), PUSH4(IntA4,IntA5,IntT8,IntT9);
      PUSH4(IntT10,IntT11,IntT12,IntAt);
      sp = (ptr_t)context.IntSp;
#   elif defined(CPPCHECK)
      sp = (ptr_t)(word)cnt; /* to workaround "cnt not used" false positive */
#   else
#     error Architecture is not supported
#   endif
#   undef context
    GC_ASSERT(cnt == PUSHED_REGS_COUNT);
    return sp;
}

STATIC word GC_push_stack_for(GC_thread thread, thread_id_t self_id,
                              GC_bool *pfound_me)
{
  GC_bool is_self = FALSE;
  ptr_t sp, stack_min;
  struct GC_traced_stack_sect_s *traced_stack_sect =
                                      thread -> traced_stack_sect;
  if (thread -> id == self_id) {
    GC_ASSERT((thread -> flags & DO_BLOCKING) == 0);
    sp = GC_approx_sp();
    is_self = TRUE;
    *pfound_me = TRUE;
  } else if ((thread -> flags & DO_BLOCKING) != 0) {
    /* Use saved sp value for blocked threads.  */
    sp = thread -> stack_ptr;
  } else {
#   ifdef RETRY_GET_THREAD_CONTEXT
      /* We cache context when suspending the thread since it may       */
      /* require looping.                                               */
      word *regs = thread -> context_regs;

      if ((thread -> flags & IS_SUSPENDED) != 0) {
        sp = thread -> context_sp;
      } else
#   else
      word regs[PUSHED_REGS_COUNT];
#   endif

      /* else */ {
        CONTEXT context;

        /* For unblocked threads call GetThreadContext().       */
        context.ContextFlags = GET_THREAD_CONTEXT_FLAGS;
        if (GetThreadContext(THREAD_HANDLE(thread), &context)) {
          sp = copy_ptr_regs(regs, &context);
        } else {
#         ifdef RETRY_GET_THREAD_CONTEXT
            /* At least, try to use the stale context if saved. */
            sp = thread->context_sp;
            if (NULL == sp) {
              /* Skip the current thread, anyway its stack will */
              /* be pushed when the world is stopped.           */
              return 0;
            }
#         else
            *(volatile ptr_t *)&sp = NULL;
                    /* to avoid "might be uninitialized" compiler warning */
            ABORT("GetThreadContext failed");
#         endif
        }
      }
#   ifdef THREAD_LOCAL_ALLOC
      GC_ASSERT((thread -> flags & IS_SUSPENDED) != 0 || !GC_world_stopped);
#   endif

#   ifndef WOW64_THREAD_CONTEXT_WORKAROUND
      GC_push_many_regs(regs, PUSHED_REGS_COUNT);
#   else
      GC_push_many_regs(regs + 2, PUSHED_REGS_COUNT - 2);
                                        /* skip ContextFlags and SegFs */

      /* WoW64 workaround. */
      if (isWow64) {
        DWORD ContextFlags = (DWORD)regs[0];
        WORD SegFs = (WORD)regs[1];

        if ((ContextFlags & CONTEXT_EXCEPTION_REPORTING) != 0
            && (ContextFlags & (CONTEXT_EXCEPTION_ACTIVE
                                /* | CONTEXT_SERVICE_ACTIVE */)) != 0) {
          LDT_ENTRY selector;
          PNT_TIB tib;

          if (!GetThreadSelectorEntry(THREAD_HANDLE(thread), SegFs, &selector))
            ABORT("GetThreadSelectorEntry failed");
          tib = (PNT_TIB)(selector.BaseLow
                          | (selector.HighWord.Bits.BaseMid << 16)
                          | (selector.HighWord.Bits.BaseHi << 24));
#         ifdef DEBUG_THREADS
            GC_log_printf("TIB stack limit/base: %p .. %p\n",
                          (void *)tib->StackLimit, (void *)tib->StackBase);
#         endif
          GC_ASSERT(!((word)(thread -> stack_end)
                      COOLER_THAN (word)tib->StackBase));
          if (thread -> stack_end != thread -> initial_stack_base
              /* We are in a coroutine. */
              && ((word)(thread -> stack_end) <= (word)tib->StackLimit
                  || (word)tib->StackBase < (word)(thread -> stack_end))) {
            /* The coroutine stack is not within TIB stack.   */
            WARN("GetThreadContext might return stale register values"
                 " including ESP= %p\n", sp);
            /* TODO: Because of WoW64 bug, there is no guarantee that   */
            /* sp really points to the stack top but, for now, we do    */
            /* our best as the TIB stack limit/base cannot be used      */
            /* while we are inside a coroutine.                         */
          } else {
            /* GetThreadContext() might return stale register values,   */
            /* so we scan the entire stack region (down to the stack    */
            /* limit).  There is no 100% guarantee that all the         */
            /* registers are pushed but we do our best (the proper      */
            /* solution would be to fix it inside Windows).             */
            sp = (ptr_t)tib->StackLimit;
          }
        } /* else */
#       ifdef DEBUG_THREADS
          else {
            static GC_bool logged;
            if (!logged
                && (ContextFlags & CONTEXT_EXCEPTION_REPORTING) == 0) {
              GC_log_printf("CONTEXT_EXCEPTION_REQUEST not supported\n");
              logged = TRUE;
            }
          }
#       endif
      }
#   endif /* WOW64_THREAD_CONTEXT_WORKAROUND */
  } /* not current thread */

  /* Set stack_min to the lowest address in the thread stack,   */
  /* or to an address in the thread stack no larger than sp,    */
  /* taking advantage of the old value to avoid slow traversals */
  /* of large stacks.                                           */
  if (thread -> last_stack_min == ADDR_LIMIT) {
#   ifdef MSWINCE
      if (GC_dont_query_stack_min) {
        stack_min = GC_wince_evaluate_stack_min(traced_stack_sect != NULL ?
                      (ptr_t)traced_stack_sect : thread -> stack_end);
        /* Keep last_stack_min value unmodified. */
      } else
#   endif
    /* else */ {
      stack_min = GC_get_stack_min(traced_stack_sect != NULL ?
                      (ptr_t)traced_stack_sect : thread -> stack_end);
      UNPROTECT_THREAD(thread);
      thread -> last_stack_min = stack_min;
    }
  } else {
    /* First, adjust the latest known minimum stack address if we       */
    /* are inside GC_call_with_gc_active().                             */
    if (traced_stack_sect != NULL &&
        (word)thread->last_stack_min > (word)traced_stack_sect) {
      UNPROTECT_THREAD(thread);
      thread -> last_stack_min = (ptr_t)traced_stack_sect;
    }

    if ((word)sp < (word)(thread -> stack_end)
        && (word)sp >= (word)thread->last_stack_min) {
      stack_min = sp;
    } else {
      /* In the current thread it is always safe to use sp value.       */
      if (may_be_in_stack(is_self && (word)sp < (word)thread->last_stack_min ?
                          sp : thread -> last_stack_min)) {
        stack_min = (ptr_t)last_info.BaseAddress;
        /* Do not probe rest of the stack if sp is correct. */
        if ((word)sp < (word)stack_min
            || (word)sp >= (word)(thread -> stack_end))
          stack_min = GC_get_stack_min(thread -> last_stack_min);
      } else {
        /* Stack shrunk?  Is this possible? */
        stack_min = GC_get_stack_min(thread -> stack_end);
      }
      UNPROTECT_THREAD(thread);
      thread -> last_stack_min = stack_min;
    }
  }

  GC_ASSERT(GC_dont_query_stack_min
            || stack_min == GC_get_stack_min(thread -> stack_end)
            || ((word)sp >= (word)stack_min
                && (word)stack_min < (word)(thread -> stack_end)
                && (word)stack_min
                        > (word)GC_get_stack_min(thread -> stack_end)));

  if ((word)sp >= (word)stack_min && (word)sp < (word)(thread -> stack_end)) {
#   ifdef DEBUG_THREADS
      GC_log_printf("Pushing stack for 0x%x from sp %p to %p from 0x%x\n",
                    (int)thread->id, (void *)sp,
                    (void *)(thread -> stack_end), (int)self_id);
#   endif
    GC_push_all_stack_sections(sp, thread -> stack_end, traced_stack_sect);
  } else {
    /* If not current thread then it is possible for sp to point to     */
    /* the guarded (untouched yet) page just below the current          */
    /* stack_min of the thread.                                         */
    if (is_self || (word)sp >= (word)(thread -> stack_end)
        || (word)(sp + GC_page_size) < (word)stack_min)
      WARN("Thread stack pointer %p out of range, pushing everything\n", sp);
#   ifdef DEBUG_THREADS
      GC_log_printf("Pushing stack for 0x%x from (min) %p to %p from 0x%x\n",
                    (int)thread->id, (void *)stack_min,
                    (void *)(thread -> stack_end), (int)self_id);
#   endif
    /* Push everything - ignore "traced stack section" data.            */
    GC_push_all_stack(stack_min, thread -> stack_end);
  }
  return thread -> stack_end - sp; /* stack grows down */
}

/* Should do exactly the right thing if the world is stopped; should    */
/* not fail if it is not.                                               */
GC_INNER void GC_push_all_stacks(void)
{
  thread_id_t self_id = GetCurrentThreadId();
  GC_bool found_me = FALSE;
# ifndef SMALL_CONFIG
    unsigned nthreads = 0;
# endif
  word total_size = 0;

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(GC_thr_initialized);
# ifndef GC_NO_THREADS_DISCOVERY
    if (GC_win32_dll_threads) {
      int i;
      LONG my_max = GC_get_max_thread_index();

      for (i = 0; i <= my_max; i++) {
        GC_thread p = (GC_thread)(dll_thread_table + i);

        if (p -> tm.in_use && p -> stack_end != NULL) {
#         ifndef SMALL_CONFIG
            ++nthreads;
#         endif
          total_size += GC_push_stack_for(p, self_id, &found_me);
        }
      }
    } else
# endif
  /* else */ {
    int i;
    for (i = 0; i < THREAD_TABLE_SZ; i++) {
      GC_thread p;

      for (p = GC_threads[i]; p != NULL; p = p -> tm.next)
        if (!KNOWN_FINISHED(p) && p -> stack_end != NULL) {
#         ifndef SMALL_CONFIG
            ++nthreads;
#         endif
          total_size += GC_push_stack_for(p, self_id, &found_me);
        }
    }
  }
# ifndef SMALL_CONFIG
    GC_VERBOSE_LOG_PRINTF("Pushed %d thread stacks%s\n", nthreads,
                          GC_win32_dll_threads ?
                                " based on DllMain thread tracking" : "");
# endif
  if (!found_me && !GC_in_thread_creation)
    ABORT("Collecting from unknown thread");
  GC_total_stacksize = total_size;
}

#ifdef PARALLEL_MARK

# ifndef MAX_MARKERS
#   define MAX_MARKERS 16
# endif

  static ptr_t marker_sp[MAX_MARKERS - 1]; /* The cold end of the stack */
                                           /* for markers.              */

  static ptr_t marker_last_stack_min[MAX_MARKERS - 1];
                                /* Last known minimum (hottest) address */
                                /* in stack (or ADDR_LIMIT if unset)    */
                                /* for markers.                         */

#endif /* PARALLEL_MARK */

/* Find stack with the lowest address which overlaps the        */
/* interval [start, limit).                                     */
/* Return stack bounds in *lo and *hi.  If no such stack        */
/* is found, both *hi and *lo will be set to an address         */
/* higher than limit.                                           */
GC_INNER void GC_get_next_stack(char *start, char *limit,
                                char **lo, char **hi)
{
  int i;
  char * current_min = ADDR_LIMIT;  /* Least in-range stack base      */
  ptr_t *plast_stack_min = NULL;    /* Address of last_stack_min      */
                                    /* field for thread corresponding */
                                    /* to current_min.                */
  GC_thread thread = NULL;          /* Either NULL or points to the   */
                                    /* thread's hash table entry      */
                                    /* containing *plast_stack_min.   */

  /* First set current_min, ignoring limit. */
  if (GC_win32_dll_threads) {
    LONG my_max = GC_get_max_thread_index();

    for (i = 0; i <= my_max; i++) {
      ptr_t s = (ptr_t)dll_thread_table[i].stack_end;

      if ((word)s > (word)start && (word)s < (word)current_min) {
        /* Update address of last_stack_min. */
        plast_stack_min = (ptr_t * /* no volatile */)
                            &dll_thread_table[i].last_stack_min;
        current_min = s;
#       if defined(CPPCHECK)
          /* To avoid a warning that thread is always null.     */
          thread = (GC_thread)&dll_thread_table[i];
#       endif
      }
    }
  } else {
    for (i = 0; i < THREAD_TABLE_SZ; i++) {
      GC_thread p;

      for (p = GC_threads[i]; p != NULL; p = p -> tm.next) {
        ptr_t s = p -> stack_end;

        if ((word)s > (word)start && (word)s < (word)current_min) {
          /* Update address of last_stack_min. */
          plast_stack_min = &(p -> last_stack_min);
          thread = p; /* Remember current thread to unprotect. */
          current_min = s;
        }
      }
    }
#   ifdef PARALLEL_MARK
      for (i = 0; i < GC_markers_m1; ++i) {
        ptr_t s = marker_sp[i];
#       ifdef IA64
          /* FIXME: not implemented */
#       endif
        if ((word)s > (word)start && (word)s < (word)current_min) {
          GC_ASSERT(marker_last_stack_min[i] != NULL);
          plast_stack_min = &marker_last_stack_min[i];
          current_min = s;
          thread = NULL; /* Not a thread's hash table entry. */
        }
      }
#   endif
  }

  *hi = current_min;
  if (current_min == ADDR_LIMIT) {
      *lo = ADDR_LIMIT;
      return;
  }

  GC_ASSERT((word)current_min > (word)start && plast_stack_min != NULL);
# ifdef MSWINCE
    if (GC_dont_query_stack_min) {
      *lo = GC_wince_evaluate_stack_min(current_min);
      /* Keep last_stack_min value unmodified. */
      return;
    }
# endif

  if ((word)current_min > (word)limit && !may_be_in_stack(limit)) {
    /* Skip the rest since the memory region at limit address is        */
    /* not a stack (so the lowest address of the found stack would      */
    /* be above the limit value anyway).                                */
    *lo = ADDR_LIMIT;
    return;
  }

  /* Get the minimum address of the found stack by probing its memory   */
  /* region starting from the recent known minimum (if set).            */
  if (*plast_stack_min == ADDR_LIMIT
      || !may_be_in_stack(*plast_stack_min)) {
    /* Unsafe to start from last_stack_min value. */
    *lo = GC_get_stack_min(current_min);
  } else {
    /* Use the recent value to optimize search for min address. */
    *lo = GC_get_stack_min(*plast_stack_min);
  }

  /* Remember current stack_min value. */
  if (thread != NULL) {
    UNPROTECT_THREAD(thread);
  }
  *plast_stack_min = *lo;
}

#ifdef PARALLEL_MARK

# if !defined(GC_PTHREADS_PARAMARK)
    STATIC HANDLE GC_marker_cv[MAX_MARKERS - 1] = {0};
                        /* Events with manual reset (one for each       */
                        /* mark helper).                                */

    STATIC thread_id_t GC_marker_Id[MAX_MARKERS - 1] = {0};
                        /* This table is used for mapping helper        */
                        /* threads ID to mark helper index (linear      */
                        /* search is used since the mapping contains    */
                        /* only a few entries).                         */
# endif

# if defined(GC_PTHREADS) && defined(HAVE_PTHREAD_SETNAME_NP_WITH_TID)
    static void set_marker_thread_name(unsigned id)
    {
      /* This code is the same as in pthread_support.c. */
      char name_buf[16]; /* pthread_setname_np may fail for longer names */
      int len = sizeof("GC-marker-") - 1;

      /* Compose the name manually as snprintf may be unavailable or    */
      /* "%u directive output may be truncated" warning may occur.      */
      BCOPY("GC-marker-", name_buf, len);
      if (id >= 10)
        name_buf[len++] = (char)('0' + (id / 10) % 10);
      name_buf[len] = (char)('0' + id % 10);
      name_buf[len + 1] = '\0';

      if (pthread_setname_np(pthread_self(), name_buf) != 0)
        WARN("pthread_setname_np failed\n", 0);
    }

# elif !defined(MSWINCE)
    /* A pointer to SetThreadDescription() which is available since     */
    /* Windows 10.  The function prototype is in processthreadsapi.h.   */
    static FARPROC setThreadDescription_fn;

    static void set_marker_thread_name(unsigned id)
    {
      WCHAR name_buf[16];
      int len = sizeof(L"GC-marker-") / sizeof(WCHAR) - 1;
      HRESULT hr;

      if (!setThreadDescription_fn) return; /* missing SetThreadDescription */

      /* Compose the name manually as swprintf may be unavailable.      */
      BCOPY(L"GC-marker-", name_buf, len * sizeof(WCHAR));
      if (id >= 10)
        name_buf[len++] = (WCHAR)('0' + (id / 10) % 10);
      name_buf[len] = (WCHAR)('0' + id % 10);
      name_buf[len + 1] = 0;

      /* Invoke SetThreadDescription().  Cast the function pointer to word  */
      /* first to avoid "incompatible function types" compiler warning.     */
      hr = (*(HRESULT (WINAPI *)(HANDLE, const WCHAR *))
            (word)setThreadDescription_fn)(GetCurrentThread(), name_buf);
      if (FAILED(hr))
        WARN("SetThreadDescription failed\n", 0);
    }
# else
#   define set_marker_thread_name(id) (void)(id)
# endif

  /* GC_mark_thread() is the same as in pthread_support.c */
# ifdef GC_PTHREADS_PARAMARK
    STATIC void * GC_mark_thread(void * id)
# elif defined(MSWINCE)
    STATIC DWORD WINAPI GC_mark_thread(LPVOID id)
# else
    STATIC unsigned __stdcall GC_mark_thread(void * id)
# endif
  {
    word my_mark_no = 0;

    if ((word)id == GC_WORD_MAX) return 0; /* to prevent a compiler warning */
    set_marker_thread_name((unsigned)(word)id);
    marker_sp[(word)id] = GC_approx_sp();
#   if !defined(GC_PTHREADS_PARAMARK)
      GC_marker_Id[(word)id] = GetCurrentThreadId();
#   endif

    /* Inform GC_start_mark_threads about completion of marker data init. */
    GC_acquire_mark_lock();
    if (0 == --GC_fl_builder_count) /* count may have a negative value */
      GC_notify_all_builder();

    for (;; ++my_mark_no) {
      if (my_mark_no - GC_mark_no > (word)2) {
        /* resynchronize if we get far off, e.g. because GC_mark_no     */
        /* wrapped.                                                     */
        my_mark_no = GC_mark_no;
      }
#     ifdef DEBUG_THREADS
        GC_log_printf("Starting helper for mark number %lu (thread %u)\n",
                      (unsigned long)my_mark_no, (unsigned)(word)id);
#     endif
      GC_help_marker(my_mark_no);
    }
  }

# ifndef GC_ASSERTIONS
#   define SET_MARK_LOCK_HOLDER (void)0
#   define UNSET_MARK_LOCK_HOLDER (void)0
# endif

  static int available_markers_m1 = 0;

# ifdef GC_PTHREADS_PARAMARK

#   if defined(GC_ASSERTIONS) && !defined(USE_PTHREAD_LOCKS)
#     define NUMERIC_THREAD_ID(id) (unsigned long)(word)GC_PTHREAD_PTRVAL(id)
      /* Id not guaranteed to be unique. */
#   endif

#   ifdef CAN_HANDLE_FORK
      static pthread_cond_t mark_cv;
                        /* initialized by GC_start_mark_threads_inner   */
#   else
      static pthread_cond_t mark_cv = PTHREAD_COND_INITIALIZER;
#   endif

    /* GC_start_mark_threads is the same as in pthread_support.c except */
    /* for thread stack that is assumed to be large enough.             */

    GC_INNER void GC_start_mark_threads_inner(void)
    {
      int i;
      pthread_attr_t attr;
      pthread_t new_thread;
#     ifndef NO_MARKER_SPECIAL_SIGMASK
        sigset_t set, oldset;
#     endif

      GC_ASSERT(I_HOLD_LOCK());
      ASSERT_CANCEL_DISABLED();
      if (available_markers_m1 <= 0 || GC_parallel) return;
                /* Skip if parallel markers disabled or already started. */
      GC_wait_for_gc_completion(TRUE);

#     ifdef CAN_HANDLE_FORK
        /* Reset mark_cv state after forking (as in pthread_support.c). */
        {
          pthread_cond_t mark_cv_local = PTHREAD_COND_INITIALIZER;
          BCOPY(&mark_cv_local, &mark_cv, sizeof(mark_cv));
        }
#     endif

      GC_ASSERT(GC_fl_builder_count == 0);
      if (0 != pthread_attr_init(&attr)) ABORT("pthread_attr_init failed");
      if (0 != pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
        ABORT("pthread_attr_setdetachstate failed");

#     ifndef NO_MARKER_SPECIAL_SIGMASK
        /* Apply special signal mask to GC marker threads, and don't drop */
        /* user defined signals by GC marker threads.                     */
        if (sigfillset(&set) != 0)
          ABORT("sigfillset failed");
        if (EXPECT(pthread_sigmask(SIG_BLOCK, &set, &oldset) < 0, FALSE)) {
          WARN("pthread_sigmask set failed, no markers started\n", 0);
          GC_markers_m1 = 0;
          (void)pthread_attr_destroy(&attr);
          return;
        }
#     endif /* !NO_MARKER_SPECIAL_SIGMASK */

      /* To have proper GC_parallel value in GC_help_marker.  */
      GC_markers_m1 = available_markers_m1;

      for (i = 0; i < available_markers_m1; ++i) {
        marker_last_stack_min[i] = ADDR_LIMIT;
        if (EXPECT(0 != pthread_create(&new_thread, &attr, GC_mark_thread,
                                       (void *)(word)i), FALSE)) {
          WARN("Marker thread %" WARN_PRIdPTR " creation failed\n",
               (signed_word)i);
          /* Don't try to create other marker threads.    */
          GC_markers_m1 = i;
          break;
        }
      }

#     ifndef NO_MARKER_SPECIAL_SIGMASK
        /* Restore previous signal mask.        */
        if (EXPECT(pthread_sigmask(SIG_SETMASK, &oldset, NULL) < 0, FALSE)) {
          WARN("pthread_sigmask restore failed\n", 0);
        }
#     endif

      (void)pthread_attr_destroy(&attr);
      GC_wait_for_markers_init();
      GC_COND_LOG_PRINTF("Started %d mark helper threads\n", GC_markers_m1);
    }

#   ifdef GC_ASSERTIONS
      STATIC unsigned long GC_mark_lock_holder = NO_THREAD;
#     define SET_MARK_LOCK_HOLDER \
                (void)(GC_mark_lock_holder = NUMERIC_THREAD_ID(pthread_self()))
#     define UNSET_MARK_LOCK_HOLDER \
                do { \
                  GC_ASSERT(GC_mark_lock_holder \
                                == NUMERIC_THREAD_ID(pthread_self())); \
                  GC_mark_lock_holder = NO_THREAD; \
                } while (0)
#   endif /* GC_ASSERTIONS */

    static pthread_mutex_t mark_mutex = PTHREAD_MUTEX_INITIALIZER;

    static pthread_cond_t builder_cv = PTHREAD_COND_INITIALIZER;

    /* GC_acquire/release_mark_lock(), GC_wait_builder/marker(),          */
    /* GC_wait_for_reclaim(), GC_notify_all_builder/marker() are the same */
    /* as in pthread_support.c except that GC_generic_lock() is not used. */

#   ifdef LOCK_STATS
      volatile AO_t GC_block_count = 0;
#   endif

    GC_INNER void GC_acquire_mark_lock(void)
    {
#     ifdef NUMERIC_THREAD_ID_UNIQUE
        GC_ASSERT(GC_mark_lock_holder != NUMERIC_THREAD_ID(pthread_self()));
#     endif
      if (pthread_mutex_lock(&mark_mutex) != 0) {
        ABORT("pthread_mutex_lock failed");
      }
#     ifdef LOCK_STATS
        (void)AO_fetch_and_add1(&GC_block_count);
#     endif
      /* GC_generic_lock(&mark_mutex); */
      SET_MARK_LOCK_HOLDER;
    }

    GC_INNER void GC_release_mark_lock(void)
    {
      UNSET_MARK_LOCK_HOLDER;
      if (pthread_mutex_unlock(&mark_mutex) != 0) {
        ABORT("pthread_mutex_unlock failed");
      }
    }

    /* Collector must wait for a freelist builders for 2 reasons:       */
    /* 1) Mark bits may still be getting examined without lock.         */
    /* 2) Partial free lists referenced only by locals may not be       */
    /* scanned correctly, e.g. if they contain "pointer-free" objects,  */
    /* since the free-list link may be ignored.                         */
    STATIC void GC_wait_builder(void)
    {
      UNSET_MARK_LOCK_HOLDER;
      if (pthread_cond_wait(&builder_cv, &mark_mutex) != 0) {
        ABORT("pthread_cond_wait failed");
      }
      GC_ASSERT(GC_mark_lock_holder == NO_THREAD);
      SET_MARK_LOCK_HOLDER;
    }

    GC_INNER void GC_wait_for_reclaim(void)
    {
      GC_acquire_mark_lock();
      while (GC_fl_builder_count > 0) {
        GC_wait_builder();
      }
      GC_release_mark_lock();
    }

    GC_INNER void GC_notify_all_builder(void)
    {
      GC_ASSERT(GC_mark_lock_holder == NUMERIC_THREAD_ID(pthread_self()));
      if (pthread_cond_broadcast(&builder_cv) != 0) {
        ABORT("pthread_cond_broadcast failed");
      }
    }

    GC_INNER void GC_wait_marker(void)
    {
      GC_ASSERT(GC_parallel);
      UNSET_MARK_LOCK_HOLDER;
      if (pthread_cond_wait(&mark_cv, &mark_mutex) != 0) {
        ABORT("pthread_cond_wait failed");
      }
      GC_ASSERT(GC_mark_lock_holder == NO_THREAD);
      SET_MARK_LOCK_HOLDER;
    }

    GC_INNER void GC_notify_all_marker(void)
    {
      GC_ASSERT(GC_parallel);
      if (pthread_cond_broadcast(&mark_cv) != 0) {
        ABORT("pthread_cond_broadcast failed");
      }
    }

# else /* ! GC_PTHREADS_PARAMARK */

#   ifndef MARK_THREAD_STACK_SIZE
#     define MARK_THREAD_STACK_SIZE 0   /* default value */
#   endif

    /* mark_mutex_event, builder_cv, mark_cv are initialized in GC_thr_init */
    static HANDLE mark_mutex_event = (HANDLE)0; /* Event with auto-reset.   */
    static HANDLE builder_cv = (HANDLE)0; /* Event with manual reset.       */
    static HANDLE mark_cv = (HANDLE)0; /* Event with manual reset.          */

    GC_INNER void GC_start_mark_threads_inner(void)
    {
      int i;

      GC_ASSERT(I_HOLD_LOCK());
      ASSERT_CANCEL_DISABLED();
      if (available_markers_m1 <= 0 || GC_parallel) return;
      GC_wait_for_gc_completion(TRUE);

      GC_ASSERT(GC_fl_builder_count == 0);
      /* Initialize GC_marker_cv[] fully before starting the    */
      /* first helper thread.                                   */
      GC_markers_m1 = available_markers_m1;
      for (i = 0; i < GC_markers_m1; ++i) {
        if ((GC_marker_cv[i] = CreateEvent(NULL /* attrs */,
                                        TRUE /* isManualReset */,
                                        FALSE /* initialState */,
                                        NULL /* name (A/W) */)) == (HANDLE)0)
          ABORT("CreateEvent failed");
      }

      for (i = 0; i < GC_markers_m1; ++i) {
#       if defined(MSWINCE) || defined(MSWIN_XBOX1)
          HANDLE handle;
          DWORD thread_id;

          marker_last_stack_min[i] = ADDR_LIMIT;
          /* There is no _beginthreadex() in WinCE. */
          handle = CreateThread(NULL /* lpsa */,
                                MARK_THREAD_STACK_SIZE /* ignored */,
                                GC_mark_thread, (LPVOID)(word)i,
                                0 /* fdwCreate */, &thread_id);
          if (EXPECT(NULL == handle, FALSE)) {
            WARN("Marker thread %" WARN_PRIdPTR " creation failed\n",
                 (signed_word)i);
            /* The most probable failure reason is "not enough memory". */
            /* Don't try to create other marker threads.                */
            break;
          }
          /* It is safe to detach the thread.   */
          CloseHandle(handle);
#       else
          GC_uintptr_t handle;
          unsigned thread_id;

          marker_last_stack_min[i] = ADDR_LIMIT;
          handle = _beginthreadex(NULL /* security_attr */,
                                MARK_THREAD_STACK_SIZE, GC_mark_thread,
                                (void *)(word)i, 0 /* flags */, &thread_id);
          if (EXPECT(!handle || handle == (GC_uintptr_t)-1L, FALSE)) {
            WARN("Marker thread %" WARN_PRIdPTR " creation failed\n",
                 (signed_word)i);
            /* Don't try to create other marker threads.                */
            break;
          } else {/* We may detach the thread (if handle is of HANDLE type) */
            /* CloseHandle((HANDLE)handle); */
          }
#       endif
      }

      /* Adjust GC_markers_m1 (and free unused resources) if failed.    */
      while (GC_markers_m1 > i) {
        GC_markers_m1--;
        CloseHandle(GC_marker_cv[GC_markers_m1]);
      }
      GC_wait_for_markers_init();
      GC_COND_LOG_PRINTF("Started %d mark helper threads\n", GC_markers_m1);
      if (EXPECT(0 == i, FALSE)) {
        CloseHandle(mark_cv);
        CloseHandle(builder_cv);
        CloseHandle(mark_mutex_event);
      }
    }

#   ifdef GC_ASSERTIONS
      STATIC unsigned long GC_mark_lock_holder = NO_THREAD;
#     define SET_MARK_LOCK_HOLDER \
                (void)(GC_mark_lock_holder = GetCurrentThreadId())
#     define UNSET_MARK_LOCK_HOLDER \
                do { \
                  GC_ASSERT(GC_mark_lock_holder == GetCurrentThreadId()); \
                  GC_mark_lock_holder = NO_THREAD; \
                } while (0)
#   endif /* GC_ASSERTIONS */

    STATIC /* volatile */ LONG GC_mark_mutex_state = 0;
                                /* Mutex state: 0 - unlocked,           */
                                /* 1 - locked and no other waiters,     */
                                /* -1 - locked and waiters may exist.   */
                                /* Accessed by InterlockedExchange().   */

    /* #define LOCK_STATS */
#   ifdef LOCK_STATS
      volatile AO_t GC_block_count = 0;
      volatile AO_t GC_unlocked_count = 0;
#   endif

    GC_INNER void GC_acquire_mark_lock(void)
    {
      GC_ASSERT(GC_mark_lock_holder != GetCurrentThreadId());
      if (EXPECT(InterlockedExchange(&GC_mark_mutex_state,
                                     1 /* locked */) != 0, FALSE)) {
#       ifdef LOCK_STATS
          (void)AO_fetch_and_add1(&GC_block_count);
#       endif
        /* Repeatedly reset the state and wait until acquire the lock.  */
        while (InterlockedExchange(&GC_mark_mutex_state,
                                   -1 /* locked_and_has_waiters */) != 0) {
          if (WaitForSingleObject(mark_mutex_event, INFINITE) == WAIT_FAILED)
            ABORT("WaitForSingleObject failed");
        }
      }
#     ifdef LOCK_STATS
        else {
          (void)AO_fetch_and_add1(&GC_unlocked_count);
        }
#     endif

      GC_ASSERT(GC_mark_lock_holder == NO_THREAD);
      SET_MARK_LOCK_HOLDER;
    }

    GC_INNER void GC_release_mark_lock(void)
    {
      UNSET_MARK_LOCK_HOLDER;
      if (EXPECT(InterlockedExchange(&GC_mark_mutex_state,
                                     0 /* unlocked */) < 0, FALSE)) {
        /* wake a waiter */
        if (SetEvent(mark_mutex_event) == FALSE)
          ABORT("SetEvent failed");
      }
    }

    /* In GC_wait_for_reclaim/GC_notify_all_builder() we emulate POSIX    */
    /* cond_wait/cond_broadcast() primitives with WinAPI Event object     */
    /* (working in "manual reset" mode).  This works here because         */
    /* GC_notify_all_builder() is always called holding lock on           */
    /* mark_mutex and the checked condition (GC_fl_builder_count == 0)    */
    /* is the only one for which broadcasting on builder_cv is performed. */

    GC_INNER void GC_wait_for_reclaim(void)
    {
      GC_ASSERT(builder_cv != 0);
      for (;;) {
        GC_acquire_mark_lock();
        if (GC_fl_builder_count == 0)
          break;
        if (ResetEvent(builder_cv) == FALSE)
          ABORT("ResetEvent failed");
        GC_release_mark_lock();
        if (WaitForSingleObject(builder_cv, INFINITE) == WAIT_FAILED)
          ABORT("WaitForSingleObject failed");
      }
      GC_release_mark_lock();
    }

    GC_INNER void GC_notify_all_builder(void)
    {
      GC_ASSERT(GC_mark_lock_holder == GetCurrentThreadId());
      GC_ASSERT(builder_cv != 0);
      GC_ASSERT(GC_fl_builder_count == 0);
      if (SetEvent(builder_cv) == FALSE)
        ABORT("SetEvent failed");
    }

    /* mark_cv is used (for waiting) by a non-helper thread.    */

    GC_INNER void GC_wait_marker(void)
    {
      HANDLE event = mark_cv;
      thread_id_t self_id = GetCurrentThreadId();
      int i = GC_markers_m1;

      while (i-- > 0) {
        if (GC_marker_Id[i] == self_id) {
          event = GC_marker_cv[i];
          break;
        }
      }

      if (ResetEvent(event) == FALSE)
        ABORT("ResetEvent failed");
      GC_release_mark_lock();
      if (WaitForSingleObject(event, INFINITE) == WAIT_FAILED)
        ABORT("WaitForSingleObject failed");
      GC_acquire_mark_lock();
    }

    GC_INNER void GC_notify_all_marker(void)
    {
      thread_id_t self_id = GetCurrentThreadId();
      int i = GC_markers_m1;

      while (i-- > 0) {
        /* Notify every marker ignoring self (for efficiency).  */
        if (SetEvent(GC_marker_Id[i] != self_id ? GC_marker_cv[i] :
                     mark_cv) == FALSE)
          ABORT("SetEvent failed");
      }
    }

# endif /* ! GC_PTHREADS_PARAMARK */

  static unsigned required_markers_cnt = 0;
                        /* The default value (0) means the number of    */
                        /* markers should be selected automatically.    */

  GC_API void GC_CALL GC_set_markers_count(unsigned markers)
  {
    /* The same implementation as in pthread_support.c. */
    required_markers_cnt = markers < MAX_MARKERS ? markers : MAX_MARKERS;
  }

# define START_MARK_THREADS() \
        if (EXPECT(GC_parallel || available_markers_m1 <= 0, TRUE)) {} \
        else GC_start_mark_threads()
#else

# define START_MARK_THREADS() (void)0
#endif /* !PARALLEL_MARK */

  /* We have no DllMain to take care of new threads.  Thus, we  */
  /* must properly intercept thread creation.                   */

  struct win32_start_info {
    LPTHREAD_START_ROUTINE start_routine;
    LPVOID arg;
  };

  STATIC void * GC_CALLBACK GC_win32_start_inner(struct GC_stack_base *sb,
                                                 void *arg)
  {
    void * ret;
    LPTHREAD_START_ROUTINE start_routine =
                        ((struct win32_start_info *)arg) -> start_routine;
    LPVOID start_arg = ((struct win32_start_info *)arg) -> arg;

    GC_register_my_thread(sb); /* This waits for an in-progress GC.     */
#   ifdef DEBUG_THREADS
      GC_log_printf("thread 0x%lx starting...\n", (long)GetCurrentThreadId());
#   endif
    GC_free(arg);

    /* Clear the thread entry even if we exit with an exception.        */
    /* This is probably pointless, since an uncaught exception is       */
    /* supposed to result in the process being killed.                  */
#   ifndef NO_SEH_AVAILABLE
      ret = NULL; /* to avoid "might be uninitialized" compiler warning */
      __try
#   endif
    {
      ret = (void *)(word)(*start_routine)(start_arg);
    }
#   ifndef NO_SEH_AVAILABLE
      __finally
#   endif
    {
      GC_unregister_my_thread();
    }

#   ifdef DEBUG_THREADS
      GC_log_printf("thread 0x%lx returned from start routine\n",
                    (long)GetCurrentThreadId());
#   endif
    return ret;
  }

  STATIC DWORD WINAPI GC_win32_start(LPVOID arg)
  {
    return (DWORD)(word)GC_call_with_stack_base(GC_win32_start_inner, arg);
  }

  GC_API HANDLE WINAPI GC_CreateThread(
                        LPSECURITY_ATTRIBUTES lpThreadAttributes,
                        GC_WIN32_SIZE_T dwStackSize,
                        LPTHREAD_START_ROUTINE lpStartAddress,
                        LPVOID lpParameter, DWORD dwCreationFlags,
                        LPDWORD lpThreadId)
  {
    if (!EXPECT(GC_is_initialized, TRUE)) GC_init();
    GC_ASSERT(GC_thr_initialized);
        /* Make sure GC is initialized (i.e. main thread is attached,   */
        /* tls is initialized).  This is redundant when                 */
        /* GC_win32_dll_threads is set by GC_use_threads_discovery().   */

#   ifdef DEBUG_THREADS
      GC_log_printf("About to create a thread from 0x%lx\n",
                    (long)GetCurrentThreadId());
#   endif
    if (GC_win32_dll_threads) {
      return CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress,
                          lpParameter, dwCreationFlags, lpThreadId);
    } else {
      struct win32_start_info *psi =
                (struct win32_start_info *)GC_malloc_uncollectable(
                                        sizeof(struct win32_start_info));
                /* Handed off to and deallocated by child thread.       */
      HANDLE thread_h;

      if (EXPECT(NULL == psi, FALSE)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
      }

      /* set up thread arguments */
      psi -> start_routine = lpStartAddress;
      psi -> arg = lpParameter;
      GC_dirty(psi);
      REACHABLE_AFTER_DIRTY(lpParameter);

      START_MARK_THREADS();
      set_need_to_lock();
      thread_h = CreateThread(lpThreadAttributes, dwStackSize, GC_win32_start,
                              psi, dwCreationFlags, lpThreadId);
      if (EXPECT(0 == thread_h, FALSE)) GC_free(psi);
      return thread_h;
    }
  }

  GC_API DECLSPEC_NORETURN void WINAPI GC_ExitThread(DWORD dwExitCode)
  {
    GC_unregister_my_thread();
    ExitThread(dwExitCode);
  }

# if !defined(CYGWIN32) && !defined(MSWINCE) && !defined(MSWIN_XBOX1) \
     && !defined(NO_CRT)
    GC_API GC_uintptr_t GC_CALL GC_beginthreadex(
                                  void *security, unsigned stack_size,
                                  unsigned (__stdcall *start_address)(void *),
                                  void *arglist, unsigned initflag,
                                  unsigned *thrdaddr)
    {
      if (!EXPECT(GC_is_initialized, TRUE)) GC_init();
      GC_ASSERT(GC_thr_initialized);
#     ifdef DEBUG_THREADS
        GC_log_printf("About to create a thread from 0x%lx\n",
                      (long)GetCurrentThreadId());
#     endif

      if (GC_win32_dll_threads) {
        return _beginthreadex(security, stack_size, start_address,
                              arglist, initflag, thrdaddr);
      } else {
        GC_uintptr_t thread_h;
        struct win32_start_info *psi =
                (struct win32_start_info *)GC_malloc_uncollectable(
                                        sizeof(struct win32_start_info));
                /* Handed off to and deallocated by child thread.       */

        if (EXPECT(NULL == psi, FALSE)) {
          /* MSDN docs say _beginthreadex() returns 0 on error and sets */
          /* errno to either EAGAIN (too many threads) or EINVAL (the   */
          /* argument is invalid or the stack size is incorrect), so we */
          /* set errno to EAGAIN on "not enough memory".                */
          errno = EAGAIN;
          return 0;
        }

        /* set up thread arguments */
        psi -> start_routine = (LPTHREAD_START_ROUTINE)start_address;
        psi -> arg = arglist;
        GC_dirty(psi);
        REACHABLE_AFTER_DIRTY(arglist);

        START_MARK_THREADS();
        set_need_to_lock();
        thread_h = _beginthreadex(security, stack_size,
                        (unsigned (__stdcall *)(void *))GC_win32_start,
                        psi, initflag, thrdaddr);
        if (EXPECT(0 == thread_h, FALSE)) GC_free(psi);
        return thread_h;
      }
    }

    GC_API void GC_CALL GC_endthreadex(unsigned retval)
    {
      GC_unregister_my_thread();
      _endthreadex(retval);
    }
# endif /* !CYGWIN32 && !MSWINCE && !MSWIN_XBOX1 && !NO_CRT */

#ifdef GC_WINMAIN_REDIRECT
  /* This might be useful on WinCE.  Shouldn't be used with GC_DLL.     */

# if defined(MSWINCE) && defined(UNDER_CE)
#   define WINMAIN_LPTSTR LPWSTR
# else
#   define WINMAIN_LPTSTR LPSTR
# endif

  /* This is defined in gc.h.   */
# undef WinMain

  /* Defined outside GC by an application.      */
  int WINAPI GC_WinMain(HINSTANCE, HINSTANCE, WINMAIN_LPTSTR, int);

  typedef struct {
    HINSTANCE hInstance;
    HINSTANCE hPrevInstance;
    WINMAIN_LPTSTR lpCmdLine;
    int nShowCmd;
  } main_thread_args;

  static DWORD WINAPI main_thread_start(LPVOID arg)
  {
    main_thread_args *main_args = (main_thread_args *)arg;
    return (DWORD)GC_WinMain(main_args -> hInstance,
                             main_args -> hPrevInstance,
                             main_args -> lpCmdLine, main_args -> nShowCmd);
  }

  STATIC void *GC_CALLBACK GC_waitForSingleObjectInfinite(void *handle)
  {
    return (void *)(word)WaitForSingleObject((HANDLE)handle, INFINITE);
  }

# ifndef WINMAIN_THREAD_STACK_SIZE
#   define WINMAIN_THREAD_STACK_SIZE 0  /* default value */
# endif

  int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                     WINMAIN_LPTSTR lpCmdLine, int nShowCmd)
  {
    DWORD exit_code = 1;

    main_thread_args args = {
                hInstance, hPrevInstance, lpCmdLine, nShowCmd
    };
    HANDLE thread_h;
    DWORD thread_id;

    /* initialize everything */
    GC_INIT();

    /* start the main thread */
    thread_h = GC_CreateThread(NULL /* lpsa */,
                        WINMAIN_THREAD_STACK_SIZE /* ignored on WinCE */,
                        main_thread_start, &args, 0 /* fdwCreate */,
                        &thread_id);
    if (NULL == thread_h)
      ABORT("GC_CreateThread(main_thread) failed");

    if ((DWORD)(word)GC_do_blocking(GC_waitForSingleObjectInfinite,
                                    (void *)thread_h) == WAIT_FAILED)
      ABORT("WaitForSingleObject(main_thread) failed");
    GetExitCodeThread(thread_h, &exit_code);
    CloseHandle(thread_h);

#   ifdef MSWINCE
      GC_deinit();
#   endif
    return (int)exit_code;
  }

#endif /* GC_WINMAIN_REDIRECT */

GC_INNER void GC_thr_init(void)
{
  struct GC_stack_base sb;
# if (!defined(HAVE_PTHREAD_SETNAME_NP_WITH_TID) && !defined(MSWINCE) \
      && defined(PARALLEL_MARK)) || defined(WOW64_THREAD_CONTEXT_WORKAROUND)
    HMODULE hK32;
#   ifdef MSWINRT_FLAVOR
      MEMORY_BASIC_INFORMATION memInfo;

      if (VirtualQuery((void*)(word)GetProcAddress, &memInfo, sizeof(memInfo))
          != sizeof(memInfo))
        ABORT("Weird VirtualQuery result");
      hK32 = (HMODULE)memInfo.AllocationBase;
#   else
      hK32 = GetModuleHandle(TEXT("kernel32.dll"));
#   endif
# endif

  GC_ASSERT(I_HOLD_LOCK());
  GC_ASSERT(!GC_thr_initialized);
  GC_ASSERT((word)(&GC_threads) % sizeof(word) == 0);
# ifdef GC_ASSERTIONS
    GC_thr_initialized = TRUE;
# endif
# ifdef GC_NO_THREADS_DISCOVERY
#   define main_thread_id GetCurrentThreadId()
# else
    main_thread_id = GetCurrentThreadId();
# endif
# ifdef CAN_HANDLE_FORK
    GC_setup_atfork();
# endif

# ifdef WOW64_THREAD_CONTEXT_WORKAROUND
    /* Set isWow64 flag. */
      if (hK32) {
        FARPROC pfn = GetProcAddress(hK32, "IsWow64Process");
        if (pfn
            && !(*(BOOL (WINAPI*)(HANDLE, BOOL*))(word)pfn)(
                                        GetCurrentProcess(), &isWow64))
          isWow64 = FALSE; /* IsWow64Process failed */
      }
# endif

  /* Add the initial thread, so we can stop it. */
  sb.mem_base = GC_stackbottom;
  GC_ASSERT(sb.mem_base != NULL);
# ifdef IA64
    sb.reg_base = GC_register_stackbottom;
# endif

# if defined(PARALLEL_MARK)
    {
      char * markers_string = GETENV("GC_MARKERS");
      int markers = required_markers_cnt;

      if (markers_string != NULL) {
        markers = atoi(markers_string);
        if (markers <= 0 || markers > MAX_MARKERS) {
          WARN("Too big or invalid number of mark threads: %" WARN_PRIdPTR
               "; using maximum threads\n", (signed_word)markers);
          markers = MAX_MARKERS;
        }
      } else if (0 == markers) {
        /* Unless the client sets the desired number of         */
        /* parallel markers, it is determined based on the      */
        /* number of CPU cores.                                 */
#       ifdef MSWINCE
          /* There is no GetProcessAffinityMask() in WinCE.     */
          /* GC_sysinfo is already initialized.                 */
          markers = (int)GC_sysinfo.dwNumberOfProcessors;
#       else
#         ifdef _WIN64
            DWORD_PTR procMask = 0;
            DWORD_PTR sysMask;
#         else
            DWORD procMask = 0;
            DWORD sysMask;
#         endif
          int ncpu = 0;
          if (
#           ifdef __cplusplus
              GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)
#           else
              /* Cast args to void* for compatibility with some old SDKs. */
              GetProcessAffinityMask(GetCurrentProcess(),
                                     (void *)&procMask, (void *)&sysMask)
#           endif
              && procMask) {
            do {
              ncpu++;
            } while ((procMask &= procMask - 1) != 0);
          }
          markers = ncpu;
#       endif
#       if defined(GC_MIN_MARKERS) && !defined(CPPCHECK)
          /* This is primarily for testing on systems without getenv(). */
          if (markers < GC_MIN_MARKERS)
            markers = GC_MIN_MARKERS;
#       endif
        if (markers > MAX_MARKERS)
          markers = MAX_MARKERS; /* silently limit the value */
      }
      available_markers_m1 = markers - 1;
    }

    /* Check whether parallel mode could be enabled.    */
      if (GC_win32_dll_threads || available_markers_m1 <= 0) {
        /* Disable parallel marking. */
        GC_parallel = FALSE;
        GC_COND_LOG_PRINTF(
                "Single marker thread, turning off parallel marking\n");
      } else {
#       ifndef GC_PTHREADS_PARAMARK
          /* Initialize Win32 event objects for parallel marking.       */
          mark_mutex_event = CreateEvent(NULL /* attrs */,
                                FALSE /* isManualReset */,
                                FALSE /* initialState */, NULL /* name */);
          builder_cv = CreateEvent(NULL /* attrs */,
                                TRUE /* isManualReset */,
                                FALSE /* initialState */, NULL /* name */);
          mark_cv = CreateEvent(NULL /* attrs */, TRUE /* isManualReset */,
                                FALSE /* initialState */, NULL /* name */);
          if (mark_mutex_event == (HANDLE)0 || builder_cv == (HANDLE)0
              || mark_cv == (HANDLE)0)
            ABORT("CreateEvent failed");
#       endif
#       if !defined(HAVE_PTHREAD_SETNAME_NP_WITH_TID) && !defined(MSWINCE)
          if (hK32)
            setThreadDescription_fn = GetProcAddress(hK32,
                                                     "SetThreadDescription");
#       endif
      }
# endif /* PARALLEL_MARK */

  GC_ASSERT(NULL == GC_lookup_thread(main_thread_id));
  GC_register_my_thread_inner(&sb, main_thread_id);
# undef main_thread_id
}

#ifdef GC_PTHREADS

  struct start_info {
    void *(*start_routine)(void *);
    void *arg;
    int detached;
  };

  GC_API int GC_pthread_join(pthread_t thread, void **retval)
  {
    int result;
#   ifndef GC_WIN32_PTHREADS
      GC_thread t;
#   endif
    DCL_LOCK_STATE;

    GC_ASSERT(!GC_win32_dll_threads);
#   ifdef DEBUG_THREADS
      GC_log_printf("thread %p(0x%lx) is joining thread %p\n",
                    (void *)GC_PTHREAD_PTRVAL(pthread_self()),
                    (long)GetCurrentThreadId(),
                    (void *)GC_PTHREAD_PTRVAL(thread));
#   endif

    /* Thread being joined might not have registered itself yet. */
    /* After the join, thread id may have been recycled.         */
    /* FIXME: It would be better if this worked more like        */
    /* pthread_support.c.                                        */
#   ifndef GC_WIN32_PTHREADS
      while ((t = GC_lookup_by_pthread(thread)) == 0)
        Sleep(10);
#   endif
    result = pthread_join(thread, retval);
    if (EXPECT(0 == result, TRUE)) {
#     ifdef GC_WIN32_PTHREADS
        /* pthreads-win32 and winpthreads id are unique (not recycled). */
        GC_thread t = GC_lookup_by_pthread(thread);
        if (NULL == t) ABORT("Thread not registered");
#     endif

      LOCK();
      if (KNOWN_FINISHED(t)) {
        GC_delete_gc_thread_no_free(t);
        GC_INTERNAL_FREE(t);
      }
      UNLOCK();
    }

#   ifdef DEBUG_THREADS
      GC_log_printf("thread %p(0x%lx) join with thread %p %s\n",
                    (void *)GC_PTHREAD_PTRVAL(pthread_self()),
                    (long)GetCurrentThreadId(),
                    (void *)GC_PTHREAD_PTRVAL(thread),
                    result != 0 ? "failed" : "succeeded");
#   endif
    return result;
  }

  /* Cygwin-pthreads calls CreateThread internally, but it's not easily */
  /* interceptable by us..., so intercept pthread_create instead.       */
  GC_API int GC_pthread_create(pthread_t *new_thread,
                               GC_PTHREAD_CREATE_CONST pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg)
  {
    int result;
    struct start_info * si;

    if (!EXPECT(GC_is_initialized, TRUE)) GC_init();
    GC_ASSERT(GC_thr_initialized);
    GC_ASSERT(!GC_win32_dll_threads);

      /* This is otherwise saved only in an area mmapped by the thread  */
      /* library, which isn't visible to the collector.                 */
      si = (struct start_info *)GC_malloc_uncollectable(
                                                sizeof(struct start_info));
      if (EXPECT(NULL == si, FALSE)) return EAGAIN;

      si -> start_routine = start_routine;
      si -> arg = arg;
      GC_dirty(si);
      REACHABLE_AFTER_DIRTY(arg);
      if (attr != NULL
          && pthread_attr_getdetachstate(attr, &(si -> detached)) != 0)
        ABORT("pthread_attr_getdetachstate failed");
#     ifdef DEBUG_THREADS
        GC_log_printf("About to create a thread from %p(0x%lx)\n",
                      (void *)GC_PTHREAD_PTRVAL(pthread_self()),
                      (long)GetCurrentThreadId());
#     endif
      START_MARK_THREADS();
      set_need_to_lock();
      result = pthread_create(new_thread, attr, GC_pthread_start, si);
      if (EXPECT(result != 0, FALSE)) GC_free(si); /* failure */
      return result;
  }

  STATIC void * GC_CALLBACK GC_pthread_start_inner(struct GC_stack_base *sb,
                                                   void * arg)
  {
    struct start_info * si = (struct start_info *)arg;
    void * result;
    void *(*start)(void *);
    void *start_arg;
    thread_id_t self_id = GetCurrentThreadId();
    pthread_t self = pthread_self();
    GC_thread me;
    DCL_LOCK_STATE;

#   ifdef DEBUG_THREADS
      GC_log_printf("thread %p(0x%x) starting...\n",
                    (void *)GC_PTHREAD_PTRVAL(self), (int)self_id);
#   endif

    GC_ASSERT(!GC_win32_dll_threads);
    /* If a GC occurs before the thread is registered, that GC will     */
    /* ignore this thread.  That's fine, since it will block trying to  */
    /* acquire the allocation lock, and won't yet hold interesting      */
    /* pointers.                                                        */
    LOCK();
    /* We register the thread here instead of in the parent, so that    */
    /* we don't need to hold the allocation lock during pthread_create. */
    me = GC_register_my_thread_inner(sb, self_id);
    SET_PTHREAD_MAP_CACHE(self, self_id);
    GC_ASSERT(me != &first_thread);
    me -> pthread_id = self;
    if (si->detached) me -> flags |= DETACHED;
#   ifdef THREAD_LOCAL_ALLOC
      GC_init_thread_local(&me->tlfs);
#   endif
    UNLOCK();

    start = si -> start_routine;
    start_arg = si -> arg;

    GC_free(si); /* was allocated uncollectible */

    pthread_cleanup_push(GC_thread_exit_proc, (void *)me);
    result = (*start)(start_arg);
    me -> status = result;
    GC_dirty(me);
    pthread_cleanup_pop(1);

#   ifdef DEBUG_THREADS
      GC_log_printf("thread %p(0x%x) returned from start routine\n",
                    (void *)GC_PTHREAD_PTRVAL(self), (int)self_id);
#   endif
    return result;
  }

  STATIC void * GC_pthread_start(void * arg)
  {
    return GC_call_with_stack_base(GC_pthread_start_inner, arg);
  }

  GC_INNER_PTHRSTART void GC_thread_exit_proc(void *arg)
  {
    GC_thread me = (GC_thread)arg;
    DCL_LOCK_STATE;

    GC_ASSERT(!GC_win32_dll_threads);
#   ifdef DEBUG_THREADS
      GC_log_printf("thread %p(0x%lx) called pthread_exit()\n",
                    (void *)GC_PTHREAD_PTRVAL(pthread_self()),
                    (long)GetCurrentThreadId());
#   endif

    LOCK();
    GC_wait_for_gc_completion(FALSE);
#   if defined(THREAD_LOCAL_ALLOC)
      GC_ASSERT(GC_getspecific(GC_thread_key) == &me->tlfs);
      GC_destroy_thread_local(&me->tlfs);
#   endif
    if (me -> flags & DETACHED) {
      GC_delete_thread(GetCurrentThreadId());
    } else {
      /* deallocate it as part of join */
      me -> flags |= FINISHED;
    }
#   if defined(THREAD_LOCAL_ALLOC)
      /* It is required to call remove_specific defined in specific.c. */
      GC_remove_specific(GC_thread_key);
#   endif
    UNLOCK();
  }

# ifndef GC_NO_PTHREAD_SIGMASK
    /* pthreads-win32 does not support sigmask. */
    /* So, nothing required here...             */
    GC_API int GC_pthread_sigmask(int how, const sigset_t *set,
                                  sigset_t *oset)
    {
      return pthread_sigmask(how, set, oset);
    }
# endif /* !GC_NO_PTHREAD_SIGMASK */

  GC_API int GC_pthread_detach(pthread_t thread)
  {
    int result;
    GC_thread t;
    DCL_LOCK_STATE;

    GC_ASSERT(!GC_win32_dll_threads);
    /* The thread might not have registered itself yet. */
    /* TODO: Wait for registration of the created thread in pthread_create. */
    while ((t = GC_lookup_by_pthread(thread)) == NULL)
      Sleep(10);
    result = pthread_detach(thread);
    if (EXPECT(0 == result, TRUE)) {
      LOCK();
      t -> flags |= DETACHED;
      /* Here the pthread id may have been recycled.    */
      if (KNOWN_FINISHED(t)) {
        GC_delete_gc_thread_no_free(t);
        GC_INTERNAL_FREE(t);
      }
      UNLOCK();
    }
    return result;
  }

#elif !defined(GC_NO_THREADS_DISCOVERY)
    /* We avoid acquiring locks here, since this doesn't seem to be     */
    /* preemptible.  This may run with an uninitialized collector, in   */
    /* which case we don't do much.  This implies that no threads other */
    /* than the main one should be created with an uninitialized        */
    /* collector.  (The alternative of initializing the collector here  */
    /* seems dangerous, since DllMain is limited in what it can do.)    */

# ifdef GC_INSIDE_DLL
    /* Export only if needed by client. */
    GC_API
# else
#   define GC_DllMain DllMain
# endif
  BOOL WINAPI GC_DllMain(HINSTANCE inst, ULONG reason, LPVOID reserved)
  {
      thread_id_t self_id;

      UNUSED_ARG(inst);
      UNUSED_ARG(reserved);
      /* Note that GC_use_threads_discovery should be called by the     */
      /* client application at start-up to activate automatic thread    */
      /* registration (it is the default GC behavior);                  */
      /* to always have automatic thread registration turned on, the GC */
      /* should be compiled with -D GC_DISCOVER_TASK_THREADS.           */
      if (!GC_win32_dll_threads && GC_is_initialized) return TRUE;

      switch (reason) {
       case DLL_THREAD_ATTACH: /* invoked for threads other than main */
#       ifdef PARALLEL_MARK
          /* Don't register marker threads. */
          if (GC_parallel) {
            /* We could reach here only if GC is not initialized.       */
            /* Because GC_thr_init() sets GC_parallel to off.           */
            break;
          }
#       endif
        /* FALLTHRU */
       case DLL_PROCESS_ATTACH:
        /* This may run with the collector uninitialized. */
        self_id = GetCurrentThreadId();
        if (GC_is_initialized && main_thread_id != self_id) {
            struct GC_stack_base sb;
            /* Don't lock here. */
#           ifdef GC_ASSERTIONS
              int sb_result =
#           endif
                        GC_get_stack_base(&sb);
            GC_ASSERT(sb_result == GC_SUCCESS);
            GC_register_my_thread_inner(&sb, self_id);
        } /* o.w. we already did it during GC_thr_init, called by GC_init */
        break;

       case DLL_THREAD_DETACH:
        /* We are hopefully running in the context of the exiting thread. */
        if (GC_win32_dll_threads) {
          GC_delete_thread(GetCurrentThreadId());
        }
        break;

       case DLL_PROCESS_DETACH:
        if (GC_win32_dll_threads) {
          int i;
          int my_max = (int)GC_get_max_thread_index();

          for (i = 0; i <= my_max; ++i) {
           if (AO_load(&(dll_thread_table[i].tm.in_use)))
             GC_delete_gc_thread_no_free(&dll_thread_table[i]);
          }
          GC_deinit();
        }
        break;
      }
      return TRUE;
  }
#endif /* !GC_NO_THREADS_DISCOVERY && !GC_PTHREADS */

/* Perform all initializations, including those that may require        */
/* allocation, e.g. initialize thread local free lists if used.         */
GC_INNER void GC_init_parallel(void)
{
# ifdef THREAD_LOCAL_ALLOC
    GC_thread me;
    DCL_LOCK_STATE;

    GC_ASSERT(GC_is_initialized);
    LOCK();
    me = GC_lookup_thread(GetCurrentThreadId());
    CHECK_LOOKUP_MY_THREAD(me);
    GC_init_thread_local(&me->tlfs);
    UNLOCK();
# endif
# if defined(CPPCHECK) && !defined(GC_NO_THREADS_DISCOVERY)
    GC_noop1((word)&GC_DllMain);
# endif
  if (GC_win32_dll_threads) {
    set_need_to_lock();
        /* Cannot intercept thread creation.  Hence we don't know if    */
        /* other threads exist.  However, client is not allowed to      */
        /* create other threads before collector initialization.        */
        /* Thus it's OK not to lock before this.                        */
  }
}

#if defined(USE_PTHREAD_LOCKS)
  /* Support for pthread locking code.          */
  /* pthread_mutex_trylock may not win here,    */
  /* due to builtin support for spinning first? */

  GC_INNER void GC_lock(void)
  {
    pthread_mutex_lock(&GC_allocate_ml);
  }
#endif /* USE_PTHREAD_LOCKS */

#if defined(THREAD_LOCAL_ALLOC)

  /* Add thread-local allocation support.  VC++ uses __declspec(thread).  */

  /* We must explicitly mark ptrfree and gcj free lists, since the free   */
  /* list links wouldn't otherwise be found.  We also set them in the     */
  /* normal free lists, since that involves touching less memory than if  */
  /* we scanned them normally.                                            */
  GC_INNER void GC_mark_thread_local_free_lists(void)
  {
    int i;
    GC_thread p;

    for (i = 0; i < THREAD_TABLE_SZ; ++i) {
      for (p = GC_threads[i]; p != NULL; p = p -> tm.next)
        if (!KNOWN_FINISHED(p)) {
#         ifdef DEBUG_THREADS
            GC_log_printf("Marking thread locals for 0x%x\n", (int)p->id);
#         endif
          GC_mark_thread_local_fls_for(&p->tlfs);
        }
    }
  }

# if defined(GC_ASSERTIONS)
    /* Check that all thread-local free-lists are completely marked.    */
    /* also check that thread-specific-data structures are marked.      */
    void GC_check_tls(void)
    {
        int i;
        GC_thread p;

        for (i = 0; i < THREAD_TABLE_SZ; ++i) {
          for (p = GC_threads[i]; p != NULL; p = p -> tm.next) {
            if (!KNOWN_FINISHED(p))
              GC_check_tls_for(&p->tlfs);
          }
        }
#       if defined(USE_CUSTOM_SPECIFIC)
          if (GC_thread_key != 0)
            GC_check_tsd_marks(GC_thread_key);
#       endif
    }
# endif /* GC_ASSERTIONS */

#endif /* THREAD_LOCAL_ALLOC ... */

# ifndef GC_NO_THREAD_REDIRECTS
    /* Restore thread calls redirection.        */
#   define CreateThread GC_CreateThread
#   define ExitThread GC_ExitThread
#   undef _beginthreadex
#   define _beginthreadex GC_beginthreadex
#   undef _endthreadex
#   define _endthreadex GC_endthreadex
# endif /* !GC_NO_THREAD_REDIRECTS */

#endif /* GC_WIN32_THREADS */
