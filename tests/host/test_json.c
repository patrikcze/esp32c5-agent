#include "lla_json.h"
#include "test.h"

static int parse(const char *s, lla_jtok_t *t, int n) { return lla_json_parse(s, strlen(s), t, (uint16_t)n); }

static void test_parse_basic(void)
{
    lla_jtok_t t[32];
    const char *js = "{\"a\":1,\"b\":[true,false,null],\"c\":{\"d\":\"x\\ny\"},\"e\":-1.5e3}";
    int n = parse(js, t, 32);
    CHECK(n > 0);
    CHECK_EQ_INT(t[0].type, LLA_JT_OBJECT);
    CHECK_EQ_INT(t[0].size, 4);
    CHECK_EQ_INT(t[0].skip, n);
    int b = lla_json_obj_get(js, t, 0, "b");
    CHECK_EQ_INT(t[b].type, LLA_JT_ARRAY);
    CHECK_EQ_INT(t[lla_json_arr_get(t, b, 2)].type, LLA_JT_NULL);
    int c = lla_json_obj_get(js, t, 0, "c");
    int d = lla_json_obj_get(js, t, c, "d");
    char buf[16];
    CHECK_EQ_INT(lla_json_str(js, &t[d], buf, sizeof(buf)), 3);
    CHECK_STR(buf, "x\ny");
    double e;
    CHECK(lla_json_num(js, &t[lla_json_obj_get(js, t, 0, "e")], &e) && e == -1500.0);
    int64_t i;
    CHECK(lla_json_int(js, &t[lla_json_obj_get(js, t, 0, "a")], &i) && i == 1);
    CHECK(!lla_json_int(js, &t[lla_json_obj_get(js, t, 0, "e")], &i) || i == -1500);
    CHECK_EQ_INT(lla_json_obj_get(js, t, 0, "zz"), -1);
}

static void test_parse_errors(void)
{
    lla_jtok_t t[16];
    CHECK_EQ_INT(parse("{\"a\":", t, 16), LLA_JSON_ERR_PART);
    CHECK_EQ_INT(parse("{\"a\":1", t, 16), LLA_JSON_ERR_PART);
    CHECK_EQ_INT(parse("[tru", t, 16), LLA_JSON_ERR_PART);
    CHECK_EQ_INT(parse("{\"a\" 1}", t, 16), LLA_JSON_ERR_INVAL);
    CHECK_EQ_INT(parse("{a:1}", t, 16), LLA_JSON_ERR_INVAL);
    CHECK_EQ_INT(parse("[1,]", t, 16), LLA_JSON_ERR_INVAL);
    CHECK_EQ_INT(parse("01", t, 16), LLA_JSON_ERR_INVAL);
    CHECK_EQ_INT(parse("{} x", t, 16), LLA_JSON_ERR_INVAL);
    CHECK_EQ_INT(parse("\"\x01\"", t, 16), LLA_JSON_ERR_INVAL);
    CHECK_EQ_INT(parse("\"\\q\"", t, 16), LLA_JSON_ERR_INVAL);
    CHECK_EQ_INT(parse("[1,2,3,4]", t, 3), LLA_JSON_ERR_NOMEM);
    char deep[64];
    memset(deep, '[', 40);
    deep[40] = '\0';
    CHECK_EQ_INT(parse(deep, t, 16), LLA_JSON_ERR_NOMEM);
    lla_jtok_t big[64];
    CHECK_EQ_INT(lla_json_parse(deep, strlen(deep), big, 64), LLA_JSON_ERR_DEPTH);
}

static void test_unicode(void)
{
    lla_jtok_t t[4];
    const char *js = "\"\\u00e9\\ud83d\\ude00\\u0000\"";
    CHECK(parse(js, t, 4) == 1);
    char out[16];
    int n = lla_json_str(js, &t[0], out, sizeof(out));
    CHECK_EQ_INT(n, 2 + 4 + 3);
    CHECK(memcmp(out, "\xc3\xa9\xf0\x9f\x98\x80\xef\xbf\xbd", 9) == 0);
    char tiny[3];
    CHECK_EQ_INT(lla_json_str(js, &t[0], tiny, sizeof(tiny)), -1);
}

static void test_writer(void)
{
    char buf[128];
    lla_jw_t w;
    lla_jw_init(&w, buf, sizeof(buf));
    lla_jw_obj_begin(&w);
    lla_jw_kv_str(&w, "s", "a\"b\\c\n\x01");
    lla_jw_kv_int(&w, "i", -42);
    lla_jw_key(&w, "arr");
    lla_jw_arr_begin(&w);
    lla_jw_bool(&w, true);
    lla_jw_null(&w);
    lla_jw_num(&w, 1.5);
    lla_jw_arr_end(&w);
    lla_jw_obj_end(&w);
    CHECK(lla_jw_ok(&w));
    CHECK_STR(buf, "{\"s\":\"a\\\"b\\\\c\\n\\u0001\",\"i\":-42,\"arr\":[true,null,1.5]}");
    lla_jtok_t t[16];
    CHECK(parse(buf, t, 16) > 0);

    char small[8];
    lla_jw_init(&w, small, sizeof(small));
    lla_jw_obj_begin(&w);
    lla_jw_kv_str(&w, "key", "value");
    lla_jw_obj_end(&w);
    CHECK(!lla_jw_ok(&w));
    CHECK(strlen(small) < sizeof(small));
}

void suite_json(void)
{
    RUN(test_parse_basic);
    RUN(test_parse_errors);
    RUN(test_unicode);
    RUN(test_writer);
}
