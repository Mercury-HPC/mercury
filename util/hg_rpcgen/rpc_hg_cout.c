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
 * rpc_cout.c, XDR routine outputter for the RPC protocol compiler
 */
#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rpc_scan.h"
#include "rpc_parse.h"
#include "rpc_util.h"

static int findtype(definition *, const char *);
static int undefined(const char *);
static void print_generic_header(const char *, int);
static void print_header(definition *);
static void print_trailer(void);
static void print_ifopen(int, const char *);
static void print_ifarg(const char *);
static void print_ifsizeof(const char *, const char *);
static void print_ifclose(int);
static void print_ifstat(int, const char *, const char *, relation,
    const char *, const char *, const char *);
static void emit_enum(definition *);
static void emit_union(definition *);
static void emit_struct(definition *);
static void emit_typedef(definition *);
static void print_stat(int, declaration *);

/*
 * Emit the C-routine for the given definition
 */
void
emit(definition *def)
{
	bas_type *bas;
	if (def->def_kind == DEF_CONST) {
		return;
	}
	if (def->def_kind == DEF_PROGRAM) {
		return;
	}
	if (def->def_kind == DEF_TYPEDEF) {
		/* now we need to handle declarations like struct typedef foo
		 * foo; since we dont want this to be expanded into 2 calls to
		 * xdr_foo */

		if (strcmp(def->def.ty.old_type, def->def_name) == 0)
			return;
	};

	/* check for reserved/predefined type */
	bas = find_type(def->def_name);
	if (bas) {
		if (bas->length == 0)
			error("Type '%s' is reserved (used in "
					"mercury_proc.h).", def->def_name);
		error("Type '%s' already defined by mercury.", def->def_name);
	}

	print_header(def);

	switch (def->def_kind) {
	case DEF_UNION:
		emit_union(def);
		break;
	case DEF_ENUM:
		emit_enum(def);
		break;
	case DEF_STRUCT:
		emit_struct(def);
		break;
	case DEF_TYPEDEF:
		emit_typedef(def);
		break;
	case DEF_PROGRAM:
	case DEF_CONST:
		errx(1, "Internal error at %s:%d: Case %d not handled",
		    __FILE__, __LINE__, def->def_kind);
		break;
	}
	print_trailer();
}

/* callback compare function for FINDVAL */
static int
findtype(definition *def, const char *type)
{

	if (def->def_kind == DEF_PROGRAM || def->def_kind == DEF_CONST) {
		return (0);
	} else {
		return (streq(def->def_name, type));
	}
}

static int
undefined(const char *type)
{
	definition *def;

	def = (definition *) FINDVAL(defined, type, findtype);


	return (def == NULL);
}

static void
print_generic_header(const char *procname, int pointerp)
{
	f_print(fout, "\n");
	f_print(fout, "hg_return_t\n");
	f_print(fout, "hg_proc_%s(", procname);
	f_print(fout, "hg_proc_t proc, void *proc_data)\n{\n");
	f_print(fout, "\t%s ", procname);
	if (pointerp)
		f_print(fout, "*");
	f_print(fout, "objp = (%s%s) proc_data;\n", procname,
		(pointerp) ? " *" : "");
	f_print(fout, "\thg_return_t ret;\n");
}

static void
print_header(definition *def)
{
	print_generic_header(def->def_name,
	    1 ||   /* XXX: hg_rpcgen currently always uses a pointer */
	    def->def_kind != DEF_TYPEDEF ||
	    !isvectordef(def->def.ty.old_type, def->def.ty.rel));
}

static void
print_trailer(void)
{
	f_print(fout, "\treturn (HG_SUCCESS);\n");
	f_print(fout, "}\n");
}


static void
print_ifopen(int indent, const char *name)
{
	tabify(fout, indent);
	f_print(fout, "if ((ret = hg_proc_%s(proc", name);
}

static void
print_ifarg(const char *arg)
{
	f_print(fout, ", %s", arg);
}

static void
print_ifsizeof(const char *prefix, const char *type)
{
	if (streq(type, "bool")) {
		f_print(fout, ", (unsigned int)sizeof(uint8_t), %s",
			"hg_proc_uint8_t");
	} else {
		f_print(fout, ", (unsigned int)sizeof(");
		if (undefined(type) && prefix) {
			f_print(fout, "%s ", prefix);
		}
		f_print(fout, "%s), hg_proc_%s", type, type);
	}
}

static void
print_ifclose(int indent)
{
	f_print(fout, ")) != HG_SUCCESS)\n");
	tabify(fout, indent);
	f_print(fout, "\treturn (ret);\n");
}

