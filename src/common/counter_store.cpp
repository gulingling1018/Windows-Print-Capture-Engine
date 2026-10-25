/// @file counter_store.cpp
/// CounterStore implementation.

#include "common/counter_store.h"
#include "common/business_clock.h"
#include "storage/file_utils.h"
#include "storage/json_reader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <limits>
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

}  // namespace

std::string CounterStore::currentDateYyyymmdd()
{
  return current_business_date().yyyymmdd;
}

std::string CounterStore::currentDateUtcYyyymmdd()
{
  return currentDateYyyymmdd();
}

void CounterStore::setStatePath(const std::filesystem::path& path)
{
  {
    std::scoped_lock lock(mutex_);
    path_ = path;
    counters_ = {};
  }
  // Load outside the lock to avoid deadlock (load() acquires the lock).
  (void)load();
}

bool CounterStore::load()
{
  std::scoped_lock lock(mutex_);
  if (path_.empty()) { loaded_ok_ = false; return false; }
  try
  {
    std::ifstream in(path_, std::ios::binary);
    if (!in)
    {
      std::error_code ec;
      loaded_ok_ = !std::filesystem::exists(path_, ec) && !ec;
      return loaded_ok_;
    }
    Counters staged;
    bool dateSeen = false, batchSeen = false, orderSeen = false;
    storage::JsonReader r(in);
    r.beginObject();
    while (r.endObjectOrNext())
    {
      const std::string k = r.readKey();
      if (k == "last_date_yyyymmdd") { staged.last_date_yyyymmdd = r.readString(); dateSeen = true; }
      else if (k == "batch_counter") { staged.batch_counter = r.readInt(); batchSeen = true; }
      else if (k == "order_counter") { staged.order_counter = r.readInt(); orderSeen = true; }
      else r.skipValue();
    }
    r.finish();
    if (!dateSeen || !batchSeen || !orderSeen || staged.batch_counter < 0 || staged.order_counter < 0)
      throw std::runtime_error("invalid counter state");
    counters_ = std::move(staged);
    loaded_ok_ = true;
    return true;
  }
  catch (...)
  {
    loaded_ok_ = false;
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
  if (path_.empty() || !loaded_ok_) return false;
  try
  {
    std::ostringstream out;
    {
      MiniJsonWriter w(out);
      w.beginObject();
      w.key("last_date_yyyymmdd"); w.writeString(counters_.last_date_yyyymmdd);
      w.key("batch_counter");      w.writeLong(counters_.batch_counter);
      w.key("order_counter");      w.writeLong(counters_.order_counter);
      w.endObject();
      out << '\n';
      if (!out) return false;
    }
    storage::write_text_file_atomic(out.str(), path_);
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool CounterStore::healthy() const
{
  std::scoped_lock lock(mutex_); return loaded_ok_;
}
bool CounterStore::ensureMinimum(const std::string& date, long batch_counter, long order_counter)
{
  std::scoped_lock lock(mutex_);
  if (!loaded_ok_) return false;
  rollDateIfNeeded(date);
  counters_.batch_counter = std::max(counters_.batch_counter, batch_counter);
  counters_.order_counter = std::max(counters_.order_counter, order_counter);
  return saveLocked();
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

std::string CounterStore::nextBatchId(const std::string& business_date)
{
  std::scoped_lock lock(mutex_);
  if (!loaded_ok_) return {};
  const std::string today = business_date.empty() ? currentDateYyyymmdd() : business_date;
  rollDateIfNeeded(today);
  if (counters_.batch_counter == std::numeric_limits<long>::max()) return {};
  ++counters_.batch_counter;
  // Never return an ID before persistence succeeds. Failed attempts may
  // leave gaps; the service also raises counters to the loaded ledger floor.
  if (!saveLocked())
    return {};  // disk failure - signal to caller
  char buf[32];
  std::snprintf(buf, sizeof(buf), "B-%s-%06ld", today.c_str(), counters_.batch_counter);
  return buf;
}

std::string CounterStore::nextOrderId(const std::string& business_date)
{
  std::scoped_lock lock(mutex_);
  if (!loaded_ok_) return {};
  const std::string today = business_date.empty() ? currentDateYyyymmdd() : business_date;
  rollDateIfNeeded(today);
  if (counters_.order_counter == std::numeric_limits<long>::max()) return {};
  ++counters_.order_counter;
  if (!saveLocked())
    return {};  // disk failure - signal to caller
  char buf[32];
  std::snprintf(buf, sizeof(buf), "SETTLE-%s-%06ld", today.c_str(), counters_.order_counter);
  return buf;
}

}  // namespace wpce::common
