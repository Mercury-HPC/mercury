/*
 * test_hg_rpcgen.x  document/test hg_rpcgen .x input files
 * 29-Nov-2024  chuck@ece.cmu.edu
 */

/*
 * this file gets run through the C pre-processor with -DHG_RPC_HDR
 * when generating *.h files and -DHG_RPC_PROC when generating *_proc.c
 * files.   we use the same parser as rpcgen, but ignore 'program'
 * statements.  lines that start with '%' are directly passed through
 * to the output stream without being parsed.
 */

/* there are 5 basic statement types: const, enum, typedef, struct, union */

const MAXSIZE = 512;   /* becomes a C #define in the .h file */

/*
 * an enum becomes a C enum definition (with a typedef) in the .h file.
 * we also generate a hg_proc_cb_t proc function suitable for registering
 * with mercury RPCs.  the prototype for the proc function is generated
 * in the .h file and the actual function is generated in the .c file.
 *
 * note that in C an enum can handle 'int' sized values, but the compiler
 * can choose a smaller type if the values fit.  in sunrpc enum_t is
 * typedef'd to "int" which maps to int32_t on modern platforms.
 *
 * XXX: hg_rpcgen currently directly generates enums as int32_t rather
 * than defining and using an 'enum_t' type.
 */
enum rgb {
    RED = 1,
    GREEN = 2,
    BLUE = 3
};

/*
 * typdefs are passed through to the .h file.  we also generate
 * a hg_proc_cb_t proc function for the new type we are defining.
 */
typedef uint32_t counter32;   /* becomes a C typedef (w/proc fn) */

/*
 * predefined mercury types: int8_t, uint8_t, int16_t, uint16_t,
 * int32_t, uint32_t, int64_t, uint64_t.
 *
 * vectors: a fixed length C array defined with the length in '[]'
 *   - 'int32_t stats[8]'
 *
 * arrays: a variable length array defined with the max length in '<>'
 *   - 'int32_t numlist<10>'     0 to 10 int32_t
 *   - 'uint32_t blocklist<>'    list of uint32_t with no size limit
 * note: internally arrays are stored as a length and a pointer to
 *       a dynamically allocated C array (as seen in the .h output
 *       of hg_rpcgen).
 *
 * special types:
 *   - 'bool' is converted to hg_bool_t (a uint8_t)
 *   - 'opaque' is a byte array (fixed vector or variable length array)
 *   - 'string' is a null terminated C string (variable length array only)
 *   - pointers are allowed (including pointers set to NULL, but not
 *     pointers to pointers or void* pointers)
 *
 * note: hg_rpcgen itself does not detect undefined types.  instead
 * it lets the compiler detect this.
 */

/*
 * structs are converted to C structures and hg_proc_cb_t proc functions.
 * C structure definitions are adjusted for variable length arrays.
 * structures can be nested.
 */
struct rgbcount {
    rgb rval;
    counter32 pcount;
    bool valid;        /* converted to hg_bool_t */
};

/*
 * unions have an int type and are converted to a C structure
 * with an embedded C union in it.   note the union syntax here
 * is different from C.  also note that only one variable decl
 * is allowed per case (if you need more, define a struct first
 * and then use it in the union).
 */
union name_number_or_nothing switch (uint32_t type) {
    case 0: opaque name[MAXSIZE];
    case 1: uint32_t number;
    case 2:            /* fall throughs are ok, goes to next case */
    case 3: void;      /* no additional fields */
};

/* sample typedefs */

/*
 * do multiple types of typedef and then use them in a structure.
 */
typedef uint32_t type1[10];  /* a vector (REL_VECTOR) */
typedef uint32_t type2<20>;  /* a var length array (REL_ARRAY) */
typedef uint32_t type3<>;    /* var length w/o max */
typedef uint32_t type4;      /* an alias (REL_ALIAS) */
typedef uint32_t *type5;     /* a pointer (REL_POINTER) */

struct typedstr {
    type1 f1;
    type2 f2;
    type3 f3;
    type4 f4;
    type5 f5;
};

/*
 * typedef in a struct with a pointer
 */
typedef struct typedstr *tspointer;

struct tdlist {
    typedstr tdata;
    tspointer *next;
};

/*
 * we track structs we've defined and use their typedef type
 * rather than 'struct type' ...
 */
struct known {      /* will create 'known' typedef for this struct */
    uint32_t f6;
};

#ifdef HG_RPC_PROC
%/* make dummy function for unknown outside of hg_rpcgen so test compiles */
%hg_return_t hg_proc_unknown(hg_proc_t p, void *v) { return HG_SUCCESS; }
#endif /* HG_RPC_PROC */

typedef struct known type6;   /* emit: 'typedef known type6' */
typedef known type7;          /* emit: 'typedef known type7' */
typedef struct unknown type8; /* emit: 'typedef struct unknown type8' */

/* sample opaque */

