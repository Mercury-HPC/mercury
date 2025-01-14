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
 * rpc_hout.c, Header file outputter for the RPC protocol compiler
 */
#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include "rpc_scan.h"
#include "rpc_parse.h"
#include "rpc_util.h"

static void pconstdef(definition *);
static void pstructdef(definition *);
static void puniondef(definition *);
static void pdeclaration(const char *, declaration *, int, const char *);
static void pdefine(const char *, const char *);
static void penumdef(definition *);
static void ptypedef(definition *);
static int undefined2(const char *, const char *);
static void cplusplusstart(void);
static void cplusplusend(void);

/*
 * Print the C-version of an xdr definition
 */
void
print_datadef(definition *def)
{

	if (def->def_kind != DEF_CONST && def->def_kind != DEF_PROGRAM) {
		f_print(fout, "\n");
	}
	switch (def->def_kind) {
	case DEF_STRUCT:
		pstructdef(def);
		break;
	case DEF_UNION:
		puniondef(def);
		break;
	case DEF_ENUM:
		penumdef(def);
		break;
	case DEF_TYPEDEF:
		ptypedef(def);
	case DEF_PROGRAM:
		/* handle data only */
		break;
	case DEF_CONST:
		pconstdef(def);
		break;
	}
}

void
print_funcdef(definition *def, int *did)
{
	bas_type *bas;
	switch (def->def_kind) {
	case DEF_PROGRAM:
	case DEF_CONST:
		break;
	case DEF_TYPEDEF:
	case DEF_ENUM:
	case DEF_UNION:
	case DEF_STRUCT:
		if (!*did) {
		    f_print(fout, "\n");
		    cplusplusstart();
		    *did = 1;
		}

		/* check for reserved/predefined type */
		bas = find_type(def->def_name);
		if (bas) {
			if (bas->length == 0)
				error("Type '%s' is reserved (used in "
					"mercury_proc.h).", def->def_name);
			error("Type '%s' already defined by mercury.",
					def->def_name);
		}

		/* hg_proc funtion's second arg is always a void* */
		f_print(fout, "hg_return_t hg_proc_%s(hg_proc_t proc, "
			       "void *%s);\n", def->def_name, def->def_name);
		break;
	}
}

void
print_funcend(int did) {
	if (did) {
		cplusplusend();
	}
}

static void
pconstdef(definition *def)
{
	pdefine(def->def_name, def->def.co);
}

static void
pstructdef(definition *def)
{
	decl_list *l;
	const char *name = def->def_name;

	f_print(fout, "struct %s {\n", name);
	for (l = def->def.st.decls; l != NULL; l = l->next) {
		pdeclaration(name, &l->decl, 1, ";\n");
	}
	f_print(fout, "};\n");
	f_print(fout, "typedef struct %s %s;\n", name, name);
}

static void
puniondef(definition *def)
{
	case_list *l;
	const char *name = def->def_name;
	declaration *decl;

	f_print(fout, "struct %s {\n", name);
	decl = &def->def.un.enum_decl;
	if (streq(decl->type, "bool")) {
		f_print(fout, "\tuint8_t %s;\n", decl->name);
	} else {
		f_print(fout, "\t%s %s;\n", decl->type, decl->name);
	}
	f_print(fout, "\tunion {\n");
	for (l = def->def.un.cases; l != NULL; l = l->next) {
		if (l->contflag == 0)
			pdeclaration(name, &l->case_decl, 2, ";\n");
	}
	decl = def->def.un.default_decl;
	if (decl && !streq(decl->type, "void")) {
		pdeclaration(name, decl, 2, ";\n");
	}
	f_print(fout, "\t} %s_u;\n", name);
	f_print(fout, "};\n");
	f_print(fout, "typedef struct %s %s;\n", name, name);
}

static void
pdefine(const char *name, const char *num)
{
	f_print(fout, "#define %s %s\n", name, num);
}

