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
 * rpc_main.c, Top level of the RPC protocol compiler.
 */

#define HG_RPCGEN_VERSION "199506"/* This program's version (year & month) */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rpc_scan.h"
#include "rpc_parse.h"
#include "rpc_util.h"

#define EXTEND	1		/* alias for TRUE */
#define DONT_EXTEND	0	/* alias for FALSE */

struct commandline {
	int     cflag;		/* code/decode C routines */
	int     hflag;		/* header file */
	char   *infile;		/* input module name */
	char   *outfile;	/* output module name */
};

static char *cmdname;

static char *CPP;
static char CPPFLAGS[] = "-C";
static char pathbuf[MAXPATHLEN + 1];

#define ARGLISTLEN	20
#define FIXEDARGS	2

static char *arglist[ARGLISTLEN];
static int argcount = FIXEDARGS;


int	BSDflag;		/* use BSD cplusplus guard macros */
int	docleanup = 1;		/* cause atexit to remove files */

/*
 * removed xdr_inline() related code.  base mercury fns already
 * inline data movement for basic types into memcpy ops, plus
 * mercury may be config'd to do checksum code (not relevant in xdr).
 */

static char *extendfile(const char *, const char *);
static void open_output(const char *, const char *);
static void add_warning(void);
static void clear_args(void);
static void open_input(char *, char *);
static void c_output(char *, char *, int, const char *);
static void initialize_base_types(void);
static char *generate_guard(const char *);
static void h_output(char *, char *, int, const char *);
static void addarg(char *);
static void putarg(int, char *);
static void checkfiles(const char *, const char *);
static int parseargs(int, char *[], struct commandline *);
static void usage(void);

int
main(int argc, char *argv[])
{
	struct commandline cmd;

	if ((CPP = getenv("HG_RPCGEN_CPP")) == NULL) {
		CPP = "/usr/bin/cpp";
		if (access(CPP, X_OK))
			CPP = "/usr/bin/clang-cpp";
	}

	(void) memset((char *) &cmd, 0, sizeof(struct commandline));
	clear_args();
	atexit(crash);
	if (!parseargs(argc, argv, &cmd))
		usage();

	if (cmd.cflag || cmd.hflag) {
		checkfiles(cmd.infile, cmd.outfile);
	} else
		checkfiles(cmd.infile, NULL);

	if (cmd.cflag) {
		c_output(cmd.infile, "-DHG_RPC_PROC", DONT_EXTEND, cmd.outfile);
	} else if (cmd.hflag) {
		h_output(cmd.infile, "-DHG_RPC_HDR", DONT_EXTEND, cmd.outfile);
	} else {
		c_output(cmd.infile, "-DHG_RPC_PROC", EXTEND, "_proc.c");
		reinitialize();
		h_output(cmd.infile, "-DHG_RPC_HDR", EXTEND, ".h");
	}
	docleanup = 0;
	exit(0);
	/* NOTREACHED */
}
/*
 * add extension to filename
 */
static char *
extendfile(const char *path, const char *ext)
{
	const char *file;
	char   *res;
	const char *p;

	if ((file = strrchr(path, '/')) == NULL)
		file = path;
	else
		file++;

	res = alloc(strlen(file) + strlen(ext) + 1);
	if (res == NULL) {
		err(EXIT_FAILURE, "Out of memory");
	}
	p = strrchr(file, '.');
	if (p == NULL) {
		p = file + strlen(file);
	}
	(void) strcpy(res, file);
	(void) strcpy(res + (p - file), ext);
	return (res);
}
/*
 * Open output file with given extension
 */
static void
open_output(const char *infile, const char *outfile)
{

	if (outfile == NULL) {
		fout = stdout;
		return;
	}
	if (infile != NULL && streq(outfile, infile)) {
		errx(EXIT_FAILURE, "Output would overwrite `%s'", infile);
	}
	fout = fopen(outfile, "w");
	if (fout == NULL) {
		err(EXIT_FAILURE, "Can't open `%s'", outfile);
	}
	record_open(outfile);

}

static void
add_warning(void)
{
	f_print(fout, "/*\n");
	f_print(fout, " * Please do not edit this file.\n");
	f_print(fout, " * It was generated using hg_rpcgen.\n");
	f_print(fout, " */\n\n");
}

