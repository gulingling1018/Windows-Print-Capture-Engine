/// @file billing_batch_repository.cpp
/// BillingBatchRepository implementation.

#include "domain/billing_batch_repository.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace wpce::domain
{

namespace
{

class JsonWriter
{
 public:
  explicit JsonWriter(std::ostream& out) : out_(out) {}
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
  void writeInt(int v) { beforeValue(); out_ << v; }
  void writeUInt32(std::uint32_t v) { beforeValue(); out_ << v; }
  void writeDouble(double v) { beforeValue(); char buf[64]; std::snprintf(buf, sizeof(buf), "%.4f", v); out_ << buf; }
  void writeBool(bool v) { beforeValue(); out_ << (v ? "true" : "false"); }
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

class JsonReader
{
 public:
  explicit JsonReader(std::istream& in) : in_(in) { skipWs(); }
  void beginObject() { expect('{'); skipWs(); stack_.push_back({true, true}); }
  bool endObjectOrNext()
  {
    skipWs();
    if (peek() == '}') { get(); if (!stack_.empty()) stack_.pop_back(); return false; }
    if (!stack_.empty() && stack_.back().first) { stack_.back().first = false; return true; }
    expect(','); return true;
  }
  void beginArray() { expect('['); skipWs(); stack_.push_back({false, true}); }
  bool endArrayOrNext()
  {
    skipWs();
    if (peek() == ']') { get(); if (!stack_.empty()) stack_.pop_back(); return false; }
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
  double readNumber() { std::string t; while (std::isdigit(peek())||peek()=='-'||peek()=='+'||peek()=='.') t+=static_cast<char>(get()); skipWs(); return std::stod(t); }
  int readInt() { return static_cast<int>(readNumber()); }
  std::uint32_t readUInt32() { return static_cast<std::uint32_t>(readNumber()); }
  bool readBool() {
    if (peek()=='t') { for(int i=0;i<4;++i)get(); skipWs(); return true; }
    for(int i=0;i<5;++i)get(); skipWs(); return false;
  }
 private:
  struct Ctx { bool is_object; bool first; };
  std::vector<Ctx> stack_;
  std::istream& in_;
  int peek() const { return in_.peek(); }
  int get() { int c=in_.get(); if(c==std::char_traits<char>::eof()) throw std::runtime_error("unexpected end of JSON"); return c; }
  void expect(char e) { int c=get(); if(c!=e) throw std::runtime_error("expected different char"); }
  void skipWs() { while(std::isspace(peek())) get(); }
};

BatchState parseState(const std::string& s)
{
  if (s == "active")    return BatchState::Active;
  if (s == "suspended") return BatchState::Suspended;
  if (s == "settled")   return BatchState::Settled;
  if (s == "voided")    return BatchState::Voided;
  return BatchState::Active;
}

void writeAnchor(JsonWriter& w, const std::string& key, const BatchDocAnchor& a)
{
  w.key(key);
  w.beginObject();
  w.key("job_id");         w.writeUInt32(a.job_id);
  w.key("document_name");  w.writeString(a.document_name);
  w.key("submit_time_utc"); w.writeString(a.submit_time_utc);
  w.key("page_count");     w.writeInt(a.page_count);
  w.endObject();
}

void readAnchor(JsonReader& r, BatchDocAnchor& a)
{
  r.beginObject();
  while (r.endObjectOrNext())
  {
    const std::string k = r.readKey();
    if (k == "job_id")          a.job_id = r.readUInt32();
    else if (k == "document_name") a.document_name = r.readString();
    else if (k == "submit_time_utc") a.submit_time_utc = r.readString();
    else if (k == "page_count") a.page_count = r.readInt();
    else (void)r.readString();
  }
}

}  // namespace

void BillingBatchRepository::setFilePath(const std::filesystem::path& path)
{
  path_ = path;
}

bool BillingBatchRepository::load(std::vector<BillingBatch>& out)
{
  std::string ignored_date;
  return load(out, ignored_date);
}

bool BillingBatchRepository::load(std::vector<BillingBatch>& out, std::string& saved_date)
{
  out.clear();
  saved_date.clear();
  if (path_.empty()) return false;
  try
  {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return true;  // missing file = empty list is OK
    JsonReader r(in);
    r.beginObject();
    while (r.endObjectOrNext())
    {
      const std::string key = r.readKey();
      if (key == "date")
      {
        saved_date = r.readString();
      }
      else if (key == "batches")
      {
        r.beginArray();
        while (r.endArrayOrNext())
        {
          BillingBatch batch;
          r.beginObject();
          while (r.endObjectOrNext())
          {
            const std::string k = r.readKey();
            if (k == "batch_id")            batch.batch_id = r.readString();
            else if (k == "display_name")   batch.display_name = r.readString();
            else if (k == "source_user")    batch.source_user = r.readString();
            else if (k == "state")          batch.state = parseState(r.readString());
            else if (k == "period_start_utc") batch.period_start_utc = r.readString();
            else if (k == "period_end_utc") batch.period_end_utc = r.readString();
            else if (k == "suspend_time_utc") batch.suspend_time_utc = r.readString();
            else if (k == "settle_order_id") batch.settle_order_id = r.readString();
            else if (k == "total_pages")    batch.total_pages = r.readInt();
            else if (k == "simplex_pages")  batch.simplex_pages = r.readInt();
            else if (k == "duplex_pages")   batch.duplex_pages = r.readInt();
            else if (k == "document_count") batch.document_count = r.readInt();
            else if (k == "total_price")    batch.total_price = r.readNumber();
            else if (k == "pending_count")  batch.pending_count = r.readInt();
            else if (k == "first_doc_anchor") readAnchor(r, batch.first_doc_anchor);
            else if (k == "last_doc_anchor")  readAnchor(r, batch.last_doc_anchor);
            else if (k == "job_ids")
            {
              r.beginArray();
              while (r.endArrayOrNext()) batch.job_ids.push_back(r.readUInt32());
            }
            else (void)r.readString();
          }
          out.push_back(std::move(batch));
        }
      }
      else (void)r.readString();
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool BillingBatchRepository::save(const std::vector<BillingBatch>& batches) const
{
  return save(batches, std::string{});
}

bool BillingBatchRepository::save(const std::vector<BillingBatch>& batches,
                                   const std::string& date_yyyy_mm_dd) const
{
  if (path_.empty()) return false;
  try
  {
    std::filesystem::create_directories(path_.parent_path());
    const auto temp = path_.string() + ".tmp";
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      JsonWriter w(out);
      w.beginObject();
      w.key("schema_version"); w.writeInt(2);
      if (!date_yyyy_mm_dd.empty())
      {
        w.key("date"); w.writeString(date_yyyy_mm_dd);
      }
      w.key("batches");
      w.beginArray();
      for (const auto& batch : batches)
      {
        w.beginObject();
        w.key("batch_id");            w.writeString(batch.batch_id);
        w.key("display_name");        w.writeString(batch.display_name);
        w.key("source_user");         w.writeString(batch.source_user);
        w.key("state");               w.writeString(std::string(to_string(batch.state)));
        w.key("period_start_utc");    w.writeString(batch.period_start_utc);
        w.key("period_end_utc");      w.writeString(batch.period_end_utc);
        w.key("suspend_time_utc");    w.writeString(batch.suspend_time_utc);
        w.key("settle_order_id");     w.writeString(batch.settle_order_id);
        w.key("total_pages");         w.writeInt(batch.total_pages);
        w.key("simplex_pages");       w.writeInt(batch.simplex_pages);
        w.key("duplex_pages");        w.writeInt(batch.duplex_pages);
        w.key("document_count");      w.writeInt(batch.document_count);
        w.key("total_price");         w.writeDouble(batch.total_price);
        w.key("pending_count");       w.writeInt(batch.pending_count);
        writeAnchor(w, "first_doc_anchor", batch.first_doc_anchor);
        writeAnchor(w, "last_doc_anchor", batch.last_doc_anchor);
        w.key("job_ids");
        w.beginArray();
        for (auto jid : batch.job_ids) w.writeUInt32(jid);
        w.endArray();
        w.endObject();
      }
      w.endArray();
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

}  // namespace wpce::domain
