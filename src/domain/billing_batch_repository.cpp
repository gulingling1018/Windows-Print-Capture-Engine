/// @file billing_batch_repository.cpp
/// BillingBatchRepository implementation.

#include "domain/billing_batch_repository.h"
#include "bridge/settlement_bridge.h"
#include "storage/json_reader.h"
#include "storage/file_utils.h"
#include "common/business_clock.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
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
  void writeRaw(const std::string& value) { beforeValue(); out_ << value; }
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
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[7]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c)); r += escaped;
          } else r += c;
      }
    }
    return r;
  }
};

using JsonReader = storage::JsonReader;

BatchState parseState(const std::string& s)
{
  if (s == "active")    return BatchState::Active;
  if (s == "suspended") return BatchState::Suspended;
  if (s == "settled")   return BatchState::Settled;
  if (s == "voided")    return BatchState::Voided;
  throw std::runtime_error("invalid batch state");
}

core::FieldState parseFieldState(const std::string& s)
{
  if (s == "reported")     return core::FieldState::Reported;
  if (s == "unsupported")  return core::FieldState::Unsupported;
  if (s == "not_reported") return core::FieldState::NotReported;
  if (s == "error")        return core::FieldState::Error;
  return core::FieldState::Unknown;
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
    else r.skipValue();
  }
}

// PrintJobRecord serialisation helpers
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
    else if (k == "state")  fv.state = parseFieldState(r.readString());
    else r.skipValue();
  }
}

void writeJobSnapshot(JsonWriter& w, const core::JobSnapshot& s)
{
  w.key("snapshot");
  w.beginObject();
  w.key("schema_version");  w.writeInt(s.schema_version);
  w.key("job_id");          w.writeUInt32(s.job_id);
  w.key("external_job_id"); w.writeUInt32(s.external_job_id);
  w.key("idempotency_key"); w.writeString(s.idempotency_key);
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
    else if (k == "external_job_id") s.external_job_id = r.readUInt32();
    else if (k == "idempotency_key") s.idempotency_key = r.readString();
    else if (k == "user")        s.user = r.readString();
    else if (k == "document_name") s.document_name = r.readString();
    else if (k == "printer_name")  s.printer_name = r.readString();
    else if (k == "submit_time_utc") s.submit_time_utc = r.readString();
    else if (k == "status")      s.status = r.readString();
    else if (k == "raw_status_flags") s.raw_status_flags = static_cast<std::uint32_t>(r.readInt());
    else if (k == "source")      s.source = r.readString();
    else r.skipValue();
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
  if (!m.file_extension_source.empty())
  { w.key("file_extension_source"); w.writeString(m.file_extension_source); }
  if (m.file_extension_confidence > 0)
  { w.key("file_extension_confidence"); w.writeInt(m.file_extension_confidence); }
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
  else if (k == "file_extension_source") m.file_extension_source = r.readString();
  else if (k == "file_extension_confidence") m.file_extension_confidence = r.readInt();
  else r.skipValue();
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
  w.key("matched_printer_name"); w.writeString(p.matched_printer_name);
  w.key("base_price");         w.writeDouble(p.base_price);
  w.key("tail_price");         w.writeDouble(p.tail_price);
  w.key("total_price");        w.writeDouble(p.total_price);
  w.key("color_pages");        w.writeInt(p.color_pages);
  w.key("mono_pages");         w.writeInt(p.mono_pages);
  w.key("color_subtotal");     w.writeDouble(p.color_subtotal);
  w.key("mono_subtotal");      w.writeDouble(p.mono_subtotal);
  w.key("price_pending");      w.writeBool(p.price_pending);
  if (!p.pending_reason.empty())
  { w.key("pending_reason");   w.writeString(p.pending_reason); }
  w.key("color_downgraded");   w.writeBool(p.color_downgraded);
  w.key("manual_price_override"); w.writeBool(p.manual_price_override);
  w.key("manual_total_price"); w.writeDouble(p.manual_total_price);
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
    else if (k == "matched_printer_name") p.matched_printer_name = r.readString();
    else if (k == "base_price")     p.base_price = r.readNumber();
    else if (k == "tail_price")     p.tail_price = r.readNumber();
    else if (k == "total_price")    p.total_price = r.readNumber();
    else if (k == "color_pages")    p.color_pages = r.readInt();
    else if (k == "mono_pages")     p.mono_pages = r.readInt();
    else if (k == "color_subtotal") p.color_subtotal = r.readNumber();
    else if (k == "mono_subtotal")  p.mono_subtotal = r.readNumber();
    else if (k == "price_pending")  p.price_pending = r.readBool();
    else if (k == "pending_reason") p.pending_reason = r.readString();
    else if (k == "color_downgraded") p.color_downgraded = r.readBool();
    else if (k == "manual_price_override") p.manual_price_override = r.readBool();
    else if (k == "manual_total_price") p.manual_total_price = r.readNumber();
    else if (k == "note")           p.note = r.readString();
    else r.skipValue();
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

std::filesystem::path batchJobsPath(const std::filesystem::path& state_path,
                                    const std::string& batch_id)
{
  return state_path.parent_path() / "batches" /
         storage::safe_filename_token(batch_id) / "jobs.json";
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
    else r.skipValue();
  }
  if (jr.job_id != 0 && hasSnapshot && hasMetadata && hasPricing)
  {
    jr.snapshot.job_id = jr.job_id;
    jr.metadata.job_id = jr.job_id;
    jr.pricing.job_id = jr.job_id;
    if (jobs.contains(jr.job_id)) throw std::runtime_error("duplicate job id");
    jobs[jr.job_id] = std::move(jr);
    return true;
  }
  return false;
}

