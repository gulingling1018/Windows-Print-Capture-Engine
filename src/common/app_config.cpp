#include "common/app_config.h"
#include "common/pricing_engine.h"

#ifdef _WIN32
#include <windows.h>
#include <ShlObj.h>
#include <combaseapi.h>
#endif

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace wpce::common
{

// ── helpers ─────────────────────────────────────────────────────────────────

namespace
{

#ifdef _WIN32
std::filesystem::path get_exe_directory()
{
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD len = 0;
  for (;;)
  {
    len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0)
    {
      return std::filesystem::current_path();
    }
    if (len < static_cast<DWORD>(buffer.size()))
    {
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  buffer.resize(len);
  return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path get_appdata_directory()
{
  PWSTR roaming = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming != nullptr)
  {
    std::filesystem::path result(roaming);
    CoTaskMemFree(roaming);
    return result / "WPCE";
  }
  // Fallback: %APPDATA%/WPCE
  char* appdata = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata != nullptr)
  {
    std::filesystem::path result(appdata);
    free(appdata);
    return result / "WPCE";
  }
  return std::filesystem::path(".") / "WPCE";
}
#else
std::filesystem::path get_exe_directory()
{
  return std::filesystem::current_path();
}

std::filesystem::path get_appdata_directory()
{
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : ".") / ".wpce";
}
#endif

/// Split a pipe-separated string like "A4|B5" into a lowercased vector.
/// Empty input → empty vector.  Used to backfill the set fields of
/// PricingRule from older config files that only stored the pipe string.
std::vector<std::string> split_pipe_lower(const std::string& s)
{
  std::vector<std::string> out;
  std::string cur;
  for (char c : s)
  {
    if (c == '|')
    {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    }
    else
    {
      cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// ── Minimal JSON writer ─────────────────────────────────────────────────────

class JsonWriter
{
 public:
  explicit JsonWriter(std::ostream& out) : out_(out) {}

  void begin_object()
  {
    before_value();
    out_ << '{';
    stack_.push_back(Context{.object = true});
  }
  void end_object()
  {
    out_ << '}';
    if (!stack_.empty()) stack_.pop_back();
  }

  void begin_array()
  {
    before_value();
    out_ << '[';
    stack_.push_back(Context{.object = false});
  }
  void end_array()
  {
    out_ << ']';
    if (!stack_.empty()) stack_.pop_back();
  }

  void key(const std::string& k)
  {
    if (stack_.empty() || !stack_.back().object)
    {
      return;
    }

    Context& ctx = stack_.back();
    if (!ctx.first) out_ << ',';
    ctx.first = false;
    ctx.expecting_value = true;
    out_ << '"' << escape(k) << "\":";
  }

  void write_null()
  {
    before_value();
    out_ << "null";
  }
  void write_bool(bool v)
  {
    before_value();
    out_ << (v ? "true" : "false");
  }
  void write_int(int v)
  {
    before_value();
    out_ << v;
  }
  void write_uint(std::size_t v)
  {
    before_value();
    out_ << v;
  }
  void write_double(double v)
  {
    before_value();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    out_ << buf;
  }
  void write_string(const std::string& s)
  {
    before_value();
    out_ << '"' << escape(s) << '"';
  }

 private:
  struct Context
  {
    bool object{false};
    bool first{true};
    bool expecting_value{false};
  };

  void before_value()
  {
    if (stack_.empty())
    {
      return;
    }

    Context& ctx = stack_.back();
    if (ctx.object)
    {
      if (ctx.expecting_value)
      {
        ctx.expecting_value = false;
      }
      return;
    }

    if (!ctx.first) out_ << ',';
    ctx.first = false;
  }

  static std::string escape(const std::string& s)
  {
    std::string result;
    result.reserve(s.size());
    for (char ch : s)
    {
      switch (ch)
      {
        case '"':  result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:   result += ch; break;
      }
    }
    return result;
  }

  std::ostream& out_;
  std::vector<Context> stack_;
};

// ── Minimal JSON reader ─────────────────────────────────────────────────────

/// Ultra-minimal recursive-descent JSON parser.  Only supports the subset we
/// actually emit in the config file (objects, arrays, strings, numbers, bool,
/// null).  No unicode escapes, no exponential notation, no tolerance for
/// trailing commas or comments.
class JsonReader
{
 public:
  explicit JsonReader(std::istream& in) : in_(in) { skip_ws(); }

  bool is_object() const { return peek() == '{'; }
  bool is_array() const { return peek() == '['; }
  bool is_string() const { return peek() == '"'; }

  void begin_object()
  {
    expect('{');
    skip_ws();
    stack_.push_back(Context{.object = true});
  }
  bool end_object_or_next()
  {
    skip_ws();
    if (peek() == '}')
    {
      get();
      if (!stack_.empty()) stack_.pop_back();
      return false; // end of object
    }

    if (!stack_.empty() && stack_.back().first)
    {
      stack_.back().first = false;
      return true;
    }

    expect(',');
    return true; // more entries
  }

  void begin_array()
  {
    expect('[');
    skip_ws();
    stack_.push_back(Context{.object = false});
  }
  bool end_array_or_next()
  {
    skip_ws();
    if (peek() == ']')
    {
      get();
      if (!stack_.empty()) stack_.pop_back();
      return false; // end of array
    }

    if (!stack_.empty() && stack_.back().first)
    {
      stack_.back().first = false;
      return true;
    }

    expect(',');
    return true; // more elements
  }

  std::string read_key()
  {
    std::string k = read_string();
    skip_ws();
    expect(':');
    skip_ws();
    return k;
  }

  std::string read_string()
  {
    expect('"');
    std::string result;
    for (;;)
    {
      int ch = get();
      if (ch == '"') break;
      if (ch == '\\')
      {
        ch = get();
        switch (ch)
        {
          case '"':  result += '"'; break;
          case '\\': result += '\\'; break;
          case 'n':  result += '\n'; break;
          case 'r':  result += '\r'; break;
          case 't':  result += '\t'; break;
          default:   result.push_back(static_cast<char>(ch)); break;
        }
      }
      else
      {
        result += static_cast<char>(ch);
      }
    }
    skip_ws();
    return result;
  }

  double read_number()
  {
    std::string token;
    // Gather chars that belong to a number.
    while (std::isdigit(peek()) || peek() == '-' || peek() == '+' || peek() == '.')
    {
      token += static_cast<char>(get());
    }
    skip_ws();
    return std::stod(token);
  }

  int read_int()
  {
    return static_cast<int>(read_number());
  }

  bool read_bool()
  {
    if (peek() == 't')
    {
      for (int i = 0; i < 4; ++i) get();
      skip_ws();
      return true;
    }
    // "false"
    for (int i = 0; i < 5; ++i) get();
    skip_ws();
    return false;
  }

  void read_null()
  {
    for (int i = 0; i < 4; ++i) get(); // "null"
    skip_ws();
  }

  // Peek at current value type.
  int value_type() const { return peek(); }

 private:
  struct Context
  {
    bool object{false};
    bool first{true};
  };

  int peek() const { return in_.peek(); }

  int get()
  {
    int ch = in_.get();
    if (ch == std::char_traits<char>::eof())
      throw std::runtime_error("unexpected end of JSON input");
    return ch;
  }

  void expect(char expected)
  {
    int ch = get();
    if (ch != expected)
    {
      char msg[64];
      std::snprintf(msg, sizeof(msg), "expected '%c' but got '%c'", expected, static_cast<char>(ch));
      throw std::runtime_error(msg);
    }
  }

  void skip_ws()
  {
    while (std::isspace(peek())) get();
  }

  std::istream& in_;
  std::vector<Context> stack_;
};

}  // namespace

// ── AppConfig ───────────────────────────────────────────────────────────────

AppConfig AppConfig::defaults()
{
  AppConfig config;

  // Storage defaults
  config.storage.output_root = "./data";
  config.storage.use_timestamp_subdir = true;
  config.storage.queue_capacity = 256;
  config.storage.retention_days = 90;

  // Pricing defaults — matrix rules (factory preset of 6 default rules).
  // Legacy additive fields have been removed; rules are the single
  // source of pricing truth.
  config.pricing.currency = "CNY";
  config.pricing.color_price_pending = true;
  config.pricing.rules.clear();

  auto addRule = [&](std::string id, std::string paper, std::string duplex,
                     std::string color, std::string ext, double price)
  {
    core::PricingRule r;
    r.rule_id = std::move(id);
    r.paper_size = paper;
    r.duplex_mode = duplex;
    r.color_mode = color;
    r.file_extension = ext;
    r.price_per_sheet = price;
    // Expand multi-value pipe strings into the set fields.
    {
      std::string lower;
      lower.reserve(paper.size());
      for (char c : paper) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string cur;
      for (char c : lower)
      {
        if (c == '|') { if (!cur.empty()) { r.paper_size_set.push_back(cur); cur.clear(); } }
        else cur += c;
      }
      if (!cur.empty()) r.paper_size_set.push_back(cur);
    }
    {
      std::string lower;
      lower.reserve(ext.size());
      for (char c : ext) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string cur;
      for (char c : lower)
      {
        if (c == '|') { if (!cur.empty()) { r.file_extension_set.push_back(cur); cur.clear(); } }
        else cur += c;
      }
      if (!cur.empty()) r.file_extension_set.push_back(cur);
    }
    r.priority = rule_specificity(r);
    config.pricing.rules.push_back(std::move(r));
  };

  // The 6 factory default rules per project spec.
  // paper|duplex|color|filetype|price  (empty ext = all file types)
  addRule("default-1", "A4|B5", "simplex", "monochrome", "",      0.15);
  addRule("default-2", "A4|B5", "duplex",  "monochrome", "",      0.25);
  addRule("default-3", "A3",    "simplex", "monochrome", "",      0.50);
  addRule("default-4", "A3",    "duplex",  "monochrome", "",      1.00);
  addRule("default-5", "A3",    "simplex", "monochrome", "dwg",   1.00);
  addRule("default-6", "A4|B5", "simplex", "monochrome", "dwg",   0.50);

  sort_rules_by_priority(config.pricing.rules);

  // Printer capability defaults — empty; first-launch detection populates.
  config.printers.profiles.clear();
  config.printers.default_color_capable = false;

  // Merchant defaults (empty)
  config.merchant.name.clear();
  config.merchant.phone.clear();
  config.merchant.address.clear();

  // Monitoring defaults
  config.monitoring.stall_timeout_seconds = 30;
  config.monitoring.perf_log_enabled = false;

  // Window defaults
  config.window.width = 1280;
  config.window.height = 760;

  return config;
}

std::filesystem::path AppConfig::default_config_path()
{
  // 1. Portable: next to executable.
  const std::filesystem::path exe_dir = get_exe_directory();
  const std::filesystem::path portable_path = exe_dir / "wpce_config.json";

  if (std::filesystem::exists(portable_path))
  {
    return portable_path;
  }

  // 2. Per-user: %APPDATA%/WPCE/wpce_config.json
  const std::filesystem::path appdata_path = get_appdata_directory() / "wpce_config.json";
  if (std::filesystem::exists(appdata_path))
  {
    return appdata_path;
  }

  // Prefer portable location for new files.
  return portable_path;
}

bool AppConfig::ensure_config_file(const std::filesystem::path& path)
{
  if (std::filesystem::exists(path))
  {
    return true;
  }

  try
  {
    std::filesystem::create_directories(path.parent_path());
    AppConfig::defaults().save_to_json(path);
    return std::filesystem::exists(path);
  }
  catch (...)
  {
    return false;
  }
}

// ── Serialisation ───────────────────────────────────────────────────────────

bool AppConfig::save_to_json(const std::filesystem::path& path) const
{
  try
  {
    std::filesystem::create_directories(path.parent_path());

    // Write to temp file then rename atomically.
    const auto temp_path = path.string() + ".tmp";
    {
      std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
      if (!out) return false;

      JsonWriter w(out);

      w.begin_object();
      w.key("version");
      w.write_int(1);

      // storage
      w.key("storage");
      w.begin_object();
      w.key("output_root");
      w.write_string(storage.output_root);
      w.key("use_timestamp_subdir");
      w.write_bool(storage.use_timestamp_subdir);
      w.key("queue_capacity");
      w.write_uint(storage.queue_capacity);
      w.key("retention_days");
      w.write_int(storage.retention_days);
      w.end_object();

      // pricing
      w.key("pricing");
      w.begin_object();
      w.key("currency");
      w.write_string(pricing.currency);
      w.key("color_price_pending");
      w.write_bool(pricing.color_price_pending);

      w.key("rules");
      w.begin_array();
      for (const auto& rule : pricing.rules)
      {
        w.begin_object();
        w.key("rule_id");
        w.write_string(rule.rule_id);
        w.key("paper_size");
        w.write_string(rule.paper_size);
        w.key("duplex_mode");
        w.write_string(rule.duplex_mode);
        w.key("color_mode");
        w.write_string(rule.color_mode);
        w.key("file_extension");
        w.write_string(rule.file_extension);
        w.key("price_per_sheet");
        w.write_double(rule.price_per_sheet);

        // Multi-value expansions (authoritative for matching).
        w.key("paper_size_set");
        w.begin_array();
        for (const auto& s : rule.paper_size_set) w.write_string(s);
        w.end_array();

        w.key("file_extension_set");
        w.begin_array();
        for (const auto& s : rule.file_extension_set) w.write_string(s);
        w.end_array();

        w.end_object();
      }
      w.end_array();
      w.end_object();

      // printers (color capability profiles)
      w.key("printers");
      w.begin_object();
      w.key("default_color_capable");
      w.write_bool(printers.default_color_capable);
      w.key("profiles");
      w.begin_array();
      for (const auto& p : printers.profiles)
      {
        w.begin_object();
        w.key("printer_name");
        w.write_string(p.printer_name);
        w.key("is_color_capable");
        w.write_bool(p.is_color_capable);
        w.end_object();
      }
      w.end_array();
      w.end_object();

      // merchant
      w.key("merchant");
      w.begin_object();
      w.key("name");
      w.write_string(merchant.name);
      w.key("phone");
      w.write_string(merchant.phone);
      w.key("address");
      w.write_string(merchant.address);
      w.end_object();

      // monitoring
      w.key("monitoring");
      w.begin_object();
      w.key("stall_timeout_seconds");
      w.write_int(monitoring.stall_timeout_seconds);
      w.key("perf_log_enabled");
      w.write_bool(monitoring.perf_log_enabled);
      w.end_object();

      // window
      w.key("window");
      w.begin_object();
      w.key("width");
      w.write_int(window.width);
      w.key("height");
      w.write_int(window.height);
      w.end_object();

      w.end_object();
      out << '\n';
    }

    // Atomic rename.
    std::error_code ec;
    std::filesystem::rename(temp_path, path, ec);
    if (ec)
    {
      // Fallback: copy + remove
      std::filesystem::copy_file(temp_path, path,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) return false;
      std::filesystem::remove(temp_path, ec);
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool AppConfig::load_from_json(const std::filesystem::path& path)
{
  try
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    JsonReader r(in);
    r.begin_object();

    while (r.end_object_or_next())
    {
      const std::string key = r.read_key();

      if (key == "version")
      {
        /*int version =*/ r.read_int();
      }
      else if (key == "storage")
      {
        r.begin_object();
        while (r.end_object_or_next())
        {
          const std::string sk = r.read_key();
          if (sk == "output_root")            storage.output_root = r.read_string();
          else if (sk == "use_timestamp_subdir") storage.use_timestamp_subdir = r.read_bool();
          else if (sk == "queue_capacity")    storage.queue_capacity = static_cast<std::size_t>(r.read_int());
          else if (sk == "retention_days")    storage.retention_days = r.read_int();
          else (void)r.read_string(); // skip unknown
        }
      }
      else if (key == "pricing")
      {
        r.begin_object();
        while (r.end_object_or_next())
        {
          const std::string pk = r.read_key();
          if (pk == "currency")                       pricing.currency = r.read_string();
          else if (pk == "color_price_pending")       pricing.color_price_pending = r.read_bool();
          // Legacy additive fields (silently ignored for forward-compat).
          else if (pk == "single_sided_per_page")     (void)r.read_number();
          else if (pk == "double_sided_per_sheet")    (void)r.read_number();
          else if (pk == "color_surcharge")           (void)r.read_number();
          else if (pk == "large_paper_surcharge")     (void)r.read_number();
          else if (pk == "high_quality_surcharge")    (void)r.read_number();
          else if (pk == "volume_discounts")
          {
            r.begin_array();
            while (r.end_array_or_next())
            {
              r.begin_object();
              while (r.end_object_or_next()) (void)r.read_string();
            }
          }
          else if (pk == "rules")
          {
            pricing.rules.clear();
            r.begin_array();
            while (r.end_array_or_next())
            {
              core::PricingRule rule;
              r.begin_object();
              while (r.end_object_or_next())
              {
                const std::string rk = r.read_key();
                if (rk == "rule_id")              rule.rule_id = r.read_string();
                else if (rk == "paper_size")      rule.paper_size = r.read_string();
                else if (rk == "duplex_mode")     rule.duplex_mode = r.read_string();
                else if (rk == "color_mode")      rule.color_mode = r.read_string();
                else if (rk == "file_extension")  rule.file_extension = r.read_string();
                else if (rk == "price_per_sheet") rule.price_per_sheet = r.read_number();
                else if (rk == "paper_size_set")
                {
                  r.begin_array();
                  while (r.end_array_or_next()) rule.paper_size_set.push_back(r.read_string());
                }
                else if (rk == "file_extension_set")
                {
                  r.begin_array();
                  while (r.end_array_or_next()) rule.file_extension_set.push_back(r.read_string());
                }
                else (void)r.read_string();
              }
              // Backfill set fields from the pipe-string form when the
              // set was missing (e.g. older config files).
              if (rule.paper_size_set.empty() && !rule.paper_size.empty())
                rule.paper_size_set = split_pipe_lower(rule.paper_size);
              if (rule.file_extension_set.empty() && !rule.file_extension.empty())
                rule.file_extension_set = split_pipe_lower(rule.file_extension);
              rule.priority = rule_specificity(rule);
              pricing.rules.push_back(std::move(rule));
            }
            sort_rules_by_priority(pricing.rules);
          }
          else (void)r.read_string(); // skip unknown
        }
      }
      else if (key == "printers")
      {
        r.begin_object();
        while (r.end_object_or_next())
        {
          const std::string pk = r.read_key();
          if (pk == "default_color_capable")  printers.default_color_capable = r.read_bool();
          else if (pk == "profiles")
          {
            printers.profiles.clear();
            r.begin_array();
            while (r.end_array_or_next())
            {
              PrinterProfile profile;
              r.begin_object();
              while (r.end_object_or_next())
              {
                const std::string ppk = r.read_key();
                if (ppk == "printer_name")        profile.printer_name = r.read_string();
                else if (ppk == "is_color_capable") profile.is_color_capable = r.read_bool();
                else (void)r.read_string();
              }
              printers.profiles.push_back(std::move(profile));
            }
          }
          else (void)r.read_string();
        }
      }
      else if (key == "merchant")
      {
        r.begin_object();
        while (r.end_object_or_next())
        {
          const std::string mk = r.read_key();
          if (mk == "name")       merchant.name = r.read_string();
          else if (mk == "phone") merchant.phone = r.read_string();
          else if (mk == "address") merchant.address = r.read_string();
          else (void)r.read_string();
        }
      }
      else if (key == "monitoring")
      {
        r.begin_object();
        while (r.end_object_or_next())
        {
          const std::string mk = r.read_key();
          if (mk == "stall_timeout_seconds") monitoring.stall_timeout_seconds = r.read_int();
          else if (mk == "perf_log_enabled") monitoring.perf_log_enabled = r.read_bool();
          else (void)r.read_string();
        }
      }
      else if (key == "window")
      {
        r.begin_object();
        while (r.end_object_or_next())
        {
          const std::string wk = r.read_key();
          if (wk == "width")       window.width = r.read_int();
          else if (wk == "height") window.height = r.read_int();
          else (void)r.read_string();
        }
      }
      else
      {
        (void)r.read_string(); // skip unknown top-level key
      }
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

}  // namespace wpce::common
