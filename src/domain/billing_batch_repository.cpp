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

// ── PrintJobRecord serialisation helpers ─────────────────────────────

void writeFieldValue(JsonWriter& w, const std::string& key,
                     const core::FieldValue& fv)
{
  if (fv.value.empty() && fv.state == core::FieldState::Unknown) return;
  w.key(key);
  w.beginObject();
  if (!fv.value.empty())
  {
    w.key("value"); w.writeString(fv.value);
  }
  w.key("state"); w.writeString(std::string(core::to_string(fv.state)));
  w.endObject();
}

void readFieldValue(JsonReader& r, core::FieldValue& fv)
{
  r.beginObject();
  while (r.endObjectOrNext())
  {
    const std::string k = r.readKey();
    if (k == "value")       fv.value = r.readString();
    else if (k == "state")  fv.state = core::FieldState::Reported; // default
    else (void)r.readString();
  }
}

void writeJobSnapshot(JsonWriter& w, const core::JobSnapshot& s)
{
  w.key("snapshot");
  w.beginObject();
  w.key("schema_version");  w.writeInt(s.schema_version);
  w.key("job_id");          w.writeUInt32(s.job_id);
  w.key("user");            w.writeString(s.user);
  w.key("document_name");   w.writeString(s.document_name);
  w.key("printer_name");    w.writeString(s.printer_name);
  w.key("submit_time_utc"); w.writeString(s.submit_time_utc);
  w.key("status");          w.writeString(s.status);
  w.key("raw_status_flags"); w.writeInt(static_cast<int>(s.raw_status_flags));
  w.key("source");          w.writeString(s.source);
  w.endObject();
}

void readJobSnapshot(JsonReader& r, core::JobSnapshot& s)
{
  r.beginObject();
  while (r.endObjectOrNext())
  {
    const std::string k = r.readKey();
    if (k == "schema_version")   s.schema_version = r.readInt();
    else if (k == "job_id")      s.job_id = r.readUInt32();
    else if (k == "user")        s.user = r.readString();
    else if (k == "document_name") s.document_name = r.readString();
    else if (k == "printer_name")  s.printer_name = r.readString();
    else if (k == "submit_time_utc") s.submit_time_utc = r.readString();
    else if (k == "status")      s.status = r.readString();
    else if (k == "raw_status_flags") s.raw_status_flags = static_cast<std::uint32_t>(r.readInt());
    else if (k == "source")      s.source = r.readString();
    else (void)r.readString();
  }
}

void writeMetadataSnapshot(JsonWriter& w, const core::MetadataSnapshot& m)
{
  w.key("metadata");
  w.beginObject();
  w.key("schema_version"); w.writeInt(m.schema_version);
  w.key("job_id");         w.writeUInt32(m.job_id);
  if (!m.printer_name.empty())
  { w.key("printer_name"); w.writeString(m.printer_name); }
  writeFieldValue(w, "total_pages", m.total_pages);
  writeFieldValue(w, "pages_printed", m.pages_printed);
  writeFieldValue(w, "copies", m.copies);
  writeFieldValue(w, "duplex", m.duplex);
  writeFieldValue(w, "color", m.color);
  writeFieldValue(w, "paper_size", m.paper_size);
  writeFieldValue(w, "orientation", m.orientation);
  writeFieldValue(w, "n_up", m.n_up);
  writeFieldValue(w, "collate", m.collate);
  writeFieldValue(w, "print_quality", m.print_quality);
  writeFieldValue(w, "media_type", m.media_type);
  writeFieldValue(w, "page_range", m.page_range);
  writeFieldValue(w, "file_extension", m.file_extension);
  if (!m.dm_fields_hex.empty())
  { w.key("dm_fields_hex"); w.writeString(m.dm_fields_hex); }
  w.endObject();
}

