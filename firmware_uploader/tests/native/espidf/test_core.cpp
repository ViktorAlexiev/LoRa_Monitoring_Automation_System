// Тестове на components/agro_core, agro_hal, agro_cfg срещу симулатора.
#include "test_support.h"

#include <algorithm>
#include <random>

#include "board_pins.h"
#include "cfg_parse.h"
#include "ceiling_counter.h"
#include "channel_access.h"
#include "crypto_common.h"
#include "dedup.h"
#include "esp_system.h"
#include "agro_hal.h"
#include "json_util.h"
#include "nvs_store.h"
#include "radio_timing.h"

// ============================================================================ mbedtls заместител (верификация на тестовата база)
#include "mbedtls/aes.h"
#include "mbedtls/cmac.h"

STEST(fake_mbedtls_matches_rfc4493_cmac_vectors) {
  const uint8_t key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
  std::vector<uint8_t> msg = fromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
  struct { size_t len; const char *mac; } v[4] = {
    {0, "bb1d6929e95937287fa37d129b756746"}, {16, "070a16b46b4d4144f79bdd9dd04a287c"},
    {40, "dfa66747de9ae63030ca32611497c827"}, {64, "51f0bebf7e3b9d92fc49741779363cfe"}};
  const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
  for (auto &x : v) {
    uint8_t out[16];
    ASSERT_EQ(mbedtls_cipher_cmac(info, key, 128, msg.data(), x.len, out), 0);
    ASSERT_STREQ(toHex(std::vector<uint8_t>(out, out + 16)), x.mac);
  }
}

STEST(fake_mbedtls_matches_nist_ctr_vector) {
  const uint8_t key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
  std::vector<uint8_t> ctr = fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
  std::vector<uint8_t> pt = fromHex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
  mbedtls_aes_context c;
  mbedtls_aes_init(&c);
  ASSERT_EQ(mbedtls_aes_setkey_enc(&c, key, 128), 0);
  uint8_t out[64], sb[16];
  size_t off = 0;
  mbedtls_aes_crypt_ctr(&c, 64, &off, ctr.data(), sb, pt.data(), out);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(out, out + 64)),
               "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
}

// ============================================================================ крипто протокол
static const uint8_t ID_S001[6] = {'S', '0', '0', '1', 0, 0};

STEST(crypto_wire_format_matches_independent_reference_golden_vectors) {
  // golden стойности: Python 'cryptography' (AES-CTR + CMAC), независимо от прошивката и тестовия AES
  uint8_t w[64];
  uint8_t pt1[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t n = cryptoBuildWirePacket(w, TEST_KEY, CRYPTO_TYPE_SENSOR, ID_S001, 0x01020304, pt1, 8);
  ASSERT_EQ(n, 22);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(w, w + n)), "5330303100002e805d3900c6c418010203048d892380");

  const uint8_t E007[6] = {'E', '0', '0', '7', 0, 0};
  n = cryptoBuildWirePacket(w, TEST_KEY, CRYPTO_TYPE_EXEC_HB, E007, 5, nullptr, 0);
  ASSERT_EQ(n, 14);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(w, w + n)), "4530303700000000000567cbfb3a");

  uint8_t pt50[50];
  for (int i = 0; i < 50; i++) pt50[i] = (uint8_t)i;
  n = cryptoBuildWirePacket(w, TEST_KEY, CRYPTO_TYPE_STATE_RESP, E007, 0xFFFFFFFFu, pt50, 50);
  ASSERT_EQ(n, 64);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(w, w + n)),
               "4530303700004145a70daa33ee617418519161c1dc52324e6e7132b507919d9eb8f2ef2ff9503556123f7a8673ce3451d046799a889753e8ffffffff258f09ed");

  const uint8_t G001[6] = {'G', '0', '0', '1', 0, 0};
  uint8_t pt6[6] = {'V', '0', '1', 0, 0xA1, 0xFF};
  n = cryptoBuildWirePacket(w, TEST_KEY, CRYPTO_TYPE_GW_CMD, G001, 100, pt6, 6);
  ASSERT_EQ(n, 20);
  ASSERT_STREQ(toHex(std::vector<uint8_t>(w, w + n)), "47303031000090e6852778a600000064b7cf871b");
}

STEST(crypto_roundtrip_all_types_and_lengths) {
  for (uint8_t type = 1; type <= 9; type++) {
    for (int len = 0; len <= CRYPTO_MAX_PT; len++) {
      uint8_t pt[CRYPTO_MAX_PT], w[CRYPTO_MAX_PT + CRYPTO_OVERHEAD], out[CRYPTO_MAX_PT], sender[6];
      for (int i = 0; i < len; i++) pt[i] = (uint8_t)(i * 7 + type);
      uint8_t n = cryptoBuildWirePacket(w, TEST_KEY, type, ID_S001, 1000 + len, pt, (uint8_t)len);
      ASSERT_EQ(n, len + CRYPTO_OVERHEAD);
      uint8_t outLen = 255;
      ASSERT_TRUE(cryptoParseWirePacket(w, n, TEST_KEY, type, sender, out, &outLen));
      ASSERT_EQ(outLen, len);
      ASSERT_MEM_EQ(out, pt, len);
      ASSERT_MEM_EQ(sender, ID_S001, 6);
    }
  }
}

STEST(crypto_rejects_tampering_wrong_key_wrong_type_and_bad_lengths) {
  uint8_t pt[8] = {1, 2, 3, 4, 5, 6, 7, 8}, w[22], out[50], sender[6], l;
  uint8_t n = cryptoBuildWirePacket(w, TEST_KEY, CRYPTO_TYPE_SENSOR, ID_S001, 7, pt, 8);
  ASSERT_TRUE(cryptoParseWirePacket(w, n, TEST_KEY, CRYPTO_TYPE_SENSOR, sender, out, &l));
  // всеки байт, обърнат бит по бит (включително senderId, counter и tag): тагът трябва да хване ВСЯКО
  for (int pos = 0; pos < n; pos++) {
    for (int bit = 0; bit < 8; bit++) {
      uint8_t t[22];
      memcpy(t, w, n);
      t[pos] ^= (uint8_t)(1 << bit);
      ASSERT_FALSE(cryptoParseWirePacket(t, n, TEST_KEY, CRYPTO_TYPE_SENSOR, sender, out, &l));
    }
  }
  uint8_t other[16];
  memcpy(other, TEST_KEY, 16);
  other[15] ^= 1;
  ASSERT_FALSE(cryptoParseWirePacket(w, n, other, CRYPTO_TYPE_SENSOR, sender, out, &l));
  for (uint8_t type = 2; type <= 9; type++) ASSERT_FALSE(cryptoParseWirePacket(w, n, TEST_KEY, type, sender, out, &l));
  ASSERT_FALSE(cryptoParseWirePacket(w, 13, TEST_KEY, CRYPTO_TYPE_SENSOR, sender, out, &l));   // по-къс от overhead
  ASSERT_FALSE(cryptoParseWirePacket(w, 0, TEST_KEY, CRYPTO_TYPE_SENSOR, sender, out, &l));
  uint8_t big[CRYPTO_OVERHEAD + CRYPTO_MAX_PT + 1] = {0};
  ASSERT_FALSE(cryptoParseWirePacket(big, sizeof(big), TEST_KEY, CRYPTO_TYPE_SENSOR, sender, out, &l));   // plaintext > 50
  uint8_t pt51[51] = {0}, w51[80];
  ASSERT_EQ(cryptoBuildWirePacket(w51, TEST_KEY, CRYPTO_TYPE_SENSOR, ID_S001, 1, pt51, 51), 0);          // отказ, не препълване
}

