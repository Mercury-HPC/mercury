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
 * xdr_array.c, Generic XDR routines implementation.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * These are the "non-trivial" xdr primitives used to serialize and de-serialize
 * arrays.  See xdr.h for more info on the interface to xdr.
 */

#include <err.h>
#include <stdlib.h>
#include "mercury_proc_extra.h"

/*
 * handle an array of arbitrary elements
 * *addrp is a pointer to the array, *sizep is the number of elements.
 * If addrp is NULL (*sizep * elsize) bytes are allocated.
 * elsize is the size (in bytes) of each element, and elproc is the
 * procedure to call to handle each element of the array.
 */
hg_return_t
hg_proc_array(hg_proc_t proc, char **addrp, uint32_t *sizep,
    uint32_t maxsize, uint32_t elsize, hg_proc_cb_t elproc)
{
	uint32_t i;
	char *target = *addrp;
	uint32_t c;  /* the actual element count */
	hg_return_t stat = HG_SUCCESS;
	uint32_t nodesize;
	hg_return_t ret;

	/* like strings, arrays are really counted arrays */
	if ((ret = hg_proc_uint32_t(proc, sizep)) != HG_SUCCESS)
		return (HG_FALSE);

	c = *sizep;
	if ((c > maxsize || UINT32_MAX/elsize < c) &&
	    (hg_proc_get_op(proc) != HG_FREE))
		return (HG_FALSE);
	nodesize = c * elsize;

	/*
	 * if we are deserializing, we may need to allocate an array.
	 * We also save time by checking for a null array if we are freeing.
	 */
	if (target == NULL)
		switch (hg_proc_get_op(proc)) {
		case HG_DECODE:
			if (c == 0)
				return (HG_SUCCESS);
			*addrp = target = malloc(nodesize);
			if (target == NULL) {
				warn("%s: out of memory", __func__);
				return (HG_NOMEM);
			}
			memset(target, 0, nodesize);
			break;

		case HG_FREE:
			return (HG_SUCCESS);

		case HG_ENCODE:
			break;
	}
	
	/*
	 * now we process each element of array
	 */
	for (i = 0; (i < c) && stat == HG_SUCCESS ; i++) {
		stat = (*elproc)(proc, target);
		target += elsize;
	}

	/*
	 * the array may need freeing
	 */
	if (hg_proc_get_op(proc) == HG_FREE) {
		free(*addrp);
		*addrp = NULL;
	}
	return (stat);
}

/*
 * process a fixed length array. Unlike variable-length arrays,
 * the storage of fixed length arrays is static and unfreeable.
 * > basep: base of the array
 * > size: size of the array
 * > elemsize: size of each element
 * > xelem: routine to process each element
 */
hg_return_t
hg_proc_vector(hg_proc_t proc, char *basep, uint32_t nelem, uint32_t elemsize,
    hg_proc_cb_t xelem)
{
	uint32_t i;
	char *elptr;
	hg_return_t ret;

	elptr = basep;
	for (i = 0; i < nelem; i++) {
		if ((ret = (*xelem)(proc, elptr)) != HG_SUCCESS) {
			return(ret);
		}
		elptr += elemsize;
	}
	return(HG_SUCCESS);	
}
