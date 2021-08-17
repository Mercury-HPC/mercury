/*
 * Copyright (C) 2013-2020 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#ifndef MERCURY_THREAD_ANNOTATION_H
#define MERCURY_THREAD_ANNOTATION_H

/* Enable thread safety attributes only with clang.
 * The attributes can be safely erased when compiling with other compilers. */
#if defined(__clang__)
#    define HG_THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#    define HG_THREAD_ANNOTATION_ATTRIBUTE__(x) // no-op
#endif

#define HG_LOCK_CAPABILITY(x) HG_THREAD_ANNOTATION_ATTRIBUTE__(capability(x))

#define HG_LOCK_ACQUIRE(...)                                                   \
    HG_THREAD_ANNOTATION_ATTRIBUTE__(acquire_capability(__VA_ARGS__))

#define HG_LOCK_ACQUIRE_SHARED(...)                                            \
    HG_THREAD_ANNOTATION_ATTRIBUTE__(acquire_shared_capability(__VA_ARGS__))

#define HG_LOCK_RELEASE(...)                                                   \
    HG_THREAD_ANNOTATION_ATTRIBUTE__(release_capability(__VA_ARGS__))

#define HG_LOCK_RELEASE_SHARED(...)                                            \
    HG_THREAD_ANNOTATION_ATTRIBUTE__(release_shared_capability(__VA_ARGS__))

#define HG_LOCK_TRY_ACQUIRE(...)                                               \
    HG_THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_capability(__VA_ARGS__))

#define HG_LOCK_TRY_ACQUIRE_SHARED(...)                                        \
    HG_THREAD_ANNOTATION_ATTRIBUTE__(try_acquire_shared_capability(__VA_ARGS__))

#define HG_LOCK_NO_THREAD_SAFETY_ANALYSIS                                      \
    HG_THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

#endif /* MERCURY_THREAD_ANNOTATION_H */