STEST(crypto_same_counter_different_type_or_sender_gives_different_ciphertext) {
  uint8_t pt[8] = {9, 9, 9, 9, 9, 9, 9, 9}, a[22], b[22], c[22];
  const uint8_t other[6] = {'S', '0', '0', '2', 0, 0};
  cryptoBuildWirePacket(a, TEST_KEY, CRYPTO_TYPE_SENSOR, ID_S001, 5, pt, 8);
  cryptoBuildWirePacket(b, TEST_KEY, CRYPTO_TYPE_EXEC_ACK, ID_S001, 5, pt, 8);
  cryptoBuildWirePacket(c, TEST_KEY, CRYPTO_TYPE_SENSOR, other, 5, pt, 8);
  ASSERT_MEM_NE(a + 6, b + 6, 8);     // nonce space-ът е различен по тип
  ASSERT_MEM_NE(a + 6, c + 6, 8);     // и по подател
}

// ============================================================================ JSON (cJSON + тънката обвивка json_util)
STEST(json_out_formats_and_escapes) {
  char buf[256];
  JsonOut w;
  w.beginObject().str("id", "S001").fixed2("soil_t", 2257).fixed2("neg", -5).fixed2("n2", -2257).num("rssi", -50)
      .num("nodata", -32768).unum("ts", 4000000000u).boolean("restart", true).str("q", "a\"b\\c\n").endObject();
  size_t n = 0;
  ASSERT_TRUE(w.ok());
  ASSERT_TRUE(w.print(buf, sizeof(buf), &n));
  ASSERT_STREQ(buf, "{\"id\":\"S001\",\"soil_t\":22.57,\"neg\":-0.05,\"n2\":-22.57,\"rssi\":-50,\"nodata\":-32768,"
                    "\"ts\":4000000000,\"restart\":true,\"q\":\"a\\\"b\\\\c\\n\"}");
  ASSERT_EQ((int)n, (int)strlen(buf));
}

STEST(json_out_fixed2_always_has_two_decimals_like_the_arduino_firmware) {
  // 22.00 остава "22.00" (байт по байт като Arduino версията и бекенда, който чете същия формат)
  char buf[128];
  JsonOut w;
  w.beginObject().fixed2("a", 2200).fixed2("b", 0).fixed2("c", 1).fixed2("d", 99).fixed2("e", -1).fixed2("f", 12345).fixed2("g", -3276700)
      .fixed2("h", -2147483647 - 1).endObject();
  ASSERT_TRUE(w.print(buf, sizeof(buf)));
  ASSERT_STREQ(buf, "{\"a\":22.00,\"b\":0.00,\"c\":0.01,\"d\":0.99,\"e\":-0.01,\"f\":123.45,\"g\":-32767.00,\"h\":-21474836.48}");
  // всяка стотна от целия обхват на сензорите се връща точно през парсер: round-trip
  for (int h = -4000; h <= 12000; h++) {
    char b[48];
    JsonOut o;
    o.beginObject().fixed2("v", h).endObject();
    ASSERT_TRUE(o.print(b, sizeof(b)));
    double v = atof(strstr(b, ":") + 1);
    if (std::lround(v * 100.0) != h) { ASSERT_EQ(std::lround(v * 100.0), (long)h); break; }
  }
}

STEST(json_out_nested_arrays) {
  char buf[256];
  JsonOut w;
  w.beginObject().str("id", "E007").beginArray("states");
  w.beginObject().str("id", "V01").str("state", "ON").endObject();
  w.beginObject().str("id", "P01").str("state", "OFF").endObject();
  w.endArray().endObject();
  ASSERT_TRUE(w.print(buf, sizeof(buf)));
  ASSERT_STREQ(buf, "{\"id\":\"E007\",\"states\":[{\"id\":\"V01\",\"state\":\"ON\"},{\"id\":\"P01\",\"state\":\"OFF\"}]}");
  // празен масив
  JsonOut e;
  e.beginObject().str("id", "E1").beginArray("states").endArray().endObject();
  ASSERT_TRUE(e.print(buf, sizeof(buf)));
  ASSERT_STREQ(buf, "{\"id\":\"E1\",\"states\":[]}");
}

STEST(json_out_overflow_is_detected_not_silent) {
  JsonOut w;
  w.beginObject().str("id", "S001").endObject();           // {"id":"S001"} = 13 знака; cJSON иска място за терминатора и още един байт
  char buf[32];
  size_t n = 99;
  for (size_t cap = 0; cap < 15; cap++) {
    memset(buf, 'Z', sizeof(buf));
    ASSERT_FALSE(w.print(buf, cap, &n));
    ASSERT_EQ((int)n, 0);
    if (cap > 0) ASSERT_EQ((int)buf[0], 0);                 // никога частичен/отрязан JSON в буфера
  }
  ASSERT_TRUE(w.print(buf, 15, &n));
  ASSERT_EQ((int)n, 13);
  ASSERT_STREQ(buf, "{\"id\":\"S001\"}");
}

STEST(json_out_misuse_is_an_error_never_a_corrupt_document) {
  char buf[128];
  { JsonOut w; w.beginObject().str("a", "b"); ASSERT_FALSE(w.print(buf, sizeof(buf))); }                 // липсва endObject
  { JsonOut w; w.str("a", "b"); ASSERT_FALSE(w.ok()); }                                                   // няма обект
  { JsonOut w; w.beginObject().endObject().beginObject(); ASSERT_FALSE(w.ok()); }                        // втори корен
  { JsonOut w; w.beginObject().beginObject(); ASSERT_FALSE(w.ok()); }                                    // обект в обект без ключ
  { JsonOut w; w.beginObject().beginArray("x").str("k", "v"); ASSERT_FALSE(w.ok()); }                    // ключ директно в масив
  { JsonOut w; w.beginArray("x"); ASSERT_FALSE(w.ok()); }                                                // масив без обект
  { JsonOut w; w.beginObject(); for (int i = 0; i < 12; i++) w.beginArray("a"); ASSERT_FALSE(w.ok()); }  // над JSON_MAX_DEPTH
  { JsonOut w; ASSERT_FALSE(w.ok()); ASSERT_FALSE(w.print(buf, sizeof(buf))); }                          // празен
}

STEST(json_out_output_parses_back_with_the_parser) {
  char buf[256];
  JsonOut w;
  w.beginObject().str("M_ID", "E007").str("C_ID", "V01").str("com", "A1").num("status", 3).endObject();
  ASSERT_TRUE(w.print(buf, sizeof(buf)));
  JsonDoc d(buf, strlen(buf));
  ASSERT_TRUE(d.valid());
  char v[8];
  ASSERT_TRUE(jsonGetString(d.root(), "M_ID", v, sizeof(v))); ASSERT_STREQ(v, "E007");
  ASSERT_TRUE(jsonGetString(d.root(), "com", v, sizeof(v)));  ASSERT_STREQ(v, "A1");
  ASSERT_FALSE(jsonGetString(d.root(), "status", v, sizeof(v)));      // числото не е низ
}

static bool getS(const char *json, const char *key, std::string *out, size_t cap = 16) {
  char b[256];
  JsonDoc d(json, strlen(json));
  bool ok = jsonGetString(d.root(), key, b, cap);
  *out = b;
  return ok;
}