/*
 * 'opaque' must be an array (fixed [] or variable <>).
 * internally it becomes a char* with a length.  opaque
 * can be in typedefs, structs, or unions.  unions share
 * the same code generator as structs, so we just test
 * typedef and struct here.
 */

typedef opaque otype1[10];  /* fixed size, 10 bytes */
typedef opaque otype2<20>;  /* variable size, up to 20 bytes */
typedef opaque otype3<>;    /* variable size, no max */

struct optest {
    opaque ofield1[30];     /* fixed size in a struct */
    opaque ofield2<40>;     /* variable sized in a struct */
    opaque ofield3<>;       /* variable sized in a struct (no max) */
    /* define some fields using the typedefs */
    otype1 ofield4;
    otype2 ofield5;
    otype3 ofield6;
#if 0
    /*
     * opaque cannot be pointers or aliases.  the parser
     * will reject the two opaque lines below...
     */
    opaque *broke1;        /* NO! */
    opaque broke2;         /* NO! */
#endif
};

/* sample strings */

/*
 * strings: null terminated C strings.   we can determine string length
 * using strlen().   strings must be defined as variable length arrays
 * ("<>") and are converted to 'char *' in our output.  the underlying
 * char* for a string cannot be NULL.
 */

/*
 * typedef strings
 */
typedef string stype1<>;   /* typedef char *stype1 */
typedef string stype2<10>; /* typedef char *stype2 */

/*
 * strings inside of structs using typedefs or directly defined
 */
struct stringtest {
    stype1 field1;         /* use above typedef */
    stype2 field2;         /* use above typedef */
    string field3<>;       /* string w/o max length */
    string field4<20>;     /* string with max length */
#if 0
    /*
     * strings cannot be pointers, vectors, or aliases.  the parser
     * will reject the three string lines below...
     */
    string *broke1;        /* NO! */
    string broke2[30];     /* NO! */
    string broke3;         /* NO! */
#endif
};

/* sample struct/union */

/*
 * structures have 4 types of fields: alias, vector, pointer, array.
 * vectors are fixed sized C arrays, arrays are variable size.
 */
struct fourtypes {
    uint32_t alias_type;
    uint32_t vec_type[20];       /* vectors are fixed size */
    uint32_t *ptr_type;
    uint32_t array_type<>;       /* array becomes _len, _val structure */
};

/*
 * structs can nest
 */
struct base {
    uint32_t base_id;
    opaque base_name[16];
};

/* nest 'base' inside of counter */
struct counter {
    base baseinfo;
    uint32_t counter;
};

/* you can nest a dynamically allocated base structure with a pointer */
struct counterp {
    base *baseinfop;    /* points to a base or NULL */
    uint32_t counter;
};

/* you can make vectors/arrays of structures */
struct bases {
    base basevector[10];   /* fixed length vector */
    base basearray<>;      /* variable length */
};

/*
 * structures can have pointers to themselves (e.g. for a simple linked
 * list or basic tree).   (more complicated structures may require writing
 * helper functions directly in C.)
 */
struct i32list {
    uint32_t ival;
    i32list *next;
};

/*
 * unions have a switch with an int type and cases.  each case
 * is allowed to have a single type, fall through, or have an
 * empty structure (denoted by 'void').
 */
union utest switch (uint8_t type) {
    case 0: base b;
    case 1: counter c;
    case 2:                  /* fall through */
    case 3: void;            /* no data */
    default: uint32_t data;  /* if type does not match a case */
};

#if 0
/*
 * this will fail, as type is not an int.  the type has to be
 * something that works with a C switch statement.
union utest2 switch (float type) {    /* NO! */
    case 0: void;
}
#endif

/* code generation cases */

/*
 * here are all the code generation cases in a single structure.
 */
struct codgencases {
    /* POINTER (any type) */
    int64_t *p1;     /* pointer case: becomes hg_proc_pointer() call */

    /* VECTOR (fixed length C array) */
#if 0
    string vr1[11];  /* vector case 1: C string -- parser does not allow! */
#endif
    opaque vr2[12];  /* vector case 2: unstructed opaque blob, fixed length */
                     /*    uses hg_proc_bytes() with given size */
    int64_t vr3[9];  /* vector case 3: neither string nor opaque */
                     /*    uses hg_proc_vector() on this */

    /* ARRAY (variable length) */
    string ar1<20>;  /* array case 1: C string, becomes char* */
                     /*     use hg_proc_string() for coding (uses strlen) */
    opaque ar2<30>;  /* array case 2: unstructured blob, becomes (char*,len) */
                     /*     use hg_proc_varbytes() to store len, char*data */
    int64_t ar3<40>; /* array case 3: neither string nor opaque */
                     /*     uses hg_proc_array() and proc for element type */

    /* ALIAS */
    bool av1;       /* alias case 1: bool - gets converted to hg_bool_t */
    int64_t av2;    /* alias case 2: !bool */
};
