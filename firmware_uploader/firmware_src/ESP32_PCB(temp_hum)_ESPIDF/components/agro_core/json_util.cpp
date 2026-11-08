#include "json_util.h"
#include <inttypes.h>

#include <stdio.h>
#include <string.h>

// ======================================================================= JsonDoc
// Най-голямата вложеност на скоби извън низовете (низовете се прескачат с коректно отчитане на \").
static bool depthOk(const char *s, size_t n, int32_t maxDepth) {
  int32_t depth = 0;
  bool inStr = false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (inStr) {
      if (c == '\\') i++;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{' || c == '[') { if (++depth > maxDepth) return false; }
    else if (c == '}' || c == ']') { if (depth > 0) depth--; }
  }
  return true;
}

JsonDoc::JsonDoc(const char *text, size_t len) : root_(nullptr) {
  if (text == nullptr || len == 0) return;
  if (!depthOk(text, len, JSON_MAX_DEPTH)) return;
  const char *end = nullptr;
  cJSON *r = cJSON_ParseWithLengthOpts(text, len, &end, 0);
  if (r == nullptr) return;
  const char *stop = text + len;
  while (end != nullptr && end < stop && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
  if (end != stop || !cJSON_IsObject(r)) {      // остатък след обекта или не е обект
    cJSON_Delete(r);
    return;
  }
  root_ = r;
}

JsonDoc::~JsonDoc() { cJSON_Delete(root_); }

JsonField jsonField(const cJSON *obj, const char *key, char *out, size_t cap) {
  if (out == nullptr || cap == 0) return JSON_FIELD_MISSING;
  out[0] = 0;
  if (obj == nullptr || !cJSON_IsObject(obj)) return JSON_FIELD_MISSING;
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (it == nullptr || !cJSON_IsString(it) || it->valuestring == nullptr) return JSON_FIELD_MISSING;
  size_t n = strlen(it->valuestring);
  if (n >= cap) return JSON_FIELD_TOO_LONG;
  memcpy(out, it->valuestring, n + 1);
  return JSON_FIELD_OK;
}

// ======================================================================= JsonOut
JsonOut::JsonOut() : root_(nullptr), depth_(0), ok_(true) {
  for (int32_t i = 0; i < JSON_MAX_DEPTH; i++) stack_[i] = nullptr;
}

JsonOut::~JsonOut() { cJSON_Delete(root_); }

bool JsonOut::push(cJSON *c) {
  // Възелът c (ако не е nullptr) вече е закачен към родителя си и се освобождава от деструктора заедно с дървото:
  // тук НЕ се трие - иначе се освобождава два пъти (use-after-free).
  if (c == nullptr || depth_ >= JSON_MAX_DEPTH) { fail(); return false; }
  stack_[depth_++] = c;
  return true;
}

JsonOut &JsonOut::beginObject() {
  if (!ok_) return *this;
  if (root_ == nullptr) {
    root_ = cJSON_CreateObject();
    push(root_);
    return *this;
  }
  cJSON *parent = cur();
  if (parent == nullptr || !cJSON_IsArray(parent)) { fail(); return *this; }   // обект само като елемент на масив
  cJSON *o = cJSON_CreateObject();
  if (o == nullptr || !cJSON_AddItemToArray(parent, o)) { cJSON_Delete(o); fail(); return *this; }
  push(o);
  return *this;
}

JsonOut &JsonOut::endObject() {
  if (depth_ > 0) depth_--;
  return *this;
}

JsonOut &JsonOut::beginArray(const char *key) {
  if (!ok_) return *this;
  cJSON *parent = cur();
  if (parent == nullptr || !cJSON_IsObject(parent)) { fail(); return *this; }
  cJSON *a = cJSON_AddArrayToObject(parent, key);
  if (a == nullptr) { fail(); return *this; }
  push(a);
  return *this;
}

JsonOut &JsonOut::endArray() {
  if (depth_ > 0) depth_--;
  return *this;
}

JsonOut &JsonOut::str(const char *key, const char *val) {
  if (!ok_) return *this;
  cJSON *o = cur();
  if (o == nullptr || !cJSON_IsObject(o) || cJSON_AddStringToObject(o, key, val) == nullptr) fail();
  return *this;
}

JsonOut &JsonOut::num(const char *key, int32_t v) {
  if (!ok_) return *this;
  cJSON *o = cur();
  if (o == nullptr || !cJSON_IsObject(o) || cJSON_AddNumberToObject(o, key, (double)v) == nullptr) fail();
  return *this;
}

JsonOut &JsonOut::unum(const char *key, uint32_t v) {
  if (!ok_) return *this;
  cJSON *o = cur();
  if (o == nullptr || !cJSON_IsObject(o) || cJSON_AddNumberToObject(o, key, (double)v) == nullptr) fail();
  return *this;
}

// Винаги ДВА десетични знака (22.00, -0.05), байт по байт както в Arduino версията и в предишния писач:
// стойността се подава на cJSON като готов суров токен (cJSON_AddRawToObject), без печат на double.
JsonOut &JsonOut::fixed2(const char *key, int32_t hundredths) {
  if (!ok_) return *this;
  cJSON *o = cur();
  int64_t a = hundredths < 0 ? -(int64_t)hundredths : (int64_t)hundredths;
  char t[24];
  snprintf(t, sizeof(t), "%s%" PRId32 ".%02" PRId32, hundredths < 0 ? "-" : "", (int32_t)(a / 100), (int32_t)(a % 100));
  if (o == nullptr || !cJSON_IsObject(o) || cJSON_AddRawToObject(o, key, t) == nullptr) fail();
  return *this;
}

JsonOut &JsonOut::boolean(const char *key, bool v) {
  if (!ok_) return *this;
  cJSON *o = cur();
  if (o == nullptr || !cJSON_IsObject(o) || cJSON_AddBoolToObject(o, key, v ? 1 : 0) == nullptr) fail();
  return *this;
}

bool JsonOut::print(char *buf, size_t cap, size_t *outLen) {
  if (outLen) *outLen = 0;
  if (cap > 0) buf[0] = 0;
  if (!ok() || cap < 3 || depth_ != 0) return false;     // непълно дърво (липсва endObject/endArray)
  if (!cJSON_PrintPreallocated(root_, buf, (int32_t)cap, 0)) { buf[0] = 0; return false; }
  if (outLen) *outLen = strlen(buf);
  return true;
}