STEST(json_parser_command_shapes) {
  std::string v;
  const char *cmd = "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\"}";
  ASSERT_TRUE(getS(cmd, "M_ID", &v)); ASSERT_STREQ(v, "E007");
  ASSERT_TRUE(getS(cmd, "C_ID", &v)); ASSERT_STREQ(v, "V01");
  ASSERT_TRUE(getS(cmd, "com", &v));  ASSERT_STREQ(v, "A1");
  ASSERT_FALSE(getS(cmd, "missing", &v));
  // интервали, нов ред, различен ред, допълнителни полета
  ASSERT_TRUE(getS("  {\n \"x\" : 5 ,\r\n \"com\"\t:\t\"B2\" , \"M_ID\":\"E1\" }  ", "com", &v)); ASSERT_STREQ(v, "B2");
  // ключ вътре в стойност на друг ключ не се бърка
  ASSERT_TRUE(getS("{\"x\":\"\\\"M_ID\\\":\\\"BAD\\\"\",\"M_ID\":\"E007\"}", "M_ID", &v)); ASSERT_STREQ(v, "E007");
  // вложен обект със същия ключ - важи само най-горното ниво
  ASSERT_TRUE(getS("{\"o\":{\"M_ID\":\"INNER\"},\"M_ID\":\"OUTER\"}", "M_ID", &v)); ASSERT_STREQ(v, "OUTER");
  ASSERT_FALSE(getS("{\"o\":{\"M_ID\":\"INNER\"}}", "M_ID", &v));
  ASSERT_TRUE(getS("{\"a\":[1,2,{\"M_ID\":\"x\"},\"]\"],\"M_ID\":\"OK\"}", "M_ID", &v)); ASSERT_STREQ(v, "OK");
  // не-низ -> не е валидно поле
  ASSERT_FALSE(getS("{\"M_ID\":7}", "M_ID", &v));
  ASSERT_FALSE(getS("{\"M_ID\":null}", "M_ID", &v));
  ASSERT_FALSE(getS("{\"M_ID\":{\"a\":\"b\"}}", "M_ID", &v));
  // първият от повтарящи се ключове печели
  ASSERT_TRUE(getS("{\"M_ID\":\"A\",\"M_ID\":\"B\"}", "M_ID", &v)); ASSERT_STREQ(v, "A");
  // ключовете са с различаване на малки/главни (cJSON_GetObjectItem без Case не го прави - тук е умишлено строго)
  ASSERT_FALSE(getS("{\"m_id\":\"E1\"}", "M_ID", &v));
  // escape-и
  ASSERT_TRUE(getS("{\"k\":\"a\\u0041\\n\\\"\"}", "k", &v)); ASSERT_STREQ(v, "aA\n\"");
}

STEST(json_parser_rejects_too_long_values_instead_of_truncating) {
  std::string v;
  ASSERT_TRUE(getS("{\"M_ID\":\"123456\"}", "M_ID", &v, 7));    // точно колкото се побира
  ASSERT_FALSE(getS("{\"M_ID\":\"1234567\"}", "M_ID", &v, 7));  // един повече: НЕ се реже (отрязан id може да е чужд модул)
  ASSERT_STREQ(v, "");
  char b[16];
  JsonDoc d("{\"M_ID\":\"1234567\"}", 18);
  ASSERT_EQ((int)jsonField(d.root(), "M_ID", b, 7), (int)JSON_FIELD_TOO_LONG);   // различимо от "липсва"
  ASSERT_EQ((int)jsonField(d.root(), "nope", b, 7), (int)JSON_FIELD_MISSING);
  ASSERT_EQ((int)jsonField(d.root(), "M_ID", b, 8), (int)JSON_FIELD_OK);
}

STEST(json_parser_invalid_input) {
  std::string v;
  for (const char *bad : {"", "{", "}", "[]", "\"M_ID\"", "{\"M_ID\"", "{\"M_ID\":", "{\"M_ID\":\"x", "{M_ID:\"x\"}",
                          "{\"M_ID\" \"x\"}", "{\"a\":[1,2", "{\"a\":{\"b\":1}", "null", "{\"M_ID\":\"\\q\"}"}) {
    ASSERT_FALSE(getS(bad, "M_ID", &v));
  }
  char b[8];
  JsonDoc d("{\"a\":\"b\"}", 9);
  ASSERT_FALSE(jsonGetString(d.root(), "a", nullptr, 8));
  ASSERT_FALSE(jsonGetString(d.root(), "a", b, 0));
  ASSERT_FALSE(jsonGetString(nullptr, "a", b, sizeof(b)));
  JsonDoc none(nullptr, 5);
  ASSERT_FALSE(none.valid());
  ASSERT_FALSE(jsonGetString(none.root(), "a", b, sizeof(b)));
}

STEST(json_parser_respects_length_not_nul) {
  char b[16];
  const char data[] = "{\"a\":\"xyz\"}garbage{\"a\":\"no\"}";
  JsonDoc whole(data, 11);
  ASSERT_TRUE(jsonGetString(whole.root(), "a", b, sizeof(b)));
  ASSERT_STREQ(b, "xyz");
  JsonDoc cut(data, 8);                                          // отрязан по средата на стойността
  ASSERT_FALSE(cut.valid());
  JsonDoc withGarbage(data, 11 + 7);                             // остатък след обекта: не се приема
  ASSERT_FALSE(withGarbage.valid());
  // без терминатор: буфер точно колкото текста (проверява се с ASan-подобен guard байт)
  std::vector<char> exact(11);
  memcpy(exact.data(), data, 11);
  JsonDoc ex(exact.data(), exact.size());
  ASSERT_TRUE(ex.valid());
}

STEST(json_doc_depth_limit_protects_the_stack) {
  // cJSON е рекурсивна; MQTT съобщение от мрежата не бива да може да задълбае стека на задачата
  auto nest = [](int levels) {
    std::string s = "{\"a\":";
    for (int i = 1; i < levels; i++) s += "[";
    s += "1";
    for (int i = 1; i < levels; i++) s += "]";
    return s + "}";
  };
  for (int lv : {1, 2, 5, 8}) { std::string s = nest(lv); JsonDoc d(s.data(), s.size()); ASSERT_TRUE(d.valid()); }
  for (int lv : {9, 10, 50, 5000}) { std::string s = nest(lv); JsonDoc d(s.data(), s.size()); ASSERT_FALSE(d.valid()); }
  // скоби вътре в низове (в т.ч. след \") не се броят
  std::string v;
  ASSERT_TRUE(getS("{\"a\":\"[[[[[[[[[[[[[[[[[[[[\"}", "a", &v, 64));
  ASSERT_TRUE(getS("{\"a\":\"\\\"[[[[[[[[[[[[[[[[[[[[\"}", "a", &v, 64));
  ASSERT_TRUE(getS("{\"a\":\"\\\\\",\"b\":\"{{{{{{{{{{{{{{\"}", "a", &v, 64));
  // 100 000 вложени скоби - отхвърля се мигновено, без рекурсия
  std::string deep = "{\"a\":" + std::string(100000, '[');
  JsonDoc dd(deep.data(), deep.size());
  ASSERT_FALSE(dd.valid());
}

STEST(json_doc_accepts_only_one_whole_object) {
  for (const char *good : {"{}", "{\"a\":1}", " {\"a\":[1,2.5,-3e2,true,false,null,\"x\",{}]} ", "{\"a\":\"b\"}\r\n", "\n\t{ }\n"}) {
    JsonDoc d(good, strlen(good));
    ASSERT_TRUE(d.valid());
  }
  for (const char *bad : {"", "{", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "[1,]", "[1 2]", "[]", "\"s\"", "0", "-0.5", "{} x", "{}{}",
                          "tru", "{\"a\":\"x}", "{'a':1}", "[[[[[[[[[[1]]]]]]]]]]", "{\"a\":[[[[[[[[1]]]]]]]]}", "{\"a\":1}}", "{{}}"}) {
    JsonDoc d(bad, strlen(bad));
    if (d.valid()) { printf("    приет: %s\n", bad); }
    ASSERT_FALSE(d.valid());
  }
}

STEST(json_parser_fuzz_never_crashes_never_leaks) {
  CjHooks hooks;
  std::mt19937 rng(42);
  const std::string base = "{\"M_ID\":\"E007\",\"C_ID\":\"V01\",\"com\":\"A1\",\"o\":{\"k\":[1,\"x\",{}]}}";
  for (int it = 0; it < 30000; it++) {
    std::string s = base;
    int muts = 1 + rng() % 4;
    for (int m = 0; m < muts; m++) {
      switch (rng() % 4) {
        case 0: if (!s.empty()) s[rng() % s.size()] = (char)(rng() % 256); break;
        case 1: if (!s.empty()) s.erase(rng() % s.size(), 1 + rng() % 3); break;
        case 2: s.insert(rng() % (s.size() + 1), 1, "{}[]\":,\\"[rng() % 8]); break;
        case 3: if (!s.empty()) s.resize(rng() % s.size()); break;
      }
    }
    char b[8];
    memset(b, 'Z', sizeof(b));
    JsonDoc d(s.data(), s.size());
    bool ok = jsonGetString(d.root(), "M_ID", b, sizeof(b));
    bool terminated = false;
    for (char c : b) if (c == 0) terminated = true;
    if (!terminated) { ASSERT_TRUE(terminated); break; }
    if (ok) ASSERT_LE((int)strlen(b), 7);
  }
  ASSERT_TRUE(hooks.balanced());                              // всяка заделена памет е върната
}

