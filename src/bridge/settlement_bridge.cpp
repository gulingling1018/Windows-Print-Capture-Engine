/// @file settlement_bridge.cpp
/// Settlement bridge implementation.

#include "bridge/settlement_bridge.h"
#include "common/pricing_engine.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace wpce::bridge
{
namespace
{

// ── Minimal JSON writer (same pattern as app_config.cpp) ────────────────

class JsonWriter
{
public:
  explicit JsonWriter(std::ostream& out) : out_(out) {}

  void begin_object() { before_value(); out_ << '{'; stack_.push_back({true, true}); }
  void end_object()   { out_ << '}'; if (!stack_.empty()) stack_.pop_back(); }
  void begin_array()  { before_value(); out_ << '['; stack_.push_back({false, true}); }
  void end_array()    { out_ << ']'; if (!stack_.empty()) stack_.pop_back(); }

  void key(const std::string& k)
  {
    if (stack_.empty() || !stack_.back().is_object) return;
    auto& ctx = stack_.back();
    if (!ctx.first) out_ << ',';
    ctx.first = false;
    ctx.expecting_value = true;
    out_ << '"' << esc(k) << "\":";
  }

  void write_string(const std::string& s) { before_value(); out_ << '"' << esc(s) << '"'; }
  void write_int(int v)                   { before_value(); out_ << v; }
  void write_uint32(std::uint32_t v)      { before_value(); out_ << v; }
  void write_double(double v)             { before_value(); char buf[64]; std::snprintf(buf, sizeof(buf), "%.2f", v); out_ << buf; }
  void write_bool(bool v)                 { before_value(); out_ << (v ? "true" : "false"); }

private:
  struct Ctx { bool is_object; bool first; bool expecting_value{false}; };
  std::vector<Ctx> stack_;
  std::ostream& out_;

  void before_value()
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

// ── Minimal JSON reader ──────────────────────────────────────────────────

class JsonReader
{
public:
  explicit JsonReader(std::istream& in) : in_(in) { skip_ws(); }

  void begin_object() { expect('{'); skip_ws(); stack_.push_back({true, true}); }
  bool end_object_or_next()
  {
    skip_ws();
    if (peek() == '}') { get(); if (!stack_.empty()) stack_.pop_back(); return false; }
    if (!stack_.empty() && stack_.back().first) { stack_.back().first = false; return true; }
    expect(','); return true;
  }
  void begin_array() { expect('['); skip_ws(); stack_.push_back({false, true}); }
  bool end_array_or_next()
  {
    skip_ws();
    if (peek() == ']') { get(); if (!stack_.empty()) stack_.pop_back(); return false; }
    if (!stack_.empty() && stack_.back().first) { stack_.back().first = false; return true; }
    expect(','); return true;
  }

  std::string read_key() { std::string k = read_string(); skip_ws(); expect(':'); skip_ws(); return k; }
  std::string read_string()
  {
    expect('"'); std::string r;
    for (;;) {
      int c = get(); if (c == '"') break;
      if (c == '\\') { c = get(); switch (c) { case '"': r+='"'; break; case '\\': r+='\\'; break; case 'n': r+='\n'; break; case 'r': r+='\r'; break; case 't': r+='\t'; break; default: r+=static_cast<char>(c); } }
      else r += static_cast<char>(c);
    }
    skip_ws(); return r;
  }
  double read_number() { std::string t; while (std::isdigit(peek())||peek()=='-'||peek()=='+'||peek()=='.') t+=static_cast<char>(get()); skip_ws(); return std::stod(t); }
  int read_int() { return static_cast<int>(read_number()); }
  std::uint32_t read_uint32() { return static_cast<std::uint32_t>(read_number()); }
  bool read_bool() {
    if (peek()=='t') { for(int i=0;i<4;++i)get(); skip_ws(); return true; }
    for(int i=0;i<5;++i)get(); skip_ws(); return false;
  }
  void read_null() { for(int i=0;i<4;++i)get(); skip_ws(); }

private:
  struct Ctx { bool is_object; bool first; };
  std::vector<Ctx> stack_;
  std::istream& in_;
  int peek() const { return in_.peek(); }
  int get() { int c=in_.get(); if(c==std::char_traits<char>::eof()) throw std::runtime_error("unexpected end of JSON"); return c; }
  void expect(char e) { int c=get(); if(c!=e) throw std::runtime_error("expected different char"); }
  void skip_ws() { while(std::isspace(peek())) get(); }
};

// ── Timestamp helpers ────────────────────────────────────────────────────

std::string utc_now_iso8601()
{
  const auto now = std::chrono::system_clock::now();
  const auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::tm tm_utc{};
#ifdef _WIN32
  gmtime_s(&tm_utc, &time_t_now);
#else
  gmtime_r(&time_t_now, &tm_utc);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
  return buf;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

std::string SettlementBridge::generateOrderId(
    const std::string& date_yyyymmdd, int daily_counter)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "SETTLE-%s-%03d",
                date_yyyymmdd.c_str(), daily_counter);
  return buf;
}

std::string SettlementBridge::snapshotPricingConfig(
    const common::PricingConfig& config)
{
  // Create a temporary AppConfig and use its built-in serialisation.
  common::AppConfig temp;
  temp.pricing = config;

  // Write to a temp file and read back as string.
  const auto temp_path = std::filesystem::temp_directory_path() / "wpce_pricing_snapshot.tmp";
  if (!temp.save_to_json(temp_path)) return "{}";

  std::ifstream in(temp_path, std::ios::binary);
  if (!in) return "{}";

  std::ostringstream oss;
  oss << in.rdbuf();
  std::error_code ec;
  std::filesystem::remove(temp_path, ec);
  return oss.str();
}

core::SettlementOrder SettlementBridge::createSettlementOrder(
    const std::string& order_id,
    const std::string& task_id,
    const std::string& user_id,
    const std::vector<std::uint32_t>& job_ids,
    const std::vector<core::JobPricingEntry>& pricing_entries,
    const common::PricingConfig& pricing_snapshot,
    const std::string& period_start_utc,
    const std::string& period_end_utc,
    const std::string& settled_by)
{
  core::SettlementOrder order;
  order.schema_version = 1;
  order.order_id = order_id;
  order.task_id = task_id;
  order.user_id = user_id;
  order.settled_by = settled_by;
  order.settled_at_utc = utc_now_iso8601();
  order.period_start_utc = period_start_utc;
  order.period_end_utc = period_end_utc;
  order.job_ids = job_ids;
  order.pricing_entries = pricing_entries;
  order.pricing_snapshot_json = snapshotPricingConfig(pricing_snapshot);

  // Compute total price.
  double total = 0.0;
  for (const auto& entry : pricing_entries)
    total += entry.total_price;
  order.total_price = total;
  order.currency = pricing_snapshot.currency;

  return order;
}

std::string SettlementBridge::toJson(const core::SettlementOrder& order)
{
  std::ostringstream out;
  JsonWriter w(out);
  w.begin_object();

  w.key("schema_version");    w.write_int(order.schema_version);
  w.key("order_id");          w.write_string(order.order_id);
  w.key("task_id");           w.write_string(order.task_id);
  w.key("user_id");           w.write_string(order.user_id);
  w.key("settled_by");        w.write_string(order.settled_by);
  w.key("settled_at_utc");    w.write_string(order.settled_at_utc);
  w.key("period_start_utc");  w.write_string(order.period_start_utc);
  w.key("period_end_utc");    w.write_string(order.period_end_utc);
  w.key("currency");          w.write_string(order.currency);
  w.key("total_price");       w.write_double(order.total_price);

  // job_ids array
  w.key("job_ids");
  w.begin_array();
  for (auto jid : order.job_ids)
    w.write_uint32(jid);
  w.end_array();

  // pricing_entries array
  w.key("pricing_entries");
  w.begin_array();
  for (const auto& entry : order.pricing_entries)
  {
    w.begin_object();
    w.key("job_id");              w.write_uint32(entry.job_id);
    w.key("total_pages");         w.write_int(entry.total_pages);
    w.key("copies");              w.write_int(entry.copies);
    w.key("effective_sheets");    w.write_int(entry.effective_sheets);
    w.key("duplex_full_sheets");  w.write_int(entry.duplex_full_sheets);
    w.key("simplex_tail_pages");  w.write_int(entry.simplex_tail_pages);
    w.key("matched_rule_id");     w.write_string(entry.matched_rule_id);
    w.key("base_price");          w.write_double(entry.base_price);
    w.key("total_price");         w.write_double(entry.total_price);
    w.key("price_pending");       w.write_bool(entry.price_pending);
    w.key("pending_reason");      w.write_string(entry.pending_reason);
    w.end_object();
  }
  w.end_array();

  // pricing_snapshot_json embedded as a raw string
  w.key("pricing_snapshot_json");
  w.write_string(order.pricing_snapshot_json);

  w.end_object();
  out << '\n';
  return out.str();
}

std::optional<core::SettlementOrder> SettlementBridge::fromJson(
    const std::string& json)
{
  try
  {
    std::istringstream in(json);
    JsonReader r(in);

    core::SettlementOrder order;
    r.begin_object();
    while (r.end_object_or_next())
    {
      const std::string key = r.read_key();

      if (key == "schema_version")       order.schema_version = r.read_int();
      else if (key == "order_id")         order.order_id = r.read_string();
      else if (key == "task_id")          order.task_id = r.read_string();
      else if (key == "user_id")          order.user_id = r.read_string();
      else if (key == "settled_by")       order.settled_by = r.read_string();
      else if (key == "settled_at_utc")   order.settled_at_utc = r.read_string();
      else if (key == "period_start_utc") order.period_start_utc = r.read_string();
      else if (key == "period_end_utc")   order.period_end_utc = r.read_string();
      else if (key == "currency")         order.currency = r.read_string();
      else if (key == "total_price")      order.total_price = r.read_number();
      else if (key == "pricing_snapshot_json") order.pricing_snapshot_json = r.read_string();
      else if (key == "job_ids")
      {
        r.begin_array();
        while (r.end_array_or_next())
          order.job_ids.push_back(r.read_uint32());
      }
      else if (key == "pricing_entries")
      {
        r.begin_array();
        while (r.end_array_or_next())
        {
          core::JobPricingEntry entry;
          r.begin_object();
          while (r.end_object_or_next())
          {
            const std::string ek = r.read_key();
            if (ek == "job_id")              entry.job_id = r.read_uint32();
            else if (ek == "total_pages")         entry.total_pages = r.read_int();
            else if (ek == "copies")              entry.copies = r.read_int();
            else if (ek == "effective_sheets")    entry.effective_sheets = r.read_int();
            else if (ek == "duplex_full_sheets")  entry.duplex_full_sheets = r.read_int();
            else if (ek == "simplex_tail_pages")  entry.simplex_tail_pages = r.read_int();
            else if (ek == "matched_rule_id")     entry.matched_rule_id = r.read_string();
            else if (ek == "base_price")          entry.base_price = r.read_number();
            else if (ek == "total_price")         entry.total_price = r.read_number();
            else if (ek == "price_pending")       entry.price_pending = r.read_bool();
            else if (ek == "pending_reason")      entry.pending_reason = r.read_string();
            else (void)r.read_string();
          }
          order.pricing_entries.push_back(std::move(entry));
        }
      }
      else { (void)r.read_string(); }
    }
    return order;
  }
  catch (...) { return std::nullopt; }
}

bool SettlementBridge::saveToFile(
    const core::SettlementOrder& order,
    const std::filesystem::path& output_dir)
{
  try
  {
    const auto orders_dir = output_dir / "settlement_orders";
    std::filesystem::create_directories(orders_dir);

    const auto file_path = orders_dir / (order.order_id + ".json");
    const auto temp_path = file_path.string() + ".tmp";

    {
      std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      out << toJson(order);
      if (!out) return false;
    }

    std::error_code ec;
    std::filesystem::rename(temp_path, file_path, ec);
    if (ec)
    {
      std::filesystem::copy_file(temp_path, file_path,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) return false;
      std::filesystem::remove(temp_path, ec);
    }
    return true;
  }
  catch (...) { return false; }
}

std::optional<core::SettlementOrder> SettlementBridge::loadFromFile(
    const std::filesystem::path& file_path)
{
  try
  {
    std::ifstream in(file_path, std::ios::binary);
    if (!in) return std::nullopt;

    std::ostringstream oss;
    oss << in.rdbuf();
    return fromJson(oss.str());
  }
  catch (...) { return std::nullopt; }
}

}  // namespace wpce::bridge
