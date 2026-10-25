#pragma once

#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace wpce::storage
{
// Shared strict reader for billing buckets and frozen orders. No external
// dependency or Qt ownership crosses the persistence boundary.
class JsonReader
{
 public:
  explicit JsonReader(std::istream& in) : in_(in)
  {
    if (peek() == 0xef) { (void)get(); if (get() != 0xbb || get() != 0xbf) fail(); }
    ws();
  }
  void beginObject() { begin('{', true); }
  void beginArray() { begin('[', false); }
  bool endObjectOrNext() { return next('}'); }
  bool endArrayOrNext() { return next(']'); }
  std::string readKey()
  {
    auto key = readString();
    if (stack_.empty() || !stack_.back().keys.insert(key).second) fail();
    expect(':'); ws(); return key;
  }
  std::string readString()
  {
    ws(); expect('"'); std::string value;
    for (;;)
    {
      const int c = get();
      if (c == '"') break;
      if (c != '\\')
      {
        // Historical WPCE writers emitted the physical identity separator
        // literally. Accept that one byte for migration; all writers escape it.
        if (c < 0x20 && c != 0x1f) fail();
        value += static_cast<char>(c); continue;
      }
      switch (get())
      {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'u':
        {
          auto cp = hex4();
          if (cp >= 0xd800 && cp <= 0xdbff)
          {
            expect('\\'); expect('u'); const auto low = hex4();
            if (low < 0xdc00 || low > 0xdfff) fail();
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
          }
          else if (cp >= 0xdc00 && cp <= 0xdfff) fail();
          utf8(value, cp); break;
        }
        default: fail();
      }
    }
    ws(); return value;
  }
  double readNumber()
  {
    ws(); std::string token;
    if (peek() == '-') token += static_cast<char>(get());
    if (peek() == '0') token += static_cast<char>(get());
    else
    {
      if (peek() < '1' || peek() > '9') fail();
      do { token += static_cast<char>(get()); } while (digit(peek()));
    }
    if (peek() == '.')
    {
      token += static_cast<char>(get()); if (!digit(peek())) fail();
      do { token += static_cast<char>(get()); } while (digit(peek()));
    }
    if (peek() == 'e' || peek() == 'E')
    {
      token += static_cast<char>(get());
      if (peek() == '+' || peek() == '-') token += static_cast<char>(get());
      if (!digit(peek())) fail();
      do { token += static_cast<char>(get()); } while (digit(peek()));
    }
    std::istringstream number(token); number.imbue(std::locale::classic());
    double value{}; number >> value;
    if (!number || !std::isfinite(value)) fail();
    ws(); return value;
  }
  int readInt()
  {
    const double v = readNumber();
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max() || std::trunc(v) != v) fail();
    return static_cast<int>(v);
  }
  std::uint32_t readUInt32()
  {
    const double v = readNumber();
    if (v < 0 || v > std::numeric_limits<std::uint32_t>::max() || std::trunc(v) != v) fail();
    return static_cast<std::uint32_t>(v);
  }
  bool readBool()
  {
    ws();
    if (peek() == 't') { literal("true"); return true; }
    literal("false"); return false;
  }
  void skipValue()
  {
    ws();
    switch (peek())
    {
      case '"': (void)readString(); return;
      case '{':
        beginObject();
        while (endObjectOrNext()) { (void)readKey(); skipValue(); }
        return;
      case '[':
        beginArray(); while (endArrayOrNext()) skipValue(); return;
      case 't': case 'f': (void)readBool(); return;
      case 'n': literal("null"); return;
      default: (void)readNumber();
    }
  }
  std::string readRawValue()
  {
    ws(); if (peek() != '{') fail();
    std::string raw; int depth = 0; bool quoted = false, escaped = false;
    do
    {
      const char c = static_cast<char>(get()); raw += c;
      if (quoted)
      {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') quoted = false;
      }
      else if (c == '"') quoted = true;
      else if (c == '{' || c == '[') { if (++depth > 64) fail(); }
      else if (c == '}' || c == ']') --depth;
    } while (depth > 0);
    std::istringstream check(raw); JsonReader reader(check);
    reader.skipValue(); reader.finish(); ws(); return raw;
  }
  void finish() { ws(); if (!stack_.empty() || peek() != std::char_traits<char>::eof() || in_.bad()) fail(); }
  // Compatibility names used by SettlementBridge.
  void begin_object() { beginObject(); } void begin_array() { beginArray(); }
  bool end_object_or_next() { return endObjectOrNext(); } bool end_array_or_next() { return endArrayOrNext(); }
  std::string read_key() { return readKey(); } std::string read_string() { return readString(); }
  double read_number() { return readNumber(); } int read_int() { return readInt(); }
  std::uint32_t read_uint32() { return readUInt32(); } bool read_bool() { return readBool(); }
  void read_null() { literal("null"); }
 private:
  struct Context { bool object; bool first{true}; std::unordered_set<std::string> keys; };
  std::istream& in_;
  std::vector<Context> stack_;
  [[noreturn]] static void fail() { throw std::runtime_error("invalid billing JSON"); }
  static bool digit(int c) { return c >= '0' && c <= '9'; }
  int peek() { return in_.peek(); }
  int get() { const int c = in_.get(); if (c == std::char_traits<char>::eof()) fail(); return c; }
  void expect(char c) { if (get() != c) fail(); }
  void ws() { while (peek() == ' ' || peek() == '\n' || peek() == '\r' || peek() == '\t') (void)get(); }
  void literal(const char* value) { while (*value) expect(*value++); ws(); }
  void begin(char c, bool object)
  { ws(); expect(c); if (stack_.size() >= 64) fail(); stack_.push_back({object, true, {}}); ws(); }
  bool next(char c)
  {
    ws(); if (stack_.empty()) fail();
    if (peek() == c) { (void)get(); stack_.pop_back(); ws(); return false; }
    if (!stack_.back().first) { expect(','); ws(); }
    stack_.back().first = false; return true;
  }
  std::uint32_t hex4()
  {
    std::uint32_t cp = 0;
    for (int i = 0; i < 4; ++i)
    {
      const int c = get();
      const int n = digit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (n < 0) fail(); cp = cp * 16 + static_cast<unsigned>(n);
    }
    return cp;
  }
  static void utf8(std::string& s, std::uint32_t cp)
  {
    if (cp <= 0x7f) s += static_cast<char>(cp);
    else if (cp <= 0x7ff) { s += static_cast<char>(0xc0 | cp >> 6); s += static_cast<char>(0x80 | (cp & 63)); }
    else if (cp <= 0xffff) { s += static_cast<char>(0xe0 | cp >> 12); s += static_cast<char>(0x80 | (cp >> 6 & 63)); s += static_cast<char>(0x80 | (cp & 63)); }
    else { s += static_cast<char>(0xf0 | cp >> 18); s += static_cast<char>(0x80 | (cp >> 12 & 63)); s += static_cast<char>(0x80 | (cp >> 6 & 63)); s += static_cast<char>(0x80 | (cp & 63)); }
  }
};
}