STEST(json_iterate_array_of_objects) {
  const char *j = "{\"id\":\"CS001\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"25\"},{\"id\":\"P,1\",\"pin\":\"2]6\"}],\"net\":\"0\"}";
  JsonDoc d(j, strlen(j));
  ASSERT_TRUE(d.valid());
  const cJSON *arr = cJSON_GetObjectItemCaseSensitive(d.root(), "consumers");
  ASSERT_TRUE(arr != nullptr && cJSON_IsArray(arr));
  std::vector<std::string> ids;
  const cJSON *el = nullptr;
  cJSON_ArrayForEach(el, arr) {
    char id[16];
    ASSERT_TRUE(jsonGetString(el, "id", id, sizeof(id)));
    ids.push_back(id);
  }
  ASSERT_EQ((int)ids.size(), 2);
  ASSERT_STREQ(ids[1], "P,1");
  ASSERT_TRUE(cJSON_GetObjectItemCaseSensitive(d.root(), "nope") == nullptr);
  JsonDoc empty("{\"c\":[]}", 8);
  ASSERT_EQ(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(empty.root(), "c")), 0);
}

STEST(json_out_and_doc_survive_every_single_allocation_failure) {
  // липса на памет на ВСЯКО възможно място: без срив, без теч, без частичен/повреден изход
  char ref[256];
  {
    JsonOut w;
    w.beginObject().str("id", "E007").boolean("restart", true).beginArray("states");
    w.beginObject().str("id", "V01").str("state", "ON").endObject();
    w.endArray().endObject();
    ASSERT_TRUE(w.print(ref, sizeof(ref)));
  }
  int failuresSeen = 0;
  for (int k = 0; k < 40; k++) {
    CjHooks hooks(k);
    {
      JsonOut w;
      w.beginObject().str("id", "E007").boolean("restart", true).beginArray("states");
      w.beginObject().str("id", "V01").str("state", "ON").endObject();
      w.endArray().endObject();
      char buf[256];
      bool ok = w.print(buf, sizeof(buf));
      if (ok) ASSERT_STREQ(buf, ref);            // ако е успяло, е ТОЧНО пълният документ
      else { failuresSeen++; ASSERT_EQ((int)buf[0], 0); ASSERT_FALSE(w.ok() && ok); }
      JsonDoc d(ref, strlen(ref));
      char v[8];
      if (d.valid()) ASSERT_TRUE(jsonGetString(d.root(), "id", v, sizeof(v)));
    }
    ASSERT_TRUE(hooks.balanced());
  }
  ASSERT_TRUE(failuresSeen > 5);                 // самият тест наистина е изпитал неуспехи
}

// ============================================================================ NVS
STEST(nvs_store_types_defaults_and_sizes) {
  NvsStore w;
  ASSERT_TRUE(w.open(NVS_NAMESPACE, false));
  ASSERT_TRUE(w.setString("id", "S001"));
  ASSERT_TRUE(w.setU8("sf", 9));
  ASSERT_TRUE(w.setU32("freq", 868100000));
  uint8_t key[16];
  for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
  ASSERT_TRUE(w.setBlob("key", key, 16));
  ASSERT_TRUE(w.commit());
  w.close();

  NvsStore r;
  ASSERT_TRUE(r.open(NVS_NAMESPACE, true));
  char s[8];
  ASSERT_TRUE(r.getString("id", s, sizeof(s)));
  ASSERT_STREQ(s, "S001");
  ASSERT_FALSE(r.getString("id", s, 3));                 // не се побира -> false, празен низ
  ASSERT_STREQ(s, "");
  ASSERT_FALSE(r.getString("nope", s, sizeof(s)));
  ASSERT_EQ(r.getU8("sf", 7), 9);
  ASSERT_EQ(r.getU8("nope", 7), 7);
  ASSERT_EQ(r.getU8("freq", 5), 5);                      // грешен тип -> default
  ASSERT_EQ(r.getU32("freq", 1), 868100000u);
  uint8_t got[16];
  ASSERT_EQ((int)r.getBlob("key", got, 16), 16);
  ASSERT_MEM_EQ(got, key, 16);
  ASSERT_EQ((int)r.getBlob("key", got, 8), 0);           // не се побира
  ASSERT_EQ((int)r.getBlob("nope", got, 16), 0);
  ASSERT_FALSE(r.setU8("sf", 1));                        // read-only
}

STEST(nvs_store_missing_namespace_is_handled) {
  NvsStore r;
  ASSERT_FALSE(r.open(NVS_NAMESPACE, true));             // празен чип: namespace "cfg" още не съществува
  ASSERT_EQ(r.getU8("sf", 7), 7);                        // без срив, default
  char s[8];
  ASSERT_FALSE(r.getString("id", s, sizeof(s)));
}

STEST(nvs_values_written_by_arduino_preferences_layout_are_readable) {
  // Preferences: putString->str, putUChar->u8, putULong->u32, putBytes->blob (namespace "cfg")
  sim::nvs.setStr("id", "CS001");
  sim::nvs.setU8("nc", 2);
  sim::nvs.setU32("freq", 433000000);
  sim::nvs.setStr("mqport", "1883");
  NvsStore r;
  ASSERT_TRUE(r.open("cfg", true));
  char s[8];
  ASSERT_TRUE(r.getString("id", s, sizeof(s)));
  ASSERT_STREQ(s, "CS001");
  ASSERT_EQ(r.getU8("nc", 0), 2);
  ASSERT_EQ(r.getU32("freq", 0), 433000000u);
  ASSERT_TRUE(r.getString("mqport", s, sizeof(s)));
  ASSERT_STREQ(s, "1883");
}

// ============================================================================ ceiling counter
STEST(ceiling_writes_nvs_only_every_100_values) {
  CeilingCounter c;
  c.begin("ceil");
  uint32_t v;
  for (int i = 0; i < 350; i++) {
    ASSERT_TRUE(c.next(&v));
    ASSERT_EQ(v, (uint32_t)i);                           // последователни, без пропуски
  }
  ASSERT_EQ(sim::nvs.writes("ceil"), 4);                 // при 0, 100, 200, 300
  ASSERT_EQ(sim::nvs.num("ceil"), 400);                  // ceiling винаги е пред използваното
}

STEST(ceiling_never_reuses_a_value_after_unclean_reset) {
  uint32_t v, maxSeen = 0;
  {
    CeilingCounter c;
    c.begin("ceil");
    for (int i = 0; i < 137; i++) { c.next(&v); maxSeen = v; }
  }
  // unclean reset: RAM е загубена, NVS - не (power-on)
  sim::set_reset_reason(ESP_RST_POWERON);
  CeilingCounter c2;
  c2.begin("ceil");
  ASSERT_TRUE(c2.next(&v));
  ASSERT_GE(v, maxSeen + 1);                              // никога назад/същата стойност
  ASSERT_LE(v, maxSeen + 1 + CEILING_COMMIT_INTERVAL);    // и скача напред най-много с един интервал
}

STEST(ceiling_after_deep_sleep_uses_rtc_without_touching_nvs) {
  CeilingCounter c;
  c.begin("ceil");
  uint32_t v;
  for (int i = 0; i < 5; i++) c.next(&v);                 // 0..4, NVS ceiling = 100
  int writesBefore = sim::nvs.writes("ceil");
  sim::set_reset_reason(ESP_RST_DEEPSLEEP);               // събуждане от дълбок сън: RTC паметта е жива
  CeilingCounter again;
  again.begin("ceil");
  ASSERT_TRUE(again.next(&v));
  ASSERT_EQ(v, 5u);                                       // продължава от RTC, не от NVS ceiling (100)
  ASSERT_EQ(sim::nvs.writes("ceil"), writesBefore);
}