void readMetadataField(JsonReader& r, core::MetadataSnapshot& m,
                       const std::string& k)
{
  if (k == "schema_version")      m.schema_version = r.readInt();
  else if (k == "job_id")         m.job_id = r.readUInt32();
  else if (k == "printer_name")   m.printer_name = r.readString();
  else if (k == "dm_fields_hex")  m.dm_fields_hex = r.readString();
  else if (k == "total_pages")    readFieldValue(r, m.total_pages);
  else if (k == "pages_printed")  readFieldValue(r, m.pages_printed);
  else if (k == "copies")         readFieldValue(r, m.copies);
  else if (k == "duplex")         readFieldValue(r, m.duplex);
  else if (k == "color")          readFieldValue(r, m.color);
  else if (k == "paper_size")     readFieldValue(r, m.paper_size);
  else if (k == "orientation")    readFieldValue(r, m.orientation);
  else if (k == "n_up")           readFieldValue(r, m.n_up);
  else if (k == "collate")        readFieldValue(r, m.collate);
  else if (k == "print_quality")  readFieldValue(r, m.print_quality);
  else if (k == "media_type")     readFieldValue(r, m.media_type);
  else if (k == "page_range")     readFieldValue(r, m.page_range);
  else if (k == "file_extension") readFieldValue(r, m.file_extension);
  else (void)r.readString();
}

void writeJobPricing(JsonWriter& w, const core::JobPricingEntry& p)
{
  w.key("pricing");
  w.beginObject();
  w.key("job_id");             w.writeUInt32(p.job_id);
  w.key("total_pages");        w.writeInt(p.total_pages);
  w.key("copies");             w.writeInt(p.copies);
  w.key("effective_sheets");   w.writeInt(p.effective_sheets);
  w.key("duplex_full_sheets"); w.writeInt(p.duplex_full_sheets);
  w.key("simplex_tail_pages"); w.writeInt(p.simplex_tail_pages);
  w.key("matched_rule_id");    w.writeString(p.matched_rule_id);
  w.key("tail_rule_id");       w.writeString(p.tail_rule_id);
  w.key("base_price");         w.writeDouble(p.base_price);
  w.key("tail_price");         w.writeDouble(p.tail_price);
  w.key("total_price");        w.writeDouble(p.total_price);
  w.key("price_pending");      w.writeBool(p.price_pending);
  if (!p.pending_reason.empty())
  { w.key("pending_reason");   w.writeString(p.pending_reason); }
  w.key("color_downgraded");   w.writeBool(p.color_downgraded);
  if (!p.note.empty())
  { w.key("note");             w.writeString(p.note); }
  w.endObject();
}

void readJobPricing(JsonReader& r, core::JobPricingEntry& p)
{
  r.beginObject();
  while (r.endObjectOrNext())
  {
    const std::string k = r.readKey();
    if (k == "job_id")              p.job_id = r.readUInt32();
    else if (k == "total_pages")    p.total_pages = r.readInt();
    else if (k == "copies")         p.copies = r.readInt();
    else if (k == "effective_sheets")   p.effective_sheets = r.readInt();
    else if (k == "duplex_full_sheets") p.duplex_full_sheets = r.readInt();
    else if (k == "simplex_tail_pages") p.simplex_tail_pages = r.readInt();
    else if (k == "matched_rule_id") p.matched_rule_id = r.readString();
    else if (k == "tail_rule_id")   p.tail_rule_id = r.readString();
    else if (k == "base_price")     p.base_price = r.readNumber();
    else if (k == "tail_price")     p.tail_price = r.readNumber();
    else if (k == "total_price")    p.total_price = r.readNumber();
    else if (k == "price_pending")  p.price_pending = r.readBool();
    else if (k == "pending_reason") p.pending_reason = r.readString();
    else if (k == "color_downgraded") p.color_downgraded = r.readBool();
    else if (k == "note")           p.note = r.readString();
    else (void)r.readString();
  }
}

void writeJobRecord(JsonWriter& w, const PrintJobRecord& jr)
{
  w.beginObject();
  w.key("job_id");         w.writeUInt32(jr.job_id);
  w.key("batch_job_seq");  w.writeUInt32(jr.batch_job_seq);
  w.key("phase");          w.writeString(std::string(core::to_string(jr.phase)));
  w.key("last_updated_utc"); w.writeString(jr.last_updated_utc);
  writeJobSnapshot(w, jr.snapshot);
  writeMetadataSnapshot(w, jr.metadata);
  writeJobPricing(w, jr.pricing);
  w.endObject();
}