static void
cplusplusstart(void)
{
	if (BSDflag)
		f_print(fout, "__BEGIN_DECLS\n");
	else
		f_print(fout, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
}

static void
cplusplusend(void)
{
	if (BSDflag)
		f_print(fout, "__END_DECLS\n");
	else
		f_print(fout, "#ifdef __cplusplus\n};\n#endif\n");
}

static void
penumdef(definition *def)
{
	const char *name = def->def_name;
	enumval_list *l;
	const char *last = NULL;
	int count = 0;
	const char *first = "";

	f_print(fout, "enum %s {\n", name);
	for (l = def->def.en.vals; l != NULL; l = l->next) {
		f_print(fout, "%s\t%s", first, l->name);
		if (l->assignment) {
			f_print(fout, " = %s", l->assignment);
			last = l->assignment;
			count = 1;
		} else {
			if (last == NULL) {
				f_print(fout, " = %d", count++);
			} else {
				f_print(fout, " = %s + %d", last, count++);
			}
		}
		first = ",\n";
	}
	f_print(fout, "\n};\n");
	f_print(fout, "typedef enum %s %s;\n", name, name);
}

static void
ptypedef(definition *def)
{
	const char *name = def->def_name;
	const char *old = def->def.ty.old_type;
	char    prefix[8];	/* enough to contain "struct ", including NUL */
	relation rel = def->def.ty.rel;

	/* example: 'typedef struct foo *foolist' => name=foolist, old=foo */
	if (!streq(name, old)) {
		/* adjust special case base types */
		if (streq(old, "string")) {		/* string => char* */
			old = "char";
			rel = REL_POINTER;
		} else if (streq(old, "opaque")) {	/* opaque => char */
			old = "char";
		} else if (streq(old, "bool")) {	/* bool => bool_t */
			old = "uint8_t";
		}
		/* if we did not define 'old' and it has a prefix, use it */
		/* allows us to use struct's typedef'd name if we made it */
		if (undefined2(old, name) && def->def.ty.old_prefix) {
			s_print(prefix, "%s ", def->def.ty.old_prefix);
		} else {
			prefix[0] = 0;
		}
		f_print(fout, "typedef ");
		switch (rel) {
		case REL_ARRAY:
			f_print(fout, "struct {\n");
			f_print(fout, "\tuint32_t %s_len;\n", name);
			f_print(fout, "\t%s%s *%s_val;\n", prefix, old, name);
			f_print(fout, "} %s", name);
			break;
		case REL_POINTER:
			f_print(fout, "%s%s *%s", prefix, old, name);
			break;
		case REL_VECTOR:
			f_print(fout, "%s%s %s[%s]", prefix, old, name,
			    def->def.ty.array_max);
			break;
		case REL_ALIAS:
			f_print(fout, "%s%s %s", prefix, old, name);
			break;
		}
		f_print(fout, ";\n");
	}
}

static void
pdeclaration(const char *name, declaration *dec, int tab,
	     const char *separator)
{
	char    buf[8];		/* enough to hold "struct ", include NUL */
	const char *prefix;
	const char *type;

	if (streq(dec->type, "void")) {
		return;
	}
	tabify(fout, tab);
	/* struct that has itself in it (e.g. a pointer) */
	if (streq(dec->type, name) && !dec->prefix) {
		f_print(fout, "struct ");
	}
	if (streq(dec->type, "string")) {
		f_print(fout, "char *%s", dec->name);
	} else {
		prefix = "";
		if (streq(dec->type, "bool")) {
			type = "uint8_t";
		} else if (streq(dec->type, "opaque")) {
				type = "char";
		} else {
			if (dec->prefix) {
				s_print(buf, "%s ", dec->prefix);
				prefix = buf;
			}
			type = dec->type;
		}
		switch (dec->rel) {
		case REL_ALIAS:
			f_print(fout, "%s%s %s", prefix, type, dec->name);
			break;
		case REL_VECTOR:
			f_print(fout, "%s%s %s[%s]", prefix, type, dec->name,
			    dec->array_max);
			break;
		case REL_POINTER:
			f_print(fout, "%s%s *%s", prefix, type, dec->name);
			break;
		case REL_ARRAY:
			f_print(fout, "struct {\n");
			tabify(fout, tab);
			f_print(fout, "\tuint32_t %s_len;\n", dec->name);
			tabify(fout, tab);
			f_print(fout, "\t%s%s *%s_val;\n", prefix, type, dec->name);
			tabify(fout, tab);
			f_print(fout, "} %s", dec->name);
			break;
		}
	}
	f_print(fout, "%s", separator);  /* normally ";\n" */
}

/* look at defns from start of file until we hit 'stop' */
/* ret 1 if 'type' was not defined in above range, o.w. ret 0 */
static int
undefined2(const char *type, const char *stop)
{
	list   *l;
	definition *def;

	for (l = defined; l != NULL; l = l->next) {
		def = (definition *) l->val;
		if (def->def_kind != DEF_PROGRAM) {
			if (streq(def->def_name, stop)) {
				return (1);
			} else if (streq(def->def_name, type)) {
					return (0);
			}
		}
	}
	return (1);
}