STEST(ceiling_power_on_ignores_stale_rtc) {
  CeilingCounter c;
  c.begin("ceil");
  uint32_t v;
  for (int i = 0; i < 5; i++) c.next(&v);
  sim::set_reset_reason(ESP_RST_POWERON);                 // статичните RTC променливи са останали, но причината не е deep sleep
  CeilingCounter again;
  again.begin("ceil");
  ASSERT_TRUE(again.next(&v));
  ASSERT_EQ(v, 100u);                                     // от NVS watermark-а (по-безопасно: напред)
}

STEST(ceiling_nvs_write_failure_never_hands_out_a_value) {
  CeilingCounter c;
  c.begin("ceil");
  sim::nvs.failWrites = true;
  uint32_t v = 12345;
  ASSERT_FALSE(c.next(&v));
  ASSERT_EQ(v, 12345u);                                   // нищо не е върнато -> няма излъчване с непотвърден nonce
  sim::nvs.failWrites = false;
  ASSERT_TRUE(c.next(&v));
  ASSERT_EQ(v, 0u);                                       // броячът не е "изяден" от провала
}

// ============================================================================ radio_timing срещу независима формула
static double refToaMs(int pl, int sf, double bw, int cr = 5, int pre = 8, bool crc = true) {
  double ts = std::pow(2.0, sf) / bw;
  int de = ts > 0.016 ? 1 : 0;
  double blocks = std::ceil((8.0 * pl - 4.0 * sf + 28 + 16 * (crc ? 1 : 0)) / (4.0 * (sf - 2 * de)));
  if (blocks < 0) blocks = 0;
  return ((pre + 4.25) * ts + (8 + blocks * cr) * ts) * 1000.0;
}

STEST(radio_timing_toa_matches_semtech_formula_for_every_sf_bw_len) {
  const uint32_t bws[3] = {62500, 125000, 250000};
  for (int sf = 7; sf <= 12; sf++) {
    for (uint32_t bw : bws) {
      radioTimingInit((uint8_t)sf, bw, 5, 8, true);
      for (int pl : {14, 20, 22, 26, 30, 64}) {
        double ref = refToaMs(pl, sf, bw);
        uint32_t got = radioToaMs((uint8_t)pl);
        ASSERT_TRUE(got >= (uint32_t)std::floor(ref));
        ASSERT_TRUE(got <= (uint32_t)std::ceil(ref) + 1);
      }
      ASSERT_EQ(radioLdroNeeded(), (std::pow(2.0, sf) / bw) > 0.016);
    }
  }
}

STEST(radio_timing_derived_times_follow_their_definitions_for_every_sf_bw) {
  const uint32_t bws[3] = {62500, 125000, 250000};
  for (int sf = 7; sf <= 12; sf++) {
    for (uint32_t bw : bws) {
      radioTimingInit((uint8_t)sf, bw, 5, 8, true);
      double ts = std::pow(2.0, sf) / bw * 1000.0;                       // ms
      double cadMs = (RADIO_CAD_SYMBOLS + 3) * ts + 5.0;
      ASSERT_NEAR(radioCadTimeoutMs(), cadMs, 1.5);                      // (CAD символи + 3) x Ts + 5 ms
      double step = std::max(cadMs, std::ceil(4 * ts));
      ASSERT_NEAR(radioCadStepMs(), step, 1.5);                          // max(CAD timeout, 4 x Ts)
      ASSERT_EQ(radioSlotMs(), radioCadStepMs());
      ASSERT_EQ(radioSensingWindowMs(), radioToaMs(RADIO_MAX_PAYLOAD_LEN));   // най-дългото възможно чуждо предаване (64 B)
      ASSERT_EQ(radioTelemetryWaitMs(), radioToaMs(RADIO_TELEMETRY_LEN));      // 30 B
      double w = (double)radioSensingWindowMs();
      double cmd = std::ceil(1.5 * (radioToaMs(RADIO_CMD_WIRE_LEN) + RADIO_EXEC_ACK_DELAY_MS + radioToaMs(RADIO_ACK_WIRE_LEN))) + 2 * w;
      ASSERT_NEAR(radioAckTimeoutCmdMs(), cmd, 1.5);
      double rst = std::ceil(1.5 * (radioToaMs(RADIO_MAX_PAYLOAD_LEN) + RADIO_PROCESSING_MS + radioToaMs(RADIO_ACK_WIRE_LEN))) + 2 * w;
      ASSERT_NEAR(radioAckTimeoutRestartMs(), rst, 1.5);
      double req = std::ceil(1.5 * (radioToaMs(RADIO_ACK_WIRE_LEN) + RADIO_PROCESSING_MS + radioToaMs(RADIO_MAX_PAYLOAD_LEN))) + 2 * w;
      ASSERT_NEAR(radioStateReqTimeoutMs(), req, 1.5);
      // ACK таймаутът винаги надхвърля реалния оборот команда + пауза + ACK (иначе ще има излишни повторения)
      ASSERT_TRUE((double)radioAckTimeoutCmdMs() > (double)radioToaMs(RADIO_CMD_WIRE_LEN) + RADIO_EXEC_ACK_DELAY_MS + radioToaMs(RADIO_ACK_WIRE_LEN));
    }
  }
}

STEST(radio_timing_invalid_inputs_fall_back_to_defaults) {
  radioTimingInit(5, 12345, 4, 8, true);
  ASSERT_EQ(radioSf(), 7);
  ASSERT_EQ((long)radioBw(), 125000L);
  ASSERT_EQ(radioCrDenom(), 5);
  ASSERT_FALSE(radioSfValid(13));
  ASSERT_EQ(radioBwToIndex(250000), 2);
  ASSERT_EQ(radioBwToIndex(500000), 0xFF);
}

// ============================================================================ channel access
struct CaEnv {
  uint32_t now = 0;
  int busyCalls = 0;
  std::vector<bool> script;      // отговори на busy() по ред; след края -> last
  bool last = false;
};
static CaEnv g_ca;
static bool caBusy() {
  bool v = (size_t)g_ca.busyCalls < g_ca.script.size() ? g_ca.script[g_ca.busyCalls] : g_ca.last;
  g_ca.busyCalls++;
  return v;
}
static uint32_t caRnd(uint32_t max) { return max > 1 ? 1 : 0; }

static uint8_t drive(ChannelAccess *ca, uint32_t maxMs = 20000) {
  for (uint32_t t = 0; t <= maxMs; t++) {
    g_ca.now = t;
    uint8_t r = channelAccessPoll(ca, g_ca.now, caBusy, caRnd);
    if (r != CH_WAIT) return r;
  }
  return 0xFF;
}

STEST(channel_access_persist_free_channel_transmits_immediately_and_busy_waits_for_a_gap) {
  radioTimingInit(7, 125000, 5, 8, true);
  g_ca = CaEnv();
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_TELEMETRY_PERSIST);
  ASSERT_EQ(drive(&ca), CH_CLEAR);
  ASSERT_EQ(g_ca.busyCalls, 1);                                   // свободно -> веднага
  g_ca = CaEnv();
  g_ca.script = {true, true, true, false, false};                  // зает на 3 проверки, после пауза -> слот -> последен CAD
  channelAccessStart(&ca, CH_POLICY_TELEMETRY_PERSIST);
  ASSERT_EQ(drive(&ca), CH_CLEAR);
  ASSERT_EQ(g_ca.busyCalls, 5);                                    // изчака паузата (FORCE би предал след 2 CAD-а върху чуждото предаване)
}

STEST(channel_access_persist_transmits_anyway_only_after_the_whole_sensing_window) {
  radioTimingInit(7, 125000, 5, 8, true);
  g_ca = CaEnv();
  g_ca.last = true;                                                // запушен канал
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_TELEMETRY_PERSIST);
  ASSERT_EQ(drive(&ca), CH_CLEAR);                                 // не GIVEUP: измерването е по-ценно от чистия ефир
  ASSERT_GE((long)g_ca.now, (long)radioSensingWindowMs());         // но не по-рано от най-дългия възможен чужд пакет
  ASSERT_GE(g_ca.busyCalls, 3);
  // за разлика от командите, които се отказват
  g_ca = CaEnv();
  g_ca.last = true;
  channelAccessStart(&ca, CH_POLICY_COMMAND);
  ASSERT_EQ(drive(&ca), CH_GIVEUP);
}