/* clear list of arguments */
static void
clear_args(void)
{
	int     i;
	for (i = FIXEDARGS; i < ARGLISTLEN; i++)
		arglist[i] = NULL;
	argcount = FIXEDARGS;
}

/*
 * Open input file with given define for C-preprocessor
 */
static void
open_input(char *infile, char *define)
{
	int     pd[2];

	infilename = (infile == NULL) ? "<stdin>" : infile;
	if (pipe(pd) < 0)
		err(EXIT_FAILURE, "pipe");
	switch (fork()) {
	case 0:
		putarg(0, CPP);
		putarg(1, CPPFLAGS);
		addarg(define);
		if (infile)
			addarg(infile);
		addarg(NULL);
		(void) close(1);
		(void) dup2(pd[1], 1);
		(void) close(pd[0]);
		execvp(arglist[0], arglist);
		err(EXIT_FAILURE, "$HG_RPCGEN_CPP: %s", CPP);
	case -1:
		err(EXIT_FAILURE, "fork");
	}
	(void) close(pd[1]);
	fin = fdopen(pd[0], "r");
	if (fin == NULL) {
		err(EXIT_FAILURE, "Can't open `%s'", infilename);
	}
}

/*
 * Compile into a PROC routine output file
 */

static void
c_output(char *infile, char *define, int extend, const char *outfile)
{
	definition *def;
	char   *include;
	const char *outfilename;
	long    tell;

	initialize_base_types();
	open_input(infile, define);
	outfilename = extend ? extendfile(infile, outfile) : outfile;
	open_output(infile, outfilename);
	add_warning();
	if (infile && (include = extendfile(infile, ".h"))) {
		f_print(fout, "#include \"%s\"\n", include);
		free(include);
		/* .h file already contains mercury includes */
	} else
		f_print(fout, "#include <mercury_proc_extra.h>\n");
	tell = ftell(fout);
	while ((def = get_definition()) != NULL) {
		emit(def);
	}
	if (extend && tell == ftell(fout)) {
		(void) unlink(outfilename);
	}
}


static void
initialize_base_types(void)
{

	/* add all the starting basic types - from mercury_proc.h */
	add_type(1, "hg_int8_t");
	add_type(1, "hg_uint8_t");
	add_type(1, "hg_int16_t");
	add_type(1, "hg_uint16_t");
	add_type(1, "hg_int32_t");
	add_type(1, "hg_uint32_t");
	add_type(1, "hg_int64_t");
	add_type(1, "hg_uint64_t");
	add_type(1, "bytes");

	/* add aliases established with preprocessor defines */
	add_type(1, "int8_t");
	add_type(1, "uint8_t");
	add_type(1, "int16_t");
	add_type(1, "uint16_t");
	add_type(1, "int32_t");
	add_type(1, "uint32_t");
	add_type(1, "int64_t");
	add_type(1, "uint64_t");
	add_type(1, "hg_bool_t");  /* maps to hg_uint8_t */
	add_type(1, "hg_ptr_t");   /* maps to hg_uint64_t */
	add_type(1, "hg_size_t");  /* maps to hg_uint64_t */
	add_type(1, "hg_id_t");    /* maps to hg_uint32_t */

	/*
	 * add other hg_proc_* names used by mercury_proc.h as
	 * reserved words (we overload setting 'len' to zero
	 * to mark reserved words).
	 */
	add_type(0, "memcpy");    /* defined to hg_proc_bytes */
	add_type(0, "raw");       /* also defined to hg_proc_bytes */

	add_type(0, "array");
	add_type(0, "checksum_get");
	add_type(0, "checksum_update");
	add_type(0, "checksum_verify");
	add_type(0, "create");
	add_type(0, "create_set");
	add_type(0, "flush");
	add_type(0, "free");
	add_type(0, "get_class");
	add_type(0, "get_extra_buf");
	add_type(0, "get_extra_size");
	add_type(0, "get_flags");
	add_type(0, "get_handle");
	add_type(0, "get_op");
	add_type(0, "get_size");
	add_type(0, "get_size_left");
	add_type(0, "get_size_used");
	add_type(0, "get_xdr_ptr");
	add_type(0, "hg_bulk_t");
	add_type(0, "hg_const_string_t");
	add_type(0, "hg_string_t");
	add_type(0, "pointer");
	add_type(0, "reference");
	add_type(0, "reset");
	add_type(0, "restore_ptr");
	add_type(0, "save_ptr");
	add_type(0, "set_extra_buf_is_mine");
	add_type(0, "set_handle");
	add_type(0, "set_flags");
	add_type(0, "set_size");
	/* add_type(0, "string"); */   /* caught elsewhere */
	add_type(0, "varbytes");
	add_type(0, "vector");
}

