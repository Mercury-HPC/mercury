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
 * xdr_reference.c, Generic XDR routines implementation.
 *
 * Copyright (C) 1987, Sun Microsystems, Inc.
 *
 * These are the "non-trivial" xdr primitives used to serialize and de-serialize
 * "pointers".  See xdr.h for more info on the interface to xdr.
 */

#include <err.h>
#include <stdlib.h>
#include "mercury_proc_extra.h"

/*
 * an indirect pointer
 * this is for recursively translating a structure that is
 * referenced by a pointer inside the structure that is currently being
 * translated.  pp references a pointer to storage. If *pp is null
 * the  necessary storage is allocated.
 * size is the sizeof the referenced structure.
 * proc is the routine to handle the referenced structure.
 */
hg_return_t
hg_proc_reference(hg_proc_t proc, char **pp, uint32_t size, hg_proc_cb_t pr)
{
	char *loc = *pp;
	hg_return_t stat;

	if (loc == NULL)
		switch (hg_proc_get_op(proc)) {
		case HG_FREE:
			return (HG_SUCCESS);

		case HG_DECODE:
			*pp = loc = malloc(size);
			if (loc == NULL) {
				warn("%s: out of memory", __func__);
				return (HG_NOMEM);
			}
			memset(loc, 0, size);
			break;

		case HG_ENCODE:
			break;
		}

	stat = (*pr)(proc, loc);

	if (hg_proc_get_op(proc) == HG_FREE) {
		free(loc);
		*pp = NULL;
	}
	return (stat);
}


/*
 * handle a pointer to a possibly recursive data structure. This
 * differs with reference in that it can serialize/deserialize
 * trees correctly.
 *
 *  What's sent is actually a union:
 *
 *  union object_pointer switch (boolean b) {
 *  case TRUE: object_data data;
 *  case FALSE: void nothing;
 *  }
 *
 * > objpp: Pointer to the pointer to the object.
 * > obj_size: size of the object.
 * > xobj: routine to code an object.
 *
 */
hg_return_t
hg_proc_pointer(hg_proc_t proc, char **objpp, uint32_t obj_size,
                hg_proc_cb_t xobj)
{
	hg_return_t ret;
	hg_bool_t more_data;

	more_data = (*objpp != NULL) ? HG_TRUE : HG_FALSE;
	if ((ret = hg_proc_hg_bool_t(proc, &more_data)) != HG_SUCCESS) {
		return (ret);
	}
	if (more_data == HG_FALSE) {
		*objpp = NULL;
		return (HG_SUCCESS);
	}
	return (hg_proc_reference(proc,objpp,obj_size,xobj));
}