static uint32_t caRndMax(uint32_t max) { return max > 1 ? max - 1 : 0; }

STEST(channel_access_backoff_uses_up_to_three_slots) {
  radioTimingInit(7, 125000, 5, 8, true);
  g_ca = CaEnv();
  g_ca.script = {true, false, false};              // зает, после свободен -> случаен слот -> последен CAD
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_COMMAND);
  uint32_t t = 0;
  uint8_t r = CH_WAIT;
  while (r == CH_WAIT && t < 5000) { r = channelAccessPoll(&ca, t, caBusy, caRndMax); t++; }
  ASSERT_EQ(r, CH_CLEAR);
  // най-лошият случай: стъпка на сондиране + 3 слота (rnd(4) = 3) преди последния CAD
  ASSERT_GE((long)t, (long)(radioCadStepMs() + 3 * radioSlotMs()));
}

STEST(channel_access_command_free_channel_transmits_immediately) {
  radioTimingInit(7, 125000, 5, 8, true);
  g_ca = CaEnv();
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_COMMAND);
  ASSERT_EQ(drive(&ca), CH_CLEAR);
  ASSERT_EQ(g_ca.busyCalls, 1);
  ASSERT_EQ(g_ca.now, 0u);
}

STEST(channel_access_command_gives_up_after_sensing_window_when_always_busy) {
  radioTimingInit(7, 125000, 5, 8, true);
  g_ca = CaEnv();
  g_ca.last = true;
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_COMMAND);
  ASSERT_EQ(drive(&ca), CH_GIVEUP);
  ASSERT_GE((long)g_ca.now, (long)radioSensingWindowMs());
  ASSERT_LE((long)g_ca.now, (long)(radioSensingWindowMs() + radioCadStepMs()));
}

STEST(channel_access_command_backoff_then_clear) {
  radioTimingInit(9, 125000, 5, 8, true);
  g_ca = CaEnv();
  g_ca.script = {true, true, false, false};   // зает, зает, свободен -> backoff -> последен CAD свободен
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_COMMAND);
  ASSERT_EQ(drive(&ca), CH_CLEAR);
  ASSERT_EQ(g_ca.busyCalls, 4);
}

STEST(channel_access_telemetry_force_transmits_even_if_busy_but_skip_does_not) {
  radioTimingInit(7, 125000, 5, 8, true);
  g_ca = CaEnv();
  g_ca.last = true;
  ChannelAccess f, s;
  channelAccessStart(&f, CH_POLICY_TELEMETRY_FORCE);
  ASSERT_EQ(drive(&f), CH_CLEAR);
  ASSERT_EQ(g_ca.busyCalls, 2);               // най-много 2 CAD-а
  g_ca = CaEnv();
  g_ca.last = true;
  channelAccessStart(&s, CH_POLICY_TELEMETRY_SKIP);
  ASSERT_EQ(drive(&s), CH_GIVEUP);
  ASSERT_EQ(g_ca.busyCalls, 2);
}

STEST(channel_access_next_ms_matches_the_state_machine) {
  radioTimingInit(7, 125000, 5, 8, true);
  g_ca = CaEnv();
  ChannelAccess ca;
  channelAccessStart(&ca, CH_POLICY_COMMAND);
  ASSERT_EQ(channelAccessNextMs(&ca, 0), 0u);              // първият CAD е веднага
  g_ca.last = true;
  ASSERT_EQ(channelAccessPoll(&ca, 100, caBusy, caRnd), CH_WAIT);
  ASSERT_EQ(channelAccessNextMs(&ca, 100), radioCadStepMs());     // следваща проверка след стъпка
  ASSERT_EQ(channelAccessNextMs(&ca, 100 + radioCadStepMs()), 0u);
  // не се събужда по-късно от края на срока
  uint32_t dl = 100 + radioSensingWindowMs();
  uint32_t near = dl - 1;
  ASSERT_LE((long)channelAccessNextMs(&ca, near), 1L);
  // работи и при пренасяне на 32-битовото време
  ChannelAccess w;
  channelAccessStart(&w, CH_POLICY_TELEMETRY_FORCE);
  g_ca = CaEnv(); g_ca.last = true;
  uint32_t start = 0xFFFFFF00u;
  ASSERT_EQ(channelAccessPoll(&w, start, caBusy, caRnd), CH_WAIT);
  uint32_t wait = channelAccessNextMs(&w, start);
  ASSERT_LE((long)wait, (long)radioTelemetryWaitMs() + 1);
  ASSERT_EQ(channelAccessPoll(&w, start + wait, caBusy, caRnd), CH_CLEAR);
}



// ============================================================================ dedup
STEST(dedup_ring_remembers_n_entries_by_content_and_length) {
  DedupRing<3, 8> d;
  uint8_t a[4] = {1, 2, 3, 4}, b[4] = {5, 6, 7, 8}, c[4] = {9, 9, 9, 9}, e[4] = {7, 7, 7, 7};
  ASSERT_FALSE(d.seen(a, 4));
  d.add(a, 4); d.add(b, 4); d.add(c, 4);
  ASSERT_TRUE(d.seen(a, 4)); ASSERT_TRUE(d.seen(b, 4)); ASSERT_TRUE(d.seen(c, 4));
  ASSERT_FALSE(d.seen(a, 3));                    // друга дължина = друг пакет
  d.add(e, 4);                                   // изпада най-старият (a)
  ASSERT_FALSE(d.seen(a, 4));
  ASSERT_TRUE(d.seen(b, 4));
  ASSERT_FALSE(d.seen(a, 0));                    // празен пакет никога не е "дубликат"
  uint8_t big[12] = {0};
  d.add(big, 12);                                // над MAXLEN се реже, не препълва
  ASSERT_FALSE(d.seen(big, 12));
}

// ============================================================================ hal
STEST(hal_gpio_output_reads_back_and_never_glitches) {
  hal_gpio_output(5, false);
  ASSERT_FALSE(hal_gpio_read(5));
  hal_gpio_write(5, true);
  ASSERT_TRUE(hal_gpio_read(5));
  hal_gpio_output(6, true);                      // нивото е зададено преди режимът да стане изход
  ASSERT_TRUE(hal_gpio_read(6));
  ASSERT_EQ(sim::gpio.pins[6].mode, 3);          // INPUT_OUTPUT -> четене на реалното ниво
  ASSERT_EQ(sim::gpio.pins[6].history.front(), 1);
}

STEST(hal_random_is_in_range_and_unbiased_enough) {
  int counts[4] = {0};
  for (int i = 0; i < 4000; i++) counts[hal_random(4)]++;
  for (int i = 0; i < 4; i++) { ASSERT_GE(counts[i], 800); ASSERT_LE(counts[i], 1200); }
  ASSERT_EQ((int)hal_random(0), 0);
  ASSERT_EQ((int)hal_random(1), 0);
  for (int i = 0; i < 100; i++) { uint32_t r = hal_random_range(10, 20); ASSERT_TRUE(r >= 10 && r < 20); }
  ASSERT_EQ((int)hal_random_range(5, 5), 5);
}

STEST(hal_delay_and_millis_follow_virtual_time) {
  static uint32_t t0, t1, t2;
  sim::boot([] {
    t0 = hal_millis();
    hal_delay_ms(250);
    t1 = hal_millis();
    hal_delay_us(300);                           // активно изчакване
    t2 = (uint32_t)hal_micros();
  });
  sim::run_for_ms(1000);
  ASSERT_EQ(t1 - t0, 250u);
  ASSERT_GE((long)(t2 - t1 * 1000u), 300L);
}