bool readBatchJobsFile(
    const std::filesystem::path& path,
    std::unordered_map<std::uint32_t, PrintJobRecord>& jobs)
{
  try
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    JsonReader r(in);
    r.beginObject();
    while (r.endObjectOrNext())
    {
      const std::string key = r.readKey();
      if (key == "schema_version")
      {
        (void)r.readInt();
      }
      else if (key == "batch_id")
      {
        (void)r.readString();
      }
      else if (key == "jobs")
      {
        r.beginArray();
        while (r.endArrayOrNext())
          (void)readJobRecord(r, jobs);
      }
      else
      {
        r.skipValue();
      }
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

bool loadBatchesFromFile(
    const std::filesystem::path& path,
    std::vector<BillingBatch>& output,
    std::unordered_map<std::uint32_t, PrintJobRecord>* outputJobs,
    std::string& outputDate,
    const std::unordered_set<std::string>* batch_filter,
    std::vector<core::SettlementOrder>* outputOrders,
    BucketLoadStatus* status = nullptr,
    const std::string* content = nullptr)
{
  if (status) *status = BucketLoadStatus::Corrupt;
  std::vector<BillingBatch> out;
  std::unordered_map<std::uint32_t, PrintJobRecord> localJobs;
  std::vector<core::SettlementOrder> localOrders;
  auto* jobs = outputJobs ? &localJobs : nullptr;
  auto* orders = &localOrders;
  std::string saved_date;
  int schema = 0;
  bool hasTasks = false, hasOrders = false;
  std::unordered_set<std::string> taskIds, orderIds, orderTaskIds, completeTasks;
  if (path.empty()) return false;
  try
  {
    std::ifstream file;
    std::istringstream memory;
    if (content) memory.str(*content);
    else
    {
      std::error_code ec;
      const bool exists = std::filesystem::exists(path, ec);
      if (ec) { if (status) *status = BucketLoadStatus::AccessDenied; return false; }
      if (!exists)
      {
        output.clear(); if (outputJobs) outputJobs->clear(); if (outputOrders) outputOrders->clear(); outputDate.clear();
        if (status) *status = BucketLoadStatus::Missing;
        return true;
      }
      file.open(path, std::ios::binary);
      if (!file || !std::filesystem::is_regular_file(path))
      { if (status) *status = BucketLoadStatus::AccessDenied; return false; }
    }
    std::istream& in = content ? static_cast<std::istream&>(memory) : static_cast<std::istream&>(file);
    // Inspect the envelope before interpreting tasks: JSON object key order
    // must not allow a future schema to be mistaken for a corrupt old bucket.
    JsonReader envelope(in);
    envelope.beginObject();
    while (envelope.endObjectOrNext())
    {
      const auto key = envelope.readKey();
      if (key == "schema_version")
      {
        schema = envelope.readInt();
        if (schema < 1 || schema > 4)
        { if (status) *status = BucketLoadStatus::UnsupportedSchema; return false; }
      }
      else envelope.skipValue();
    }
    envelope.finish();
    in.clear(); in.seekg(0);
    JsonReader r(in);
    r.beginObject();
    while (r.endObjectOrNext())
    {
      const std::string key = r.readKey();
      if (key == "schema_version")
      {
        schema = r.readInt();
        if (schema < 1 || schema > 4)
        { if (status) *status = BucketLoadStatus::UnsupportedSchema; return false; }
      }
      else if (key == "date" || key == "business_date")
      {
        saved_date = r.readString();
      }
      else if (key == "batches" || key == "tasks")
      {
        if (hasTasks) throw std::runtime_error("duplicate task collection");
        hasTasks = true;
        r.beginArray();
        while (r.endArrayOrNext())
        {
          BillingBatch batch;
          bool hasJobIds = false, hasJobs = false, hasState = false, hasPrice = false;
          r.beginObject();
          while (r.endObjectOrNext())
          {
            const std::string k = r.readKey();
            if (k == "batch_id")            batch.batch_id = r.readString();
            else if (k == "task_id")
            {
              const std::string taskId = r.readString();
              if (batch.batch_id.empty()) batch.batch_id = taskId;
            }
            else if (k == "display_name")   batch.display_name = r.readString();
            else if (k == "source_user")    batch.source_user = r.readString();
            else if (k == "state")          { batch.state = parseState(r.readString()); hasState = true; }
            else if (k == "period_start_utc") batch.period_start_utc = r.readString();
            else if (k == "period_end_utc") batch.period_end_utc = r.readString();
            else if (k == "suspend_time_utc") batch.suspend_time_utc = r.readString();
            else if (k == "settle_order_id") batch.settle_order_id = r.readString();
            else if (k == "total_pages")    batch.total_pages = r.readInt();
            else if (k == "simplex_pages")  batch.simplex_pages = r.readInt();
            else if (k == "duplex_pages")   batch.duplex_pages = r.readInt();
            else if (k == "document_count") batch.document_count = r.readInt();
            else if (k == "total_price")    { batch.total_price = r.readNumber(); hasPrice = true; }
            else if (k == "pending_count")  batch.pending_count = r.readInt();
            else if (k == "first_doc_anchor") readAnchor(r, batch.first_doc_anchor);
            else if (k == "last_doc_anchor")  readAnchor(r, batch.last_doc_anchor);
            else if (k == "job_ids")
            {
              hasJobIds = true;
              r.beginArray();
              while (r.endArrayOrNext()) batch.job_ids.push_back(r.readUInt32());
            }
            else if (k == "jobs")
            {
              hasJobs = true;
              const bool should_load_jobs =
                jobs != nullptr &&
                (batch_filter == nullptr || batch_filter->find(batch.batch_id) != batch_filter->end());
              r.beginArray();
              while (r.endArrayOrNext())
              {
                if (should_load_jobs) { if (!readJobRecord(r, *jobs)) throw std::runtime_error("incomplete job"); }
                else r.skipValue();
              }
            }
            else r.skipValue();
          }
          if (batch.batch_id.empty() || !taskIds.insert(batch.batch_id).second || !hasJobIds)
            throw std::runtime_error("invalid task identity");
          if (hasJobs && hasState && hasPrice) completeTasks.insert(batch.batch_id);
          if (batch.total_price < 0 || batch.total_pages < 0 || batch.pending_count < 0)
            throw std::runtime_error("invalid task totals");
          std::unordered_set<std::uint32_t> ids;
          for (auto id : batch.job_ids)
          {
            if (id == 0 || !ids.insert(id).second) throw std::runtime_error("invalid job list");
            if (schema == 4 && jobs && !batch_filter && !jobs->contains(id))
              throw std::runtime_error("missing job record");
          }
          out.push_back(std::move(batch));
        }
      }
      else if (key == "orders")
      {
        hasOrders = true;
        r.beginArray();
        while (r.endArrayOrNext())
        {
          const std::string raw = r.readRawValue();
          if (orders != nullptr)
          {
            auto order = bridge::SettlementBridge::fromJson(raw);
            if (!order || order->order_id.empty()) throw std::runtime_error("invalid order");
            const auto task = order->task_id.empty() ? order->batch_id : order->task_id;
            if (!orderIds.insert(order->order_id).second || (!task.empty() && !orderTaskIds.insert(task).second))
              throw std::runtime_error("duplicate order");
            orders->push_back(std::move(*order));
          }
        }
      }
      else r.skipValue();
    }
    r.finish();
    if (!hasTasks || schema == 0) throw std::runtime_error("missing bucket schema/tasks");
    if (schema == 4 && (!hasOrders || !common::parse_business_date(saved_date)))
      throw std::runtime_error("incomplete daily bucket");
    if (path.filename() == "orders.json" && common::parse_business_date(path.parent_path().filename().string()) &&
        path.parent_path().filename().string() != saved_date)
      throw std::runtime_error("bucket date does not match path");
    if (schema == 4)
      for (const auto& batch : out)
      {
        if (!completeTasks.contains(batch.batch_id)) throw std::runtime_error("incomplete task");
        if (jobs && !batch_filter)
          for (auto id : batch.job_ids)
            if (!jobs->contains(id)) throw std::runtime_error("missing inline job");
        if (batch.is_settled() && (batch.settle_order_id.empty() || !orderIds.contains(batch.settle_order_id)))
          throw std::runtime_error("settled task missing order");
      }
    output = std::move(out); if (outputJobs) *outputJobs = std::move(localJobs);
    if (outputOrders) *outputOrders = std::move(localOrders); outputDate = std::move(saved_date);
    if (status) *status = BucketLoadStatus::Loaded;
    return true;
  }
  catch (...)
  {
    return false;
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
  return loadSummaries(out, ignored_date);
}

bool BillingBatchRepository::load(std::vector<BillingBatch>& out,
                                   std::string& saved_date)
{
  return loadSummaries(out, saved_date);
}

bool BillingBatchRepository::loadSummaries(std::vector<BillingBatch>& out,
                                           std::string& saved_date)
{
  return loadBatchesFromFile(path_, out, nullptr, saved_date, nullptr, nullptr);
}

bool BillingBatchRepository::load(
    std::vector<BillingBatch>& out,
    std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
    std::string& saved_date)
{
  jobs.clear();
  if (!loadSummaries(out, saved_date))
    return false;

  std::vector<std::string> batch_ids;
  batch_ids.reserve(out.size());
  for (const auto& batch : out)
    batch_ids.push_back(batch.batch_id);

  return loadJobsForBatches(batch_ids, jobs);
}

bool BillingBatchRepository::loadJobsForBatches(
    const std::vector<std::string>& batch_ids,
    std::unordered_map<std::uint32_t, PrintJobRecord>& jobs)
{
  jobs.clear();
  if (path_.empty()) return false;
  if (batch_ids.empty()) return true;

  std::vector<std::string> inline_fallback_ids;
  inline_fallback_ids.reserve(batch_ids.size());
  for (const auto& batch_id : batch_ids)
  {
    const auto bucket_path = batchJobsPath(path_, batch_id);
    std::error_code ec;
    const bool bucket_exists = std::filesystem::exists(bucket_path, ec);
    if (ec || !bucket_exists)
    {
      inline_fallback_ids.push_back(batch_id);
      continue;
    }
    if (!readBatchJobsFile(bucket_path, jobs))
      return false;
  }

  if (inline_fallback_ids.empty())
    return true;

  std::unordered_set<std::string> filter(inline_fallback_ids.begin(),
                                         inline_fallback_ids.end());
  std::vector<BillingBatch> ignored_batches;
  std::string ignored_date;
  std::unordered_map<std::uint32_t, PrintJobRecord> inline_jobs;
  if (!loadBatchesFromFile(path_, ignored_batches, &inline_jobs, ignored_date, &filter, nullptr))
    return false;
  jobs.insert(inline_jobs.begin(), inline_jobs.end());
  return true;
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
  const std::vector<core::SettlementOrder> emptyOrders;
  return save(batches, jobs, emptyOrders, date_yyyy_mm_dd);
}

bool BillingBatchRepository::save(
    const std::vector<BillingBatch>& batches,
    const std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
    const std::vector<core::SettlementOrder>& orders,
    const std::string& date_yyyy_mm_dd) const
{
  if (path_.empty()) return false;
  try
  {
    std::ostringstream out;
    {
      JsonWriter w(out);
      w.beginObject();
      w.key("schema_version"); w.writeInt(4);
      if (!date_yyyy_mm_dd.empty())
      {
        w.key("business_date"); w.writeString(date_yyyy_mm_dd);
      }
      w.key("tasks");
      w.beginArray();
      for (const auto& batch : batches)
      {
        w.beginObject();
        w.key("task_id");             w.writeString(batch.batch_id);
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

        w.key("jobs");
        w.beginArray();
        for (auto jid : batch.job_ids)
        {
          auto job = jobs.find(jid);
          if (job != jobs.end()) writeJobRecord(w, job->second);
        }
        w.endArray();

        w.endObject();
      }
      w.endArray();

      w.key("orders");
      w.beginArray();
      std::unordered_set<std::string> seenOrderIds;
      std::unordered_set<std::string> seenTaskIds;
      for (const auto& order : orders)
      {
        const std::string taskId = order.task_id.empty() ? order.batch_id : order.task_id;
        if (order.order_id.empty() || !seenOrderIds.insert(order.order_id).second)
          continue;
        if (!taskId.empty() && !seenTaskIds.insert(taskId).second)
          continue;
        w.writeRaw(bridge::SettlementBridge::toJson(order));
      }
      w.endArray();
      w.endObject();
      out << '\n';
      if (!out) return false;
    }
    // Parse the candidate through the same schema boundary, before touching the old bucket.
    const auto candidate = out.str();
    std::vector<BillingBatch> checkedBatches;
    std::unordered_map<std::uint32_t, PrintJobRecord> checkedJobs;
    std::vector<core::SettlementOrder> checkedOrders;
    std::string checkedDate;
    if (!loadBatchesFromFile(path_, checkedBatches, &checkedJobs, checkedDate, nullptr, &checkedOrders, nullptr, &candidate))
      throw std::runtime_error("invalid candidate bucket");
    if (path_.filename() == "orders.json" && common::parse_business_date(path_.parent_path().filename().string()) &&
        path_.parent_path().filename().string() != checkedDate)
      throw std::runtime_error("bucket date does not match path");
    BucketLoadStatus oldStatus;
    if (!loadBatchesFromFile(path_, checkedBatches, &checkedJobs, checkedDate, nullptr, &checkedOrders, &oldStatus))
      throw std::runtime_error("existing bucket is unreadable; overwrite refused");
    auto backup = path_; backup += ".bak";
    if (oldStatus == BucketLoadStatus::Missing && std::filesystem::exists(backup))
      throw std::runtime_error("missing bucket has a backup; recovery required before saving");
    if (oldStatus != BucketLoadStatus::Missing)
    {
      std::ifstream previous(path_, std::ios::binary);
      std::ostringstream bytes; bytes << previous.rdbuf();
      if (!previous || previous.bad()) throw std::runtime_error("backup read failed");
      storage::write_text_file_atomic(bytes.str(), backup);
    }
    storage::write_text_file_atomic(out.str(), path_);
    last_error_.clear();
    return true;
  }
  catch (const std::exception& error)
  {
    last_error_ = error.what();
    return false;
  }
}

bool BillingBatchRepository::load(
    std::vector<BillingBatch>& out,
    std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
    std::vector<core::SettlementOrder>& orders,
    std::string& saved_date)
{
  return loadDaily(out, jobs, orders, saved_date).ok();
}

bool BillingBatchRepository::loadOrders(std::vector<core::SettlementOrder>& orders)
{
  std::vector<BillingBatch> ignoredBatches;
  std::unordered_map<std::uint32_t, PrintJobRecord> ignoredJobs;
  std::string ignoredDate;
  return loadDaily(ignoredBatches, ignoredJobs, orders, ignoredDate).ok();
}

BucketLoadResult BillingBatchRepository::loadDaily(
    std::vector<BillingBatch>& batches,
    std::unordered_map<std::uint32_t, PrintJobRecord>& jobs,
    std::vector<core::SettlementOrder>& orders, std::string& saved_date)
{
  std::vector<BillingBatch> stagedBatches;
  std::unordered_map<std::uint32_t, PrintJobRecord> stagedJobs;
  std::vector<core::SettlementOrder> stagedOrders;
  std::string stagedDate;
  BucketLoadStatus status;
  const bool loaded = loadBatchesFromFile(path_, stagedBatches, &stagedJobs, stagedDate, nullptr, &stagedOrders, &status);
  auto backup = path_; backup += ".bak";
  std::error_code ec;
  const bool hasBackup = std::filesystem::exists(backup, ec);
  if (loaded && (status != BucketLoadStatus::Missing || !hasBackup))
  {
    batches = std::move(stagedBatches); jobs = std::move(stagedJobs);
    orders = std::move(stagedOrders); saved_date = std::move(stagedDate);
    last_error_.clear(); return {status, {}};
  }
  if (status == BucketLoadStatus::UnsupportedSchema || status == BucketLoadStatus::AccessDenied || ec)
  {
    last_error_ = "Order bucket is inaccessible or uses an unsupported schema; writes blocked.";
    return {status, last_error_};
  }
  std::vector<BillingBatch> recoveredBatches;
  std::unordered_map<std::uint32_t, PrintJobRecord> recoveredJobs;
  std::vector<core::SettlementOrder> recoveredOrders;
  std::string recoveredDate;
  BucketLoadStatus backupStatus;
  if (hasBackup && loadBatchesFromFile(backup, recoveredBatches, &recoveredJobs,
      recoveredDate, nullptr, &recoveredOrders, &backupStatus) && backupStatus == BucketLoadStatus::Loaded)
  {
    try
    {
      const auto pathDate = path_.parent_path().filename().string();
      if (common::parse_business_date(pathDate) && recoveredDate != pathDate)
        throw std::runtime_error("backup business date mismatch");
      if (std::filesystem::exists(path_))
      {
        std::ifstream damaged(path_, std::ios::binary);
        std::ostringstream bytes; bytes << damaged.rdbuf();
        if (!damaged || damaged.bad()) throw std::runtime_error("damaged bucket preservation failed");
        auto preserved = storage::make_temp_path(path_); preserved += ".invalid";
        storage::write_text_file_atomic(bytes.str(), preserved);
      }
      std::ifstream input(backup, std::ios::binary);
      std::ostringstream bytes; bytes << input.rdbuf();
      if (!input || input.bad()) throw std::runtime_error("backup read failed");
      storage::write_text_file_atomic(bytes.str(), path_);
      batches = std::move(recoveredBatches); jobs = std::move(recoveredJobs);
      orders = std::move(recoveredOrders); saved_date = std::move(recoveredDate);
      last_error_.clear();
      return {BucketLoadStatus::Recovered, "Recovered order bucket from last readable backup."};
    }
    catch (const std::exception& error) { last_error_ = error.what(); }
  }
  else last_error_ = "Order bucket is damaged and has no readable backup; writes blocked.";
  return {BucketLoadStatus::Corrupt, last_error_};
}

}  // namespace wpce::domain
