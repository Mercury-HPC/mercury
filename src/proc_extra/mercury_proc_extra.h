/**
 * Copyright (c) 2013-2022 UChicago Argonne, LLC and The HDF Group.
 * Copyright (c) 2022-2023 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef MERCURY_PROC_EXTRA_H
#define MERCURY_PROC_EXTRA_H

#include "mercury_proc.h"

/*************************************/
/* Public Type and Struct Definition */
/*************************************/

/* legacy string types for backward compat */
typedef const char *hg_const_string_t;
typedef char *hg_string_t;

/*****************/
/* Public Macros */
/*****************/

/*********************/
/* Public Prototypes */
/*********************/

#ifdef __cplusplus
extern "C" {
#endif

/*
 * helper functions for hg_rpcgen.
 *
 * note: memory allocated by the HG_DECODE op in the following
 * functions can be freed using the HG_FREE op.
 */

/**
 * Variable length counted bytes (xdr_bytes).  Decode will allocate
 * and return memory in *cpp (if it is NULL).
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param cpp [IN/OUT]          *cpp points to the bytes
 * \param sizep [IN/OUT]        *sizep is the count
 * \param maxsize [IN]          largest number of bytes we allow
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
HG_PUBLIC hg_return_t
hg_proc_varbytes(hg_proc_t proc, char **cpp, uint32_t *sizep, uint32_t maxsize);

/**
 * NULL terminated C strings (xdr_string).  Length can be determined
 * with strlen().  Decode will allocate and return memory in *cpp
 * (if it is NULL).
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param cpp [IN/OUT]          *cpp points to the string
 * \param maxsize [IN]          largest size of string we allow
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
HG_PUBLIC hg_return_t
hg_proc_string(hg_proc_t proc, char **cpp, uint32_t maxsize);

/**
 * A variable length array of elements (xdr_array).  Decode will
 * allocate the array memory if *addrp is NULL.
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param addrp [IN/OUT]        pointer to the variable length array
 * \param sizep [IN/OUT]        *sizep is the number of array elements
 * \param maxsize [IN]          max number of elements allowed in array
 * \param elsize [IN]           size of a single element in bytes
 * \param elproc [IN]           proc fn to handle an array element
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
HG_PUBLIC hg_return_t
hg_proc_array(hg_proc_t proc, char **addrp, uint32_t *sizep,
    uint32_t maxsize, uint32_t elsize, hg_proc_cb_t elproc);

/**
 * A fixed length array of elements (xdr_vector).
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param basep [IN]            base address of vector array
 * \param nelem [IN]            number of elements in the array
 * \param elemsize [IN]         size of one element
 * \param xelem [IN]            proc fn to handle a vector array element
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
HG_PUBLIC hg_return_t
hg_proc_vector(hg_proc_t proc, char *basep, uint32_t nelem, uint32_t elemsize,
    hg_proc_cb_t xelem);

/**
 * Pointer indirection (xdr_reference).  can be used to help encode/decode
 * basic structs with pointers.  Decode will allocate the memory we
 * indirect to (if *pp is NULL).
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param pp [IN/OUT]           *pp reference that points to our data
 * \param size [IN]             size of the data we reference
 * \param pr [IN]               proc fn to handle the reference
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
HG_PUBLIC hg_return_t
hg_proc_reference(hg_proc_t proc, char **pp, uint32_t size, hg_proc_cb_t pr);

/**
 * A pointer, the pointer is allowed to be NULL (xdr_pointer).   coded
 * as a union (NULL pointer or data structure).  uses hg_proc_reference()
 * if pointer is non-NULL, so decode will allocate memory if needed.
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param objpp [IN/OUT]        *objpp is the pointer
 * \param obj_size [IN]         size of object we point to
 * \param xobj [IN]             proc fn to handle the pointer
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
HG_PUBLIC hg_return_t
hg_proc_pointer(hg_proc_t proc, char **objpp, uint32_t obj_size,
                hg_proc_cb_t xobj);

/*
 * legacy hg_proc_hg_const_string_t() and hg_proc_hg_string_t()
 * string functions.  these now use rpcgen's hg_proc_string() as
 * a backend.  On decode we assume that the string pointer may be
 * uninitialized, so we set it to NULL to ensure that hg_proc_string()
 * calls malloc to allocate string memory (so callers cannot provide
 * their own buffer -- we always malloc).
 */

/**
 * Generic processing routine.
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param data [IN/OUT]         pointer to data
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
static HG_INLINE hg_return_t
hg_proc_hg_const_string_t(hg_proc_t proc, void *data);

/**
 * Generic processing routine.
 *
 * \param proc [IN/OUT]         abstract processor object
 * \param data [IN/OUT]         pointer to data
 *
 * \return HG_SUCCESS or corresponding HG error code
 */
static HG_INLINE hg_return_t
hg_proc_hg_string_t(hg_proc_t proc, void *data);

/************************************/
/* Local Type and Struct Definition */
/************************************/

/*---------------------------------------------------------------------------*/
static HG_INLINE hg_return_t
hg_proc_hg_const_string_t(hg_proc_t proc, void *data)
{
    if (hg_proc_get_op(proc) == HG_DECODE) {
        *( (char **) data ) = NULL;
    }
    return hg_proc_string(proc, data, UINT32_MAX);
}

/*---------------------------------------------------------------------------*/
static HG_INLINE hg_return_t
hg_proc_hg_string_t(hg_proc_t proc, void *data)
{
    if (hg_proc_get_op(proc) == HG_DECODE) {
        *( (char **) data ) = NULL;
    }
    return hg_proc_string(proc, data, UINT32_MAX);
}

#ifdef __cplusplus
}
#endif
#endif /* MERCURY_PROC_EXTRA_H */