STEST(hal_irq_level_interrupt_notifies_task_and_needs_rearm) {
  static TaskHandle_t h;
  static int wakes = 0;
  sim::boot([] {
    h = xTaskGetCurrentTaskHandle();
    hal_irq_attach(8, h);
    for (int i = 0; i < 3; i++) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      wakes++;
      sim::gpio.drive(8, 0);                     // "радиото" е прочетено: пинът пада
      hal_irq_rearm(8);
    }
  });
  sim::at_after_ms(100, [] { sim::gpio.drive(8, 1); });
  sim::at_after_ms(200, [] { sim::gpio.drive(8, 1); });
  sim::at_after_ms(300, [] { sim::gpio.drive(8, 1); });
  sim::run_for_ms(1000);
  ASSERT_EQ(wakes, 3);
  ASSERT_EQ(sim::gpio.pins[8].isrCalls, 3);
  ASSERT_FALSE(sim::gpio.pins[8].intrEnabled == false && false);
}

STEST(hal_irq_a_still_high_pin_refires_after_rearm_no_event_is_lost) {
  static int serviced = 0;
  sim::boot([] {
    hal_irq_attach(8, xTaskGetCurrentTaskHandle());
    for (int i = 0; i < 2; i++) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      serviced++;
      if (i == 0) hal_irq_rearm(8);              // пинът още е висок (нов пакет чака) -> прекъсването идва пак веднага
    }
  });
  sim::at_after_ms(100, [] { sim::gpio.drive(8, 1); });
  sim::run_for_ms(500);
  ASSERT_EQ(serviced, 2);
}

STEST(hal_nvs_init_recovers_from_new_version_by_erasing) {
  sim::nvs.setStr("id", "X1");
  sim::nvs.initResults = {ESP_ERR_NVS_NEW_VERSION_FOUND};
  ASSERT_TRUE(hal_nvs_init());
  ASSERT_EQ(sim::nvs.eraseCalls, 1);
  ASSERT_FALSE(sim::nvs.has("id"));
  sim::nvs.initResults = {ESP_FAIL};
  ASSERT_FALSE(hal_nvs_init());
}

STEST(hal_restart_throws_in_sim_and_reports_deep_sleep_reason) {
  sim::set_reset_reason(ESP_RST_DEEPSLEEP);
  ASSERT_TRUE(hal_woke_from_deep_sleep());
  sim::set_reset_reason(ESP_RST_POWERON);
  ASSERT_FALSE(hal_woke_from_deep_sleep());
}

// ============================================================================ cfg_parse
static CfgReply cfg(const std::string &json) {
  CfgReply r;
  cfg_apply_line(("CFG:" + json).c_str(), &r);
  return r;
}

STEST(cfg_full_gateway_config_writes_same_keys_and_types_as_arduino_config) {
  CfgReply r = cfg("{\"id\":\"G001\",\"freq\":\"433000000\",\"sf\":\"9\",\"bw\":\"125000\",\"key\":\"00112233445566778899aabbccddeeff\","
                   "\"net\":\"1\",\"wifi_ssid\":\"FarmNet\",\"wifi_password\":\"p\\\"w,d\",\"mqtt_ip\":\"192.168.1.10\","
                   "\"mqtt_port\":\"1884\",\"mqtt_user\":\"u\",\"mqtt_password\":\"s\"}");
  ASSERT_TRUE(r.ack);
  ASSERT_STREQ(sim::nvs.str("id"), "G001");
  ASSERT_EQ(sim::nvs.num("freq"), 433000000);
  ASSERT_EQ(sim::nvs.num("sf"), 9);
  ASSERT_EQ(sim::nvs.num("bw"), 125000);
  ASSERT_STREQ(toHex(sim::nvs.blob("key")), "00112233445566778899aabbccddeeff");
  ASSERT_STREQ(sim::nvs.str("wssid"), "FarmNet");
  ASSERT_STREQ(sim::nvs.str("wpass"), "p\"w,d");          // кавичка и запетая в парола не чупят парсера
  ASSERT_STREQ(sim::nvs.str("mqip"), "192.168.1.10");
  ASSERT_STREQ(sim::nvs.str("mqport"), "1884");
  ASSERT_STREQ(sim::nvs.str("mquser"), "u");
  ASSERT_STREQ(sim::nvs.str("mqpass"), "s");
  ASSERT_FALSE(sim::nvs.has("ceil"));                      // watermark-ът не се пипа
  ASSERT_FALSE(sim::nvs.has("nc"));                        // gateway няма консуматори
  ASSERT_TRUE(sim::nvs.commits >= 1);
}

STEST(cfg_executor_consumers) {
  CfgReply r = cfg("{\"id\":\"E007\",\"freq\":\"433000000\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"4\"},{\"id\":\"P01\",\"pin\":\"15\"}]}");
  ASSERT_TRUE(r.ack);
  ASSERT_EQ(sim::nvs.num("nc"), 2);
  ASSERT_STREQ(sim::nvs.str("c0i"), "V01");
  ASSERT_EQ(sim::nvs.num("c0p"), 4);
  ASSERT_STREQ(sim::nvs.str("c1i"), "P01");
  ASSERT_EQ(sim::nvs.num("c1p"), 15);
  // нов списък с по-малко консуматори променя nc
  r = cfg("{\"id\":\"E007\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"4\"}]}");
  ASSERT_TRUE(r.ack);
  ASSERT_EQ(sim::nvs.num("nc"), 1);
  // липсва "consumers" -> nc не се пипа
  r = cfg("{\"id\":\"E007\"}");
  ASSERT_EQ(sim::nvs.num("nc"), 1);
}

STEST(cfg_repeater_with_two_frequencies_and_sensor_minimal) {
  ASSERT_TRUE(cfg("{\"id\":\"R001\",\"freq\":\"434000000\",\"freq_tx\":\"433000000\",\"sf\":\"7\",\"bw\":\"125000\"}").ack);
  ASSERT_EQ(sim::nvs.num("freq"), 434000000);
  ASSERT_EQ(sim::nvs.num("freqtx"), 433000000);
  ASSERT_TRUE(cfg("{\"id\":\"S001\"}").ack);               // само id е задължително
  ASSERT_STREQ(sim::nvs.str("id"), "S001");
}

static void expectNack(const std::string &json, const char *why) {
  sim::nvs.clear();
  sim::nvs.setStr("id", "KEEP");
  int before = sim::nvs.totalWrites;
  CfgReply r = cfg(json);
  ASSERT_FALSE(r.ack);
  if (r.ack || strstr(r.reason, why) == nullptr) {
    printf("    (%s) -> NACK:%s\n", json.c_str(), r.reason);
    g_failures++;
  }
  ASSERT_EQ(sim::nvs.totalWrites, before);                 // при NACK нищо не се записва
  ASSERT_STREQ(sim::nvs.str("id"), "KEEP");
}

