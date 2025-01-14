/*
 * test_hg_rpcgen.c  test hg_rpcgen/hg_proc integration
 * 29-Nov-2024  chuck@ece.cmu.edu
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "mercury_unit.h"
#include "mercury_proc_extra.h"
#include "test_hg_rpcgen.h"

/* proc ops: HG_ENCODE, HG_DECODE, HG_FREE */

/* encode/decode a uint32_t directly, w/o hg_rpcgen code */
static void
test0(hg_proc_t prc)
{
    hg_return_t ret;
    char buf[BUFSIZ];
    uint32_t in0, in, out;

    HG_TEST("u32 direct");

    in0 = 0xcafec001;
    in = in0;
    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    ret = hg_proc_uint32_t(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc encode");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    in = out = 0;
    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_uint32_t(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc decode");

    HG_TEST_CHECK_ERROR(out != in0, done, ret, HG_OTHER_ERROR, "data mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_uint32_t(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc free");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

/* encode/decode a fixed buffer directly, w/o hgrpcgen code */
static void
test1(hg_proc_t prc)
{
    hg_return_t ret;
    char buf[BUFSIZ];
    char in0[64], in[64], out[64];
#define TEST1_MSG "0xcafec001 is on!"

    HG_TEST("fixed buf direct");

    snprintf(in0, sizeof(in0), TEST1_MSG);
    snprintf(in, sizeof(in), "%s", in0);

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    ret = hg_proc_bytes(prc, in, sizeof(TEST1_MSG));
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc encode");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));

    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_bytes(prc, out, sizeof(TEST1_MSG));
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc decode");

    HG_TEST_CHECK_ERROR(strcmp(in0, out) != 0, done, ret,
        HG_OTHER_ERROR, "data mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_bytes(prc, out, sizeof(TEST1_MSG));
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc free");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

/* encode/decode a 'rgbcount' struct using generated output */
static void
test2(hg_proc_t prc)
{
    hg_return_t ret;
    char buf[BUFSIZ];
    rgbcount in0, in, out;

    HG_TEST("hg_rpcgen struct");

    in0.rval = GREEN;
    in0.pcount = 54321;
    in0.valid = HG_TRUE;
    in = in0;   /* struct copy */

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    ret = hg_proc_rgbcount(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc encode");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_rgbcount(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc decode");

    HG_TEST_CHECK_ERROR(out.rval != in0.rval || out.pcount != in0.pcount ||
        out.valid != HG_TRUE, done, ret, HG_OTHER_ERROR, "data mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_rgbcount(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc free");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

/* encode/decode opaque items */
static void
test3(hg_proc_t prc)
{
    hg_return_t ret;
    char buf[BUFSIZ], scratch[BUFSIZ];
    otype1 o1;
    otype2 o2;
    otype3 o3;
    optest in0, in, out;

    HG_TEST("hg_rpcgen opaque");

    /* first test encode (not bothering about checking the data)  */
    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    ret = hg_proc_otype1(prc, &o1);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode otype1");

    o2.otype2_len = 10;
    o2.otype2_val = scratch;
    ret = hg_proc_otype2(prc, &o2);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode otype2");

    o3.otype3_len = 100;
    o3.otype3_val = scratch;
    ret = hg_proc_otype3(prc, &o3);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode otype3");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    /* try again, but overflow max len */
    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE2)");

    o2.otype2_len = 21;   /* over maxsize limit */
    o2.otype2_val = scratch;
    ret = hg_proc_otype2(prc, &o2);
    HG_TEST_CHECK_ERROR(ret != HG_OVERFLOW, done, ret, HG_OTHER_ERROR,
        "failed overflow check");

    /* now switch to optest structure - make up some test data */
    snprintf(in0.ofield1, sizeof(in0.ofield1), "%s", "hi!");
    snprintf(in0.ofield4, sizeof(in0.ofield4), "%s", "there");
    in0.ofield2.ofield2_len = 0;
    in0.ofield2.ofield2_val = NULL;
    in0.ofield5.otype2_len = 0;
    in0.ofield5.otype2_val = NULL;
    in0.ofield3.ofield3_len = sizeof(TEST1_MSG);
    in0.ofield3.ofield3_val = TEST1_MSG;
#define TEST3_MSG "more testing"
    in0.ofield6.otype3_len = sizeof(TEST3_MSG);
    in0.ofield6.otype3_val = TEST3_MSG;
    in = in0;   /* struct copy */

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE3)");

    ret = hg_proc_optest(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode optest");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush2");

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_optest(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode optest");

    HG_TEST_CHECK_ERROR(strcmp(in0.ofield1, out.ofield1) ||
        strcmp(in0.ofield4, out.ofield4) || out.ofield2.ofield2_len ||
        out.ofield2.ofield2_val || out.ofield5.otype2_len ||
        out.ofield5.otype2_val ||
        out.ofield3.ofield3_len != sizeof(TEST1_MSG) ||
        strcmp(out.ofield3.ofield3_val, TEST1_MSG) ||
        out.ofield6.otype3_len != sizeof(TEST3_MSG) ||
        strcmp(out.ofield6.otype3_val, TEST3_MSG),
        done, ret, HG_OTHER_ERROR, "data mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_optest(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "free optest");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

/* encode/decode strings */
void test4(hg_proc_t prc) {
    hg_return_t ret;
    char buf[BUFSIZ];
    stype1 s1;
    stype2 s2;
    stringtest in0, in, out;

    HG_TEST("hg_rpcgen string");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    s1 = TEST1_MSG;
    ret = hg_proc_stype1(prc, &s1);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode stype1");

    s2 = "more";
    ret = hg_proc_stype2(prc, &s2);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode stype2");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE2)");

    s2 = TEST1_MSG;    /* over limit */
    ret = hg_proc_stype2(prc, &s2);
    HG_TEST_CHECK_ERROR(ret != HG_OVERFLOW, done, ret, HG_OTHER_ERROR,
        "failed overflow check");

    /* fields cannot be null (or we'll crash in strlen()) */
    in0.field1 = "first string";
    in0.field2 = "short";
    in0.field3 = "f3";
    in0.field4 = "closing soon!";
    in = in0;   /* struct copy */

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE3)");

    ret = hg_proc_stringtest(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode stringtest");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_stringtest(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode stringtest");

    HG_TEST_CHECK_ERROR(strcmp(out.field1, in0.field1) ||
        strcmp(out.field2, in0.field2) || strcmp(out.field3, in0.field3) ||
        strcmp(out.field4, in0.field4), done, ret,
        HG_OTHER_ERROR, "data mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_stringtest(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

/* encode/decode various data types */
static void
test5(hg_proc_t prc)
{
    hg_return_t ret;
    char buf[BUFSIZ];
    size_t lcv;
    uint32_t scratch[BUFSIZ];
    uint32_t tmpval;
    typedstr in0, in, out;

    HG_TEST("hg_rpcgen types");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    for (lcv = 0 ; lcv < sizeof(in0.f1)/sizeof(in0.f1[0]) ; lcv++) {
        in0.f1[lcv] = lcv+10;
        scratch[lcv] = lcv+100;
    }
    in0.f2.type2_len = sizeof(in0.f1)/sizeof(in0.f1[0]);
    in0.f2.type2_val = scratch;
    in0.f3.type3_len = 0;
    in0.f3.type3_val = NULL;
    in0.f4 = 4000;
    tmpval = 5000;
    in0.f5 = &tmpval;
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    in = in0;  /* struct copy */

    ret = hg_proc_typedstr(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode typedstr");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_typedstr(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode typedstr");

    for (lcv = 0 ; lcv  < sizeof(in0.f1)/sizeof(in0.f1[0]) ; lcv++) {
        HG_TEST_CHECK_ERROR(in0.f1[lcv] != out.f1[lcv], done, ret,
            HG_OTHER_ERROR, "f1 data mismatch");
    }
    HG_TEST_CHECK_ERROR(in0.f2.type2_len != out.f2.type2_len, done, ret,
        HG_OTHER_ERROR, "f2 data len mismatch");
    for (lcv = 0 ; lcv < out.f2.type2_len ; lcv++) {
        HG_TEST_CHECK_ERROR(in0.f2.type2_val[lcv] != out.f2.type2_val[lcv],
            done, ret, HG_OTHER_ERROR, "f2 data mismatch");
    }
    HG_TEST_CHECK_ERROR(out.f3.type3_len || out.f3.type3_val ||
        out.f4 != in0.f4, done, ret, HG_OTHER_ERROR, "f3 mismatch");
    HG_TEST_CHECK_ERROR(*in0.f5 != *out.f5 || in.f5 == out.f5,
        done, ret, HG_OTHER_ERROR, "f3 mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_typedstr(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "free typedstr");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

/* encode/decode various data types */
static void
test6(hg_proc_t prc)
{
    hg_return_t ret;
    char buf[BUFSIZ];
    utest in, out0, out1, out2, out3, out4;

    HG_TEST("hg_rpcgen struct+union");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    in.type = 0;
    in.utest_u.b.base_id = 10;
    snprintf(in.utest_u.b.base_name, sizeof(in.utest_u.b.base_name), "type0");

    ret = hg_proc_utest(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode utest t0");

    in.type = 1;
    in.utest_u.c.baseinfo.base_id = 11;
    snprintf(in.utest_u.c.baseinfo.base_name,
             sizeof(in.utest_u.c.baseinfo.base_name), "type1");
    in.utest_u.c.counter = 0xcafec001;

    ret = hg_proc_utest(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode utest t1");

    in.type = 2;
    ret = hg_proc_utest(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode utest t2");

    in.type = 3;
    ret = hg_proc_utest(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode utest t3");

    in.type = 4;
    in.utest_u.data = 0xfeedcafe;

    ret = hg_proc_utest(prc, &in);
    HG_TEST_CHECK_HG_ERROR(done, ret, "encode utest t4");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    memset(&in, 0, sizeof(in));
    memset(&out0, 0, sizeof(out0));
    memset(&out1, 0, sizeof(out1));
    memset(&out2, 0, sizeof(out2));
    memset(&out3, 0, sizeof(out3));
    memset(&out4, 0, sizeof(out4));

    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_utest(prc, &out0);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode utest t0");

    HG_TEST_CHECK_ERROR(out0.type != 0 || out0.utest_u.b.base_id != 10 ||
        strcmp("type0", out0.utest_u.b.base_name), done, ret,
        HG_OTHER_ERROR, "out0 mismatch");

    ret = hg_proc_utest(prc, &out1);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode utest t1");

    HG_TEST_CHECK_ERROR(out1.type != 1 ||
        out1.utest_u.c.baseinfo.base_id != 11 ||
        strcmp(out1.utest_u.c.baseinfo.base_name, "type1") ||
        out1.utest_u.c.counter != 0xcafec001,
        done, ret, HG_OTHER_ERROR, "out1 mismatch");

    ret = hg_proc_utest(prc, &out2);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode utest t2");

    HG_TEST_CHECK_ERROR(out2.type != 2, done, ret, HG_OTHER_ERROR,
        "out2 mismatch");

    ret = hg_proc_utest(prc, &out3);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode utest t3");

    HG_TEST_CHECK_ERROR(out3.type != 3, done, ret, HG_OTHER_ERROR,
        "out3 mismatch");

    ret = hg_proc_utest(prc, &out4);
    HG_TEST_CHECK_HG_ERROR(done, ret, "decode utest t4");

    HG_TEST_CHECK_ERROR(out4.type != 4 || out4.utest_u.data != 0xfeedcafe,
        done, ret, HG_OTHER_ERROR, "out4 mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_utest(prc, &out0);
    HG_TEST_CHECK_HG_ERROR(done, ret, "free utest t0");
    ret = hg_proc_utest(prc, &out1);
    HG_TEST_CHECK_HG_ERROR(done, ret, "free utest t1");
    ret = hg_proc_utest(prc, &out2);
    HG_TEST_CHECK_HG_ERROR(done, ret, "free utest t2");
    ret = hg_proc_utest(prc, &out3);
    HG_TEST_CHECK_HG_ERROR(done, ret, "free utest t3");
    ret = hg_proc_utest(prc, &out4);
    HG_TEST_CHECK_HG_ERROR(done, ret, "free utest t4");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

/* encode/decode linked list */
void test7(hg_proc_t prc) {
    hg_return_t ret;
    char buf[BUFSIZ];
    i32list il[4], out, *p;
    int lcv;

    HG_TEST("hg_rpcgen linked list");

    for (lcv = 0 ; lcv < 4 ; lcv++) {
        il[lcv].ival = lcv+1;
        if (lcv < 3)
            il[lcv].next = &il[lcv+1];
    }
    il[3].next = NULL;
    out.ival = 0;
    out.next = NULL;

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_ENCODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (ENCODE)");

    ret = hg_proc_i32list(prc, il);
    HG_TEST_CHECK_HG_ERROR(done, ret, "i32list encode");

    ret = hg_proc_flush(prc);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc flush");

    ret = hg_proc_reset(prc, buf, hg_proc_get_size_used(prc), HG_DECODE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (DECODE)");

    ret = hg_proc_i32list(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "i32list decode");

    for (lcv = 0, p = &out ; lcv < 4 ; lcv++, p = p->next) {
        HG_TEST_CHECK_ERROR(il[lcv].ival != p->ival, done, ret,
            HG_OTHER_ERROR, "data mismatch");
    }
    HG_TEST_CHECK_ERROR(p != NULL, done, ret, HG_OTHER_ERROR, "null mismatch");

    ret = hg_proc_reset(prc, buf, sizeof(buf), HG_FREE);
    HG_TEST_CHECK_HG_ERROR(done, ret, "proc_reset (FREE)");

    ret = hg_proc_i32list(prc, &out);
    HG_TEST_CHECK_HG_ERROR(done, ret, "i32list free");

    HG_PASSED();

done:
    if (ret != HG_SUCCESS)
        HG_FAILED();

    return;
}

int
main(void)
{
    hg_return_t hg_ret;
    int ret = EXIT_SUCCESS;
    hg_class_t *hgclass;
    hg_proc_t prc;              /* a pointer */

    /*
     * XXX:
     * hg_proc_create() will fail without a non-NULL class pointer
     * even though we do not need an hg_class_t for any of our tests.
     * create a fake non-NULL class pointer to work around this...
     */
    hgclass = (hg_class_t *) 1;   /* will crash test program if used */

    /* create proc test */
    HG_TEST("create proc");
    hg_ret = hg_proc_create(hgclass, HG_CRC32, &prc);
    HG_TEST_CHECK_ERROR(hg_ret != HG_SUCCESS, done, ret,
        EXIT_FAILURE, "create proc test failed");
    HG_PASSED();

    test0(prc);
    test1(prc);
    test2(prc);
    test3(prc);
    test4(prc);
    test5(prc);
    test6(prc);
    test7(prc);

    /* dispose of proc test */
    HG_TEST("dispose of proc");
    hg_ret = hg_proc_free(prc);
    HG_TEST_CHECK_ERROR(hg_ret != HG_SUCCESS, done, ret,
        EXIT_FAILURE, "dispose of proc test failed");
    HG_PASSED();

done:
    if (ret != EXIT_SUCCESS)
        HG_FAILED();

    return ret;
}