static char *
generate_guard(const char *pathname)
{
	const char *filename;
	char *guard, *tmp, *tmp2, *extdot;

	filename = strrchr(pathname, '/');	/* find last component */
	filename = ((filename == 0) ? pathname : filename + 1);
	guard = strdup(filename);
	if (guard == NULL) {
		err(EXIT_FAILURE, "strdup");
	}
	extdot = strrchr(guard, '.');

	/*
	 * Convert to valid C symbol name and make it upper case.
	 * Map non alphanumerical characters to '_'.
	 *
	 * Leave extension as it is. It will be handled in extendfile().
	 */
	for (tmp = guard; *tmp; tmp++) {
		if (islower((unsigned char)*tmp))
			*tmp = toupper((unsigned char)*tmp);
		else if (isupper((unsigned char)*tmp))
			continue;
		else if (isdigit((unsigned char)*tmp))
			continue;
		else if (*tmp == '_')
			continue;
		else if (tmp == extdot)
			break;
		else
			*tmp = '_';
	}

	/*
	 * Can't have a '_' or '.' at the front of a symbol name, because it
	 * will end up as "__".
	 *
	 * Prefix it with "HG_RPCGEN_".
	 */
	if (guard[0] == '_' || guard[0] == '.') {
		size_t sz = (strlen(guard) + 1) + (sizeof("HG_RPCGEN_") - 1);
		tmp2 = malloc(sz);
		if (!tmp2) {
			errx(EXIT_FAILURE, "malloc");
		}
		snprintf(tmp2, sz, "HG_RPCGEN_%s", guard);
		free(guard);
		guard = tmp2;
	}

	/* Replace the file extension */
	tmp2 = extendfile(guard, "_H_HG_RPCGEN");
	free(guard);
	guard = tmp2;

	return (guard);
}

/*
 * Compile into an XDR header file
 */
static void
h_output(char *infile, char *define, int extend, const char *outfile)
{
	definition *def;
	const char *outfilename;
	long    tell;
	char   *guard;
	list   *l;
	int did;

	initialize_base_types();
	open_input(infile, define);
	outfilename = extend ? extendfile(infile, outfile) : outfile;
	open_output(infile, outfilename);
	add_warning();
	if (outfilename || infile)
		guard = generate_guard(outfilename ? outfilename : infile);
	else {
		guard = strdup("STDIN_");
		if (guard == NULL) {
			err(EXIT_FAILURE, "strdup");
		}
	}

	f_print(fout, "#ifndef _%s\n#define _%s\n\n", guard,
	    guard);

	f_print(fout, "#define HG_RPCGEN_VERSION\t%s\n\n", HG_RPCGEN_VERSION);
	f_print(fout, "#include <mercury_proc_extra.h>\n\n");

	tell = ftell(fout);
	/* print data definitions */
	while ((def = get_definition()) != NULL) {
		print_datadef(def);
	}

	/* print function declarations.  Do this after data definitions
	 * because they might be used as arguments for functions */
	did = 0;
	for (l = defined; l != NULL; l = l->next) {
		print_funcdef(l->val, &did);
	}
	print_funcend(did);

	if (extend && tell == ftell(fout)) {
		(void) unlink(outfilename);
	}
	f_print(fout, "\n#endif /* !_%s */\n", guard);

	free(guard);
}

/*
 * Add another argument to the arg list
 */
static void
addarg(char *cp)
{
	if (argcount >= ARGLISTLEN) {
		errx(EXIT_FAILURE, "Internal error: too many defines");
		/* NOTREACHED */
	}
	arglist[argcount++] = cp;

}

static void
putarg(int pwhere, char *cp)
{
	if (pwhere >= ARGLISTLEN) {
		errx(EXIT_FAILURE, "Internal error: arglist coding error");
		/* NOTREACHED */
	}
	arglist[pwhere] = cp;

}
/*
 * if input file is stdin and an output file is specified then complain
 * if the file already exists. Otherwise the file may get overwritten
 * If input file does not exist, exit with an error
 */