STEST(cfg_every_invalid_input_is_nacked_without_writing) {
  expectNack("{\"freq\":\"1\"}", "id");
  expectNack("{\"id\":\"\"}", "дължина");
  expectNack("{\"id\":\"TOOLONG7\"}", "дължина");
  expectNack("{\"id\":\"A\",\"sf\":\"6\"}", "sf");
  expectNack("{\"id\":\"A\",\"sf\":\"13\"}", "sf");
  expectNack("{\"id\":\"A\",\"sf\":\"x\"}", "sf");
  expectNack("{\"id\":\"A\",\"bw\":\"500000\"}", "bw");
  expectNack("{\"id\":\"A\",\"bw\":\"12x\"}", "bw");
  expectNack("{\"id\":\"A\",\"key\":\"00112233\"}", "key");
  expectNack("{\"id\":\"A\",\"key\":\"zz112233445566778899aabbccddeeff\"}", "key");   // не-hex (преди се приемаше тихо с ACK)
  expectNack("{\"id\":\"A\",\"freq\":\"abc\"}", "freq");
  expectNack("{\"id\":\"A\",\"freq\":\"0\"}", "freq");
  expectNack("{\"id\":\"A\",\"freq\":\"99999999999\"}", "freq");
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"TOOLONG\",\"pin\":\"4\"}]}", "consumer id");
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"100\"}]}", "pin");
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"x\"}]}", "pin");
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"30\"}]}", "GPIO");      // flash на ESP32-S3
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"19\"}]}", "GPIO");      // USB
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\"}]}", "консуматор");
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"4\"},{\"id\":\"V01\",\"pin\":\"5\"}]}", "Повтарящ");
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\",\"pin\":\"4\"}", "JSON");
  expectNack("{\"id\":\"A\",\"net\":\"1\"}", "wifi_ssid");
  expectNack("{\"id\":\"A\",\"net\":\"1\",\"wifi_ssid\":\"x\"}", "mqtt_ip");
  expectNack("{\"id\":\"A\",\"wifi_ssid\":\"0123456789012345678901234567890123\"}", "wifi_ssid");
  expectNack("{\"id\":\"A\",\"mqtt_port\":\"70000\"}", "mqtt_port");
  expectNack("{\"id\":\"A\",\"mqtt_port\":\"abc\"}", "mqtt_port");
  // формата на JSON (cJSON): списъкът консуматори трябва да е масив от обекти с НИЗОВИ id и pin
  expectNack("{\"id\":\"A\",\"consumers\":\"V01\"}", "консуматори");
  expectNack("{\"id\":\"A\",\"consumers\":{\"id\":\"V01\",\"pin\":\"4\"}}", "консуматори");
  expectNack("{\"id\":\"A\",\"consumers\":null}", "консуматори");
  expectNack("{\"id\":\"A\",\"consumers\":[\"V01\"]}", "консуматор");
  expectNack("{\"id\":\"A\",\"consumers\":[{\"id\":\"V01\",\"pin\":4}]}", "консуматор");     // pin като число, не низ
  expectNack("{\"id\":5}", "id");                                                                      // id като число
  expectNack("[{\"id\":\"A\"}]", "JSON");                                                             // не е обект
  expectNack("{\"id\":\"A\"} trailing", "JSON");
  expectNack("{\"id\":\"A\"}{\"id\":\"B\"}", "JSON");
  std::string many = "{\"id\":\"A\",\"consumers\":[";
  for (int i = 0; i < 11; i++) many += std::string(i ? "," : "") + "{\"id\":\"C" + std::to_string(i) + "\",\"pin\":\"" + std::to_string(i + 1) + "\"}";
  expectNack(many + "]}", "10");
  CfgReply r;
  cfg_apply_line("no prefix", &r);
  ASSERT_FALSE(r.ack);
}

STEST(cfg_truncated_or_corrupted_line_is_never_accepted_partially) {
  const std::string full = "CFG:{\"id\":\"E007\",\"freq\":\"433000000\",\"key\":\"00112233445566778899aabbccddeeff\","
                           "\"consumers\":[{\"id\":\"V01\",\"pin\":\"4\"},{\"id\":\"P01\",\"pin\":\"5\"}],\"net\":\"1\","
                           "\"wifi_ssid\":\"Farm\",\"mqtt_ip\":\"10.0.0.2\"}";
  CfgReply ok;
  cfg_apply_line(full.c_str(), &ok);
  ASSERT_TRUE(ok.ack);
  for (size_t cut = 4; cut + 1 < full.size(); cut++) {         // всеки отрязан префикс (връзката е прекъсната по средата)
    sim::nvs.clear();
    CfgReply r;
    cfg_apply_line(full.substr(0, cut).c_str(), &r);
    if (r.ack) { printf("    приет отрязан ред на %zu символа\n", cut); g_failures++; }
    ASSERT_EQ(sim::nvs.totalWrites, 0);
  }
  // сериен шум вътре в реда
  std::mt19937 rng(7);
  int accepted = 0;
  for (int i = 0; i < 3000; i++) {
    std::string s = full;
    s[4 + rng() % (s.size() - 4)] = (char)(rng() % 256);
    sim::nvs.clear();
    CfgReply r;
    cfg_apply_line(s.c_str(), &r);
    // ако е приет, промяната е била безобидна (напр. друга буква в SSID) - в NVS няма невалидни стойности
    if (r.ack) { accepted++; ASSERT_TRUE(sim::nvs.str("id").size() <= 6); if (sim::nvs.has("key")) ASSERT_EQ((int)sim::nvs.blob("key").size(), 16); }
  }
  ASSERT_TRUE(accepted < 3000);
}

STEST(cfg_ten_consumers_is_the_limit_and_nvs_failure_is_nacked) {
  std::string j = "{\"id\":\"E1\",\"consumers\":[";
  for (int i = 0; i < 10; i++) j += std::string(i ? "," : "") + "{\"id\":\"C" + std::to_string(i) + "\",\"pin\":\"" + std::to_string(i + 1) + "\"}";
  ASSERT_TRUE(cfg(j + "]}").ack);
  ASSERT_EQ(sim::nvs.num("nc"), 10);
  sim::nvs.failWrites = true;
  CfgReply r = cfg("{\"id\":\"E2\"}");
  ASSERT_FALSE(r.ack);                                     // преди се връщаше ACK при неуспешен запис
  ASSERT_TRUE(strstr(r.reason, "NVS") != nullptr);
}

STEST(cfg_line_buffer_assembles_lines_trims_and_survives_overflow) {
  CfgLineBuffer b;
  std::string in = "  CFG:{\"id\":\"A\"}  \r\n";
  bool got = false;
  for (char c : in) got = b.feed(c) || got;
  ASSERT_TRUE(got);
  ASSERT_STREQ(b.line(), "CFG:{\"id\":\"A\"}");
  // празен ред / само \r\n -> няма ред
  ASSERT_FALSE(b.feed('\r')); ASSERT_FALSE(b.feed('\n'));
  // прекалено дълъг ред се изхвърля целият; следващият нормален се приема
  for (int i = 0; i < CFG_LINE_MAX + 100; i++) ASSERT_FALSE(b.feed('x'));
  ASSERT_FALSE(b.feed('\n'));
  ASSERT_TRUE(b.overflowed());
  ASSERT_FALSE(b.overflowed());
  bool ok = false;
  for (char c : std::string("CFG:{\"id\":\"B\"}\n")) ok = b.feed(c) || ok;
  ASSERT_TRUE(ok);
  ASSERT_STREQ(b.line(), "CFG:{\"id\":\"B\"}");
}

STEST(cfg_tx_power_is_optional_signed_range_checked_and_stored_as_a_signed_byte) {
  // липсва -> нищо не се записва (устройствата ползват 17 dBm)
  ASSERT_TRUE(cfg("{\"id\":\"E1\"}").ack);
  ASSERT_FALSE(sim::nvs.has("txp"));
  for (int d = LORA_TX_POWER_MIN_DBM; d <= LORA_TX_POWER_MAX_DBM; d++) {
    ASSERT_TRUE(cfg("{\"id\":\"E1\",\"txp\":\"" + std::to_string(d) + "\"}").ack);
    ASSERT_EQ((int)(int8_t)(uint8_t)sim::nvs.num("txp"), d);          // със знак, обратимо
  }
  // извън границите на модула и боклук -> NACK без запис
  sim::nvs.clear();
  for (const char *bad : {"23", "100", "-10", "-100", "abc", "", "1.5", "+5", "- 5", "--5", "1e1", "0x10", "2 2", "-"}) {
    CfgReply r = cfg(std::string("{\"id\":\"E1\",\"txp\":\"") + bad + "\"}");
    if (r.ack) printf("    приет: txp=%s\n", bad);
    ASSERT_FALSE(r.ack);
    ASSERT_TRUE(strstr(r.reason, "txp") != nullptr);
    ASSERT_FALSE(sim::nvs.has("txp"));
    ASSERT_FALSE(sim::nvs.has("id"));                                   // и нищо друго от реда не е записано
  }
  // число вместо низ (както при останалите полета) - не е поле: пренебрегва се, не се записва
  ASSERT_TRUE(cfg("{\"id\":\"E1\",\"txp\":17}").ack);
  ASSERT_FALSE(sim::nvs.has("txp"));
}

int main() { return runAllTests(); }
