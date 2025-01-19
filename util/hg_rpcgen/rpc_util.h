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
 * rpc_util.h, Useful definitions for the RPC protocol compiler
 */

/* quick hack for printf formatting checks on most platforms+noreturn */
#if defined(__GNUC__) && (__GNUC__ > 2)
#define xnoreturn() __attribute__((__noreturn__))
#define xprintfattr(fmtarg, firstvararg) \
        __attribute__((__format__ (__printf__, fmtarg, firstvararg)))
#define xscanfattr(fmtarg, firstvararg) \
            __attribute__((__format__ (__scanf__, fmtarg, firstvararg)))
#else
#define xnoreturn()
#define xprintfattr(fmtarg, firstvararg)
#define xscanfattr(fmtarg, firstvararg)
#endif

#define alloc(size)		((char *)malloc((size_t)(size)))
#define ALLOC(object)		((object *)malloc(sizeof(object)))

#define s_print	(void) sprintf
#define f_print (void) fprintf

struct list {
	definition *val;
	struct list *next;
};
typedef struct list list;

#define PUT 1
#define GET 2

/*
 * Global variables
 */
#define MAXLINESIZE 1024
extern char curline[MAXLINESIZE];
extern char *where;
extern int linenum;
extern int docleanup;

extern const char *infilename;
extern FILE *fout;
extern FILE *fin;

extern list *defined;

/*
 * All the option flags
 */
extern int BSDflag;

/*
 * rpc_util routines
 */

#define STOREVAL(list,item)	\
	storeval(list,item)

#define FINDVAL(list,item,finder) \
	findval(list, item, finder)

void reinitialize(void);
int streq(const char *, const char *);
definition *findval(list *, const char *,
			 int (*)(definition *, const char *));
void storeval(list **, definition *);
const char *fixtype(const char *);
void ptype(const char *, const char *, int);
int isvectordef(const char *, relation);
char *locase(const char *);
void pvname_svc(const char *, const char *);
void pvname(const char *, const char *);
xnoreturn() xprintfattr(1, 2) void error(const char *, ...);
void crash(void);
void record_open(const char *);
void expected1(tok_kind) xnoreturn();
void expected2(tok_kind, tok_kind) xnoreturn();
void expected3(tok_kind, tok_kind, tok_kind) xnoreturn();
void tabify(FILE *, int);
char *make_argname(const char *, const char *);
void add_type(int, const char *);
bas_type *find_type(const char *);

/*
 * rpc_cout routines
 */
void emit(definition *);

/*
 * rpc_hout routines
 */

void print_datadef(definition *);
void print_funcdef(definition *, int *);
void print_funcend(int);
