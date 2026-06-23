/// @file counter_store.cpp
/// CounterStore implementation.

#include "common/counter_store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace wpce::common
{

namespace
{

class MiniJsonWriter
{
 public:
  explicit MiniJsonWriter(std::ostream& out) : out_(out) {}
  void beginObject() { beforeValue(); out_ << '{'; stack_.push_back({true, true}); }
  void endObject()   { out_ << '}'; if (!stack_.empty()) stack_.pop_back(); }
  void beginArray()  { beforeValue(); out_ << '['; stack_.push_back({false, true}); }
  void endArray()    { out_ << ']'; if (!stack_.empty()) stack_.pop_back(); }
  void key(const std::string& k)
  {
    if (stack_.empty() || !stack_.back().is_object) return;
    auto& ctx = stack_.back();
    if (!ctx.first) out_ << ',';
    ctx.first = false;
    ctx.expecting_value = true;
    out_ << '"' << esc(k) << "\":";
  }
  void writeString(const std::string& s) { beforeValue(); out_ << '"' << esc(s) << '"'; }
  void writeLong(long v) { beforeValue(); out_ << v; }
 private:
  struct Ctx { bool is_object; bool first; bool expecting_value{false}; };
  std::vector<Ctx> stack_;
  std::ostream& out_;
  void beforeValue()
  {
    if (stack_.empty()) return;
    auto& ctx = stack_.back();
    if (ctx.is_object) { if (ctx.expecting_value) ctx.expecting_value = false; return; }
    if (!ctx.first) out_ << ',';
    ctx.first = false;
  }
  static std::string esc(const std::string& s)
  {
    std::string r; r.reserve(s.size());
    for (char c : s) {
      switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        default: r += c;
      }
    }
    return r;
  }
};

class MiniJsonReader
{
 public:
  explicit MiniJsonReader(std::istream& in) : in_(in) { skipWs(); }
  void beginObject() { expect('{'); skipWs(); stack_.push_back({true, true}); }
  bool endObjectOrNext()
  {
    skipWs();
    if (peek() == '}') { get(); if (!stack_.empty()) stack_.pop_back(); return false; }
    if (!stack_.empty() && stack_.back().first) { stack_.back().first = false; return true; }
    expect(','); return true;
  }
  std::string readKey() { std::string k = readString(); skipWs(); expect(':'); skipWs(); return k; }
  std::string readString()
  {
    expect('"'); std::string r;
    for (;;) {
      int c = get(); if (c == '"') break;
      if (c == '\\') { c = get(); switch (c) { case '"': r+='"'; break; case '\\': r+='\\'; break; case 'n': r+='\n'; break; case 'r': r+='\r'; break; case 't': r+='\t'; break; default: r+=static_cast<char>(c); } }
      else r += static_cast<char>(c);
    }
    skipWs(); return r;
  }
  long readLong() { std::string t; while (std::isdigit(peek())||peek()=='-'||peek()=='+'||peek()=='.') t+=static_cast<char>(get()); skipWs(); return std::stol(t); }
 private:
  struct Ctx { bool is_object; bool first; };
  std::vector<Ctx> stack_;
  std::istream& in_;
  int peek() const { return in_.peek(); }
  int get() { int c=in_.get(); if(c==std::char_traits<char>::eof()) throw std::runtime_error("unexpected end of JSON"); return c; }
  void expect(char e) { int c=get(); if(c!=e) throw std::runtime_error("expected different char"); }
  void skipWs() { while(std::isspace(peek())) get(); }
};

}  // namespace

std::string CounterStore::currentDateUtcYyyymmdd()
{
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_utc{};
#ifdef _WIN32
  gmtime_s(&tm_utc, &t);
#else
  gmtime_r(&t, &tm_utc);
#endif
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d",
                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
  return buf;
}

void CounterStore::setStatePath(const std::filesystem::path& path)
{
  {
    std::scoped_lock lock(mutex_);
    path_ = path;
  }
  // Load outside the lock to avoid deadlock (load() acquires the lock).
  (void)load();
}

bool CounterStore::load()
{
  std::scoped_lock lock(mutex_);
  if (path_.empty()) return false;
  try
  {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return true;  // file missing is OK — counters stay at 0
    MiniJsonReader r(in);
    r.beginObject();
    while (r.endObjectOrNext())
    {
      const std::string k = r.readKey();
      if (k == "last_date_yyyymmdd")  counters_.last_date_yyyymmdd = r.readString();
      else if (k == "batch_counter")  counters_.batch_counter = r.readLong();
      else if (k == "order_counter")  counters_.order_counter = r.readLong();
      else (void)r.readString();
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool CounterStore::save() const
{
  std::scoped_lock lock(mutex_);
  return saveLocked();
}

bool CounterStore::saveLocked() const
{
  if (path_.empty()) return false;
  try
  {
    std::filesystem::create_directories(path_.parent_path());
    const auto temp = path_.string() + ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      MiniJsonWriter w(out);
      w.beginObject();
      w.key("last_date_yyyymmdd"); w.writeString(counters_.last_date_yyyymmdd);
      w.key("batch_counter");      w.writeLong(counters_.batch_counter);
      w.key("order_counter");      w.writeLong(counters_.order_counter);
      w.endObject();
      out << '\n';
      if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, path_, ec);
    if (ec)
    {
      std::filesystem::copy_file(temp, path_,
                                  std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) return false;
      std::filesystem::remove(temp, ec);
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

void CounterStore::rollDateIfNeeded(const std::string& today)
{
  if (counters_.last_date_yyyymmdd != today)
  {
    counters_.last_date_yyyymmdd = today;
    counters_.batch_counter = 0;
    counters_.order_counter = 0;
  }
}

std::string CounterStore::nextBatchId()
{
  std::scoped_lock lock(mutex_);
  const std::string today = currentDateUtcYyyymmdd();
  rollDateIfNeeded(today);
  ++counters_.batch_counter;
  (void)saveLocked();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "B-%s-%06ld", today.c_str(), counters_.batch_counter);
  return buf;
}

std::string CounterStore::nextOrderId()
{
  std::scoped_lock lock(mutex_);
  const std::string today = currentDateUtcYyyymmdd();
  rollDateIfNeeded(today);
  ++counters_.order_counter;
  (void)saveLocked();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "SETTLE-%s-%06ld", today.c_str(), counters_.order_counter);
  return buf;
}

}  // namespace wpce::common