bool readJobRecord(JsonReader& r,
                   std::unordered_map<std::uint32_t, PrintJobRecord>& jobs)
{
  PrintJobRecord jr;
  bool hasSnapshot = false, hasMetadata = false, hasPricing = false;
  r.beginObject();
  while (r.endObjectOrNext())
  {
    const std::string k = r.readKey();
    if (k == "job_id")         jr.job_id = r.readUInt32();
    else if (k == "batch_job_seq") jr.batch_job_seq = r.readUInt32();
    else if (k == "phase")
    {
      const std::string ps = r.readString();
      if (ps == "Accepted")   jr.phase = core::JobLifecyclePhase::Accepted;
      else if (ps == "Spooling")  jr.phase = core::JobLifecyclePhase::Spooling;
      else if (ps == "Printing")  jr.phase = core::JobLifecyclePhase::Printing;
      else if (ps == "Completed") jr.phase = core::JobLifecyclePhase::Completed;
      else if (ps == "Failed")    jr.phase = core::JobLifecyclePhase::Failed;
      else if (ps == "Cancelled") jr.phase = core::JobLifecyclePhase::Cancelled;
      else if (ps == "Stalled")   jr.phase = core::JobLifecyclePhase::Stalled;
      else                        jr.phase = core::JobLifecyclePhase::Unknown;
    }
    else if (k == "last_updated_utc") jr.last_updated_utc = r.readString();
    else if (k == "snapshot")
    { readJobSnapshot(r, jr.snapshot); hasSnapshot = true; }
    else if (k == "metadata")
    {
      r.beginObject();
      while (r.endObjectOrNext())
      {
        const std::string mk = r.readKey();
        readMetadataField(r, jr.metadata, mk);
      }
      hasMetadata = true;
    }
    else if (k == "pricing")
    { readJobPricing(r, jr.pricing); hasPricing = true; }
    else (void)r.readString();
  }
  if (jr.job_id != 0)
  {
    jr.snapshot.job_id = jr.job_id;
    jr.metadata.job_id = jr.job_id;
    jr.pricing.job_id = jr.job_id;
    jobs[jr.job_id] = std::move(jr);
    return true;
  }
  return false;
}

}  // namespace

void BillingBatchRepository::setFilePath(const std::filesystem::path& path)
{
  path_ = path;
}

bool BillingBatchRepository::load(std::vector<BillingBatch>& out)
{
  std::string ignored_date;
  std::unordered_map<std::uint32_t, PrintJobRecord> ignored_jobs;
  return load(out, ignored_jobs, ignored_date);
}

bool BillingBatchRepository::load(std::vector<BillingBatch>& out,
                                   std::string& saved_date)
{
  std::unordered_map<std::uint32_t, PrintJobRecord> ignored_jobs;
  return load(out, ignored_jobs, saved_date);
}

bool BillingBatchRepository::load(
    std::vector<BillingBatch>& out,
    std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
    std::string& saved_date)
{
  out.clear();
  jobs.clear();
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
      if (key == "schema_version")
      {
        (void)r.readInt(); // read but not enforced yet — future compat
      }
      else if (key == "date")
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
            else if (k == "jobs")
            {
              r.beginArray();
              while (r.endArrayOrNext()) readJobRecord(r, jobs);
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
  const std::unordered_map<std::uint32_t, PrintJobRecord> emptyJobs;
  return save(batches, emptyJobs, std::string{});
}

bool BillingBatchRepository::save(const std::vector<BillingBatch>& batches,
                                   const std::string& date_yyyy_mm_dd) const
{
  const std::unordered_map<std::uint32_t, PrintJobRecord> emptyJobs;
  return save(batches, emptyJobs, date_yyyy_mm_dd);
}

bool BillingBatchRepository::save(
    const std::vector<BillingBatch>& batches,
    const std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
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
      w.key("schema_version"); w.writeInt(3);
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

        // Write per-batch job records for full registry restoration.
        w.key("jobs");
        w.beginArray();
        for (auto jid : batch.job_ids)
        {
          auto it = jobs.find(jid);
          if (it != jobs.end())
            writeJobRecord(w, it->second);
        }
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