static void
checkfiles(const char *infile, const char *outfile)
{

	struct stat buf;

	if (infile)		/* infile ! = NULL */
		if (stat(infile, &buf) < 0) {
			err(EXIT_FAILURE, "Can't stat `%s'", infile);
		};
	if (outfile && 0 == 1 /* XXX disable */) {
		if (stat(outfile, &buf) < 0)
			return;	/* file does not exist */
		else {
			errx(EXIT_FAILURE,
			    "`%s' already exists and would be overwritten",
			    outfile);
		}
	}
}
/*
 * Parse command line arguments
 */
static int
parseargs(int argc, char *argv[], struct commandline *cmd)
{
	int     i;
	int     j;
	int     c;
	char    flag[1 << CHAR_BIT];
	int     nflags;

	cmdname = argv[0];
	cmd->infile = cmd->outfile = NULL;
	if (argc < 2) {
		return (0);
	}
	flag['c'] = 0;
	flag['h'] = 0;
	flag['o'] = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			if (cmd->infile) {
				f_print(stderr, "Cannot specify more than one input file!\n");

				return (0);
			}
			cmd->infile = argv[i];
		} else {
			for (j = 1; argv[i][j] != 0; j++) {
				c = argv[i][j];
				switch (c) {
				case 'B':
					BSDflag = 1;
					break;
				case 'c':
				case 'h':
				case 't':
					if (flag[c]) {
						return (0);
					}
					flag[c] = 1;
					break;
				case 'o':
					if (argv[i][j - 1] != '-' ||
					    argv[i][j + 1] != 0) {
						return (0);
					}
					flag[c] = 1;
					if (++i == argc) {
						return (0);
					}
					if (cmd->outfile) {
						return (0);
					}
					cmd->outfile = argv[i];
					goto nextarg;
				case 'D':
					if (argv[i][j - 1] != '-') {
						return (0);
					}
					(void) addarg(argv[i]);
					goto nextarg;
				case 'Y':
					if (++i == argc) {
						return (0);
					}
					(void) strncpy(pathbuf, argv[i],
						sizeof(pathbuf)-1);
					(void) strncat(pathbuf, "/cpp",
						sizeof(pathbuf)-1);
					CPP = pathbuf;
					goto nextarg;

				case 'v':
					printf("version 1.0\n");
					exit(0);

				default:
					return (0);
				}
			}
	nextarg:
			;
		}
	}

	cmd->cflag = flag['c'];
	cmd->hflag = flag['h'];

	/* check no conflicts with file generation flags */
	nflags = cmd->cflag + cmd->hflag;

	if (nflags == 0) {
		if (cmd->outfile != NULL || cmd->infile == NULL) {
			return (0);
		}
	} else
		if (nflags > 1) {
			f_print(stderr, "Cannot have more than one file generation flag!\n");
			return (0);
		}
	return (1);
}

static void
usage(void)
{
	f_print(stderr, "usage:\n");
	f_print(stderr, "\t%s [flags] infile\n", cmdname);
	f_print(stderr, "\t%s [flags] [-c | -h] [-o outfile] [input]\n",
		cmdname);
	f_print(stderr, "\n");
	f_print(stderr, "First version generates all file from input file.\n");
	f_print(stderr, "Second version generates specified type of file\n");
	f_print(stderr, "from input (default input=stdin, output=stdout)\n\n");
	f_print(stderr, "Available output formats:\n");
	f_print(stderr, "-c\t\tgenerate code/decode proc routines\n");
	f_print(stderr, "-h\t\tgenerate header file\n");
	f_print(stderr, "\nflags:\n");
	f_print(stderr, "-B\t\tuse BSD c++ guard macros in header files\n");
	f_print(stderr, "-Dname[=value]\tdefine a symbol (same as #define)\n");
	f_print(stderr, "-v\t\tdisplay hg_rpcgen version number\n");
	f_print(stderr, "-Y path\t\tdirectory name to find C preprocessor (cpp)\n");
	f_print(stderr, "\n");
	f_print(stderr, "$HG_RPCGEN_CPP directly sets path to C preprocessor\n");
	exit(1);
}