static void
print_ifstat(int indent, const char *prefix, const char *type, relation rel,
	     const char *amax, const char *objname, const char *name)
{
	const char *alt = NULL;

	switch (rel) {
	case REL_POINTER:
		print_ifopen(indent, "pointer");
		print_ifarg("(char **)(void *)");
		f_print(fout, "%s", objname);
		print_ifsizeof(prefix, type);
		break;
	case REL_VECTOR:
		if (streq(type, "string")) {
			alt = "string";
			/*
			 * this case should not be possible as the parser
			 * only allows REL_ARRAY strings (so error() it).
			 */
			error("Unexpected REL_VECTOR string");
		} else if (streq(type, "opaque")) {
			/* hg_proc_bytes() corresponds to xdr_opaque() */
			alt = "bytes"; /* XXX was 'opaque' in xdr */
		}
		if (alt) {
			print_ifopen(indent, alt);
			print_ifarg(objname);
		} else {
			print_ifopen(indent, "vector");
			print_ifarg("(char *)(void *)");
			f_print(fout, "%s", objname);
		}
		print_ifarg(amax);
		if (!alt) {
			print_ifsizeof(prefix, type);
		}
		break;
	case REL_ARRAY:
		if (streq(type, "string")) {
			alt = "string";
		} else if (streq(type, "opaque")) {
			/* hg_proc_varbytes() corresponds to xdr_bytes() */
			alt = "varbytes";  /* XXX was 'bytes' in xdr */
		}
		if (streq(type, "string")) {
			print_ifopen(indent, alt);
			print_ifarg(objname);
		} else {
			if (alt) {
				print_ifopen(indent, alt);
			} else {
				print_ifopen(indent, "array");
			}
			print_ifarg("(char **)(void *)");
			if (*objname == '&') {
				f_print(fout, "%s.%s_val, (uint32_t *)%s.%s_len",
				    objname, name, objname, name);
			} else {
				f_print(fout, "&%s->%s_val, (uint32_t *)&%s->%s_len",
				    objname, name, objname, name);
			}
		}
		print_ifarg(amax);
		if (!alt) {
			print_ifsizeof(prefix, type);
		}
		break;
	case REL_ALIAS:
		if (streq(type, "bool")) {
			alt = "uint8_t";
		}
		if (alt) {
			print_ifopen(indent, alt);
		} else {
			print_ifopen(indent, type);
		}
		print_ifarg(objname);
		break;
	}
	print_ifclose(indent);
}
/* ARGSUSED */
static void
emit_enum(definition *def)
{
	tabify(fout, 1);
	f_print(fout, "{\n");
	tabify(fout, 2);
	f_print(fout, "int32_t et = (int32_t)*objp;\n");
	print_ifopen(2, "int32_t");
	print_ifarg("&et");
	print_ifclose(2);
	tabify(fout, 2);
	f_print(fout, "*objp = (%s)et;\n", def->def_name);
	tabify(fout, 1);
	f_print(fout, "}\n");
}

static void
emit_union(definition *def)
{
	declaration *dflt;
	case_list *cl;
	declaration *cs;
	char   *object;
	static const char vecformat[] = "objp->%s_u.%s";
	static const char format[] = "&objp->%s_u.%s";

	f_print(fout, "\n");
	print_stat(1, &def->def.un.enum_decl);
	f_print(fout, "\tswitch (objp->%s) {\n", def->def.un.enum_decl.name);
	for (cl = def->def.un.cases; cl != NULL; cl = cl->next) {
		f_print(fout, "\tcase %s:\n", cl->case_name);
		if (cl->contflag == 1)	/* a continued case statement */
			continue;
		cs = &cl->case_decl;
		if (!streq(cs->type, "void")) {
			object = alloc(strlen(def->def_name) + strlen(format) +
			    strlen(cs->name) + 1);
			if (isvectordef(cs->type, cs->rel)) {
				s_print(object, vecformat, def->def_name,
				    cs->name);
			} else {
				s_print(object, format, def->def_name,
				    cs->name);
			}
			print_ifstat(2, cs->prefix, cs->type, cs->rel,
			    cs->array_max, object, cs->name);
			free(object);
		}
		f_print(fout, "\t\tbreak;\n");
	}
	dflt = def->def.un.default_decl;
	f_print(fout, "\tdefault:\n");
	if (dflt != NULL) {
		if (!streq(dflt->type, "void")) {
			object = alloc(strlen(def->def_name) + strlen(format) +
			    strlen(dflt->name) + 1);
			if (isvectordef(dflt->type, dflt->rel)) {
				s_print(object, vecformat, def->def_name,
				    dflt->name);
			} else {
				s_print(object, format, def->def_name,
				    dflt->name);
			}
			print_ifstat(2, dflt->prefix, dflt->type, dflt->rel,
			    dflt->array_max, object, dflt->name);
			free(object);
		}
		f_print(fout, "\t\tbreak;\n");
	} else {
		f_print(fout, "\t\treturn (ret);\n");
	}

	f_print(fout, "\t}\n");
}

static void
emit_struct(definition *def)
{
	decl_list *dl;

	f_print(fout, "\n");
	for (dl = def->def.st.decls; dl != NULL; dl = dl->next)
		print_stat(1, &dl->decl);
	return;
}

static void
emit_typedef(definition *def)
{
	const char *prefix = def->def.ty.old_prefix;
	const char *type = def->def.ty.old_type;
	const char *amax = def->def.ty.array_max;
	relation rel = def->def.ty.rel;

	f_print(fout, "\n");
	print_ifstat(1, prefix, type, rel, amax, "objp", def->def_name);
}

static void
print_stat(int indent, declaration *dec)
{
	const char *prefix = dec->prefix;
	const char *type = dec->type;
	const char *amax = dec->array_max;
	relation rel = dec->rel;
	char    name[256];

	if (isvectordef(type, rel)) {
		s_print(name, "objp->%s", dec->name);
	} else {
		s_print(name, "&objp->%s", dec->name);
	}
	print_ifstat(indent, prefix, type, rel, amax, name, dec->name);
}
