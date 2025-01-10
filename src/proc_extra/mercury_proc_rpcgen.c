/*
 * Copyright (c) 2010, Oracle America, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the "Oracle America, Inc." nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *   FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *   COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 *   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *   GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * xdr.c, Generic XDR routines implementation.
 *
 * Copyright (C) 1986, Sun Microsystems, Inc.
 *
 * These are the "generic" xdr routines used to serialize and de-serialize
 * most common data items.  See xdr.h for more info on the interface to
 * xdr.
 */

#include <err.h>
#include <stdlib.h>
#include "mercury_proc_extra.h"

/*
 * counted bytes
 * *cpp is a pointer to the bytes, *sizep is the count.
 * If *cpp is NULL maxsize bytes are allocated
 */
hg_return_t
hg_proc_varbytes(hg_proc_t proc, char **cpp, uint32_t *sizep, uint32_t maxsize)
{
	char *sp;  		/* sp is the actual string pointer */
	uint32_t nodesize;
	hg_return_t ret;
	hg_bool_t allocated = HG_FALSE;

	sp = *cpp;

	/*
	 * first deal with the length since bytes are counted
	 */
	if ((ret = hg_proc_uint32_t(proc, sizep)) != HG_SUCCESS) {
		return (ret);
	}
	nodesize = *sizep;
	if ((nodesize > maxsize) && (hg_proc_get_op(proc) != HG_FREE)) {
		return(HG_OVERFLOW);
	}

	/*
	 * now deal with the actual bytes
	 */
	switch (hg_proc_get_op(proc)) {
	case HG_DECODE:
		if (nodesize == 0) {
			return (HG_SUCCESS);
		}
		if (sp == NULL) {
			*cpp = sp = malloc(nodesize);
			allocated = HG_TRUE;
		}
		if (sp == NULL) {
			warn("%s: out of memory", __func__);
			return (HG_NOMEM);
		}
		/* FALLTHROUGH */

	case HG_ENCODE:
		ret = hg_proc_bytes(proc, sp, nodesize);
		if ((hg_proc_get_op(proc) == HG_DECODE) &&
		    (ret != HG_SUCCESS)) {
			if (allocated == HG_TRUE) {
				free(sp);
				*cpp = NULL;
			}
		}
		return (ret);

	case HG_FREE:
		if (sp != NULL) {
			free(sp);
			*cpp = NULL;
		}
		return (HG_SUCCESS);
	}
	/* NOTREACHED */
	return (HG_OVERFLOW);
}

/*
 * null terminated ASCII strings
 * we deal with "C strings" - arrays of bytes that are
 * terminated by a NULL character.  The parameter cpp references a
 * pointer to storage; If the pointer is null, then the necessary
 * storage is allocated.  The last parameter is the max allowed length
 * of the string as specified by a protocol.
 */
hg_return_t
hg_proc_string(hg_proc_t proc, char **cpp, uint32_t maxsize)
{
	char *sp;  		/* sp is the actual string pointer */
	uint32_t size = 0;	/* XXX: GCC */
	uint32_t nodesize;
	size_t len;
	hg_return_t ret;
	hg_bool_t allocated = HG_FALSE;

	sp = *cpp;

	/*
	 * first deal with the length since strings are counted-strings
	 */
	switch (hg_proc_get_op(proc)) {
	case HG_FREE:
		if (sp == NULL) {
			return(HG_SUCCESS);	/* already free */
		}
		/* FALLTHROUGH */
	case HG_ENCODE:
		len = strlen(sp);
		size = (uint32_t)len;
		break;
	case HG_DECODE:
		break;
	}
	if ((ret = hg_proc_uint32_t(proc, &size)) != HG_SUCCESS) {
		return (ret);
	}
	if (size > maxsize) {
		return (HG_OVERFLOW);
	}

	nodesize = size + 1;

	/*
	 * now deal with the actual bytes
	 */
	switch (hg_proc_get_op(proc)) {

	case HG_DECODE:
		if (nodesize == 0) {
			return (HG_SUCCESS);
		}
		if (sp == NULL) {
			*cpp = sp = malloc(nodesize);
			allocated = HG_TRUE;
		}
		if (sp == NULL) {
			warn("%s: out of memory", __func__);
			return (HG_NOMEM);
		}
		sp[size] = 0;
		/* FALLTHROUGH */

	case HG_ENCODE:
		ret = hg_proc_bytes(proc, sp, size);
		if ((hg_proc_get_op(proc) == HG_DECODE) &&
		    (ret != HG_SUCCESS)) {
			if (allocated == HG_TRUE) {
				free(sp);
				*cpp = NULL;
			}
		}
		return (ret);

	case HG_FREE:
		free(sp);
		*cpp = NULL;
		return (HG_SUCCESS);
	}
	/* NOTREACHED */
	return (HG_OVERFLOW);
}
