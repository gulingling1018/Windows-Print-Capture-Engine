#include "storage/reliable_file_writer.h"
#include "storage/json_reader.h"
#include "storage/json_serializer.h"
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <algorithm>
#include <iomanip>
#include <memory>
#include <limits>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace wpce::storage
{
namespace
{
std::string hex(const unsigned char* bytes, std::size_t length)
{
  constexpr char alphabet[] = "0123456789abcdef";
  std::string result; result.reserve(length * 2);
  for (std::size_t i = 0; i < length; ++i)
  { result += alphabet[bytes[i] >> 4]; result += alphabet[bytes[i] & 15]; }
  return result;
}
std::string path_utf8(const std::filesystem::path& path)
{ const auto value = path.generic_u8string(); return {value.begin(), value.end()}; }
bool pending(const std::filesystem::path& path) { return path.filename().string().ends_with(".pending.json"); }
std::uint64_t ordinal(const std::filesystem::path& path)
{
  const auto name = path.filename().string(); std::uint64_t value = 0;
  if (name.size() < 21 || std::from_chars(name.data(), name.data() + 20, value).ec != std::errc{}) return 0;
  return value;
}
std::string read_bounded(const std::filesystem::path& path, std::size_t max)
{
  if (std::filesystem::file_size(path) > max) throw std::runtime_error("pending transaction is oversized");
  std::ifstream in(path, std::ios::binary); std::ostringstream out; out << in.rdbuf();
  if (!in || in.bad()) throw std::runtime_error("cannot read pending transaction");
  return out.str();
}
std::vector<FileWrite> parse(const std::string& text)
{
  std::istringstream input(text); JsonReader reader(input); std::vector<FileWrite> writes;
  reader.beginArray();
  while (reader.endArrayOrNext())
  {
    FileWrite item; bool path = false, data = false;
    reader.beginObject();
    while (reader.endObjectOrNext())
    {
      const auto key = reader.readKey();
      if (key == "path")
      {
        const auto encoded = reader.readString();
        item.relative_path = std::filesystem::path(std::u8string(encoded.begin(), encoded.end())); path = true;
      }
      else if (key == "content") { item.content = reader.readString(); data = true; }
      else reader.skipValue();
    }
    if (!path || !data || writes.size() >= 16) throw std::runtime_error("invalid pending transaction");
    writes.push_back(std::move(item));
  }
  reader.finish();
  if (writes.empty()) throw std::runtime_error("empty pending transaction");
  return writes;
}
}
std::string stable_digest(const std::string& value)
{
  unsigned char digest[32];
  if (value.size() > std::numeric_limits<ULONG>::max() || BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()),
      digest, sizeof(digest)) < 0) throw std::runtime_error("SHA-256 failed");
  return hex(digest, sizeof(digest));
}
std::string unique_storage_id()
{
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("cannot allocate capture identity");
  return hex(reinterpret_cast<unsigned char*>(&guid), sizeof(guid));
}
std::string printer_storage_token(const std::string& printer) { return "p-" + stable_digest(printer); }
bool valid_artifact_filename(const std::string& value)
{
  if (value.empty() || value.size() > 200 || value == "." || value == ".." ||
      value.back() == '.' || value.back() == ' ') return false;
  for (const auto ch : value)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) return false;
  auto base = value.substr(0, value.find('.'));
  std::transform(base.begin(), base.end(), base.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return false;
  if (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' && base[3] <= '9') return false;
  return true;
}
std::filesystem::path checked_relative_target(const std::filesystem::path& root, const std::filesystem::path& relative)
{
  if (relative.empty() || relative.is_absolute() || relative.has_root_name()) throw std::runtime_error("absolute capture target rejected");
  auto result = root;
  for (const auto& part : relative)
  {
    auto name = part.string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (name == "_state" || name == "history" || name == "orders.json" || name.starts_with("orders.json.") ||
        name == "counters.json" || name == "wpce_config.json")
      throw std::runtime_error("capture transaction cannot overwrite business data");
    if (part == "." || part == ".." || part.empty() || !valid_artifact_filename(part.string()))
      throw std::runtime_error("unsafe capture target rejected");
    result /= part;
    const DWORD attributes = GetFileAttributesW(result.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      throw std::runtime_error("capture target traverses a link");
  }
  return result;
}
DirectoryLease::DirectoryLease(const std::filesystem::path& file)
{
  handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
DirectoryLease::~DirectoryLease() { if (*this) CloseHandle(handle_); }
DirectoryLease::operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
ReliableFileWriter::ReliableFileWriter(ReliableWriterConfig config) : config_(std::move(config))
{
  if (config_.root.empty() || !valid_artifact_filename(config_.queue_name)) throw std::invalid_argument("invalid reliable writer root/queue");
  config_.root = std::filesystem::absolute(config_.root).lexically_normal();
  spool_ = checked_relative_target(config_.root, std::filesystem::path("_writer-spool") / config_.queue_name);
}
ReliableFileWriter::~ReliableFileWriter() { stop(); }
void ReliableFileWriter::start()
{
  std::scoped_lock lock(state_mutex_);
  if (running_) return;
  if (worker_.joinable()) worker_.join();
  try
  {
    const auto counter = spool_ / "sequence.txt";
    std::uint64_t sequence = 0;
    if (std::filesystem::exists(counter))
    { std::ifstream input(counter); if (!(input >> sequence)) throw std::runtime_error("invalid capture sequence"); }
    required_sequence_ = sequence;
  }
  catch (...) { required_sequence_ = UINT64_MAX; }
  stopping_ = false; running_ = true;
  try { worker_ = std::thread(&ReliableFileWriter::run, this); }
  catch (...) { running_ = false; throw; }
}
void ReliableFileWriter::stop() noexcept
{
  std::thread worker;
  { std::scoped_lock lock(state_mutex_); stopping_ = true; if (worker_.joinable()) worker = std::move(worker_); }
  cv_.notify_all(); if (worker.joinable()) worker.join();
}
void ReliableFileWriter::request_stop() noexcept
{
  { std::scoped_lock lock(state_mutex_); stopping_ = true; }
  cv_.notify_all();
}
bool ReliableFileWriter::enqueue(std::vector<FileWrite> writes)
{
  std::scoped_lock lock(state_mutex_);
  if (!running_ || stopping_) { set_error("writer is stopped; transaction was not accepted"); return false; }
  try
  {
    if (writes.empty() || writes.size() > 16) throw std::runtime_error("invalid write transaction");
    std::ostringstream out; out << '[';
    for (std::size_t i = 0; i < writes.size(); ++i)
    {
      (void)checked_relative_target(config_.root, writes[i].relative_path);
      if (i) out << ',';
      out << "{\"path\":" << json_escape(path_utf8(writes[i].relative_path))
          << ",\"content\":" << json_escape(writes[i].content) << '}';
      if (out.tellp() > static_cast<std::streamoff>(config_.max_item_bytes)) throw std::runtime_error("transaction exceeds memory limit");
    }
    out << ']';
    ensure_output_directory(spool_);
    std::unique_ptr<DirectoryLease> lease;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
      lease = std::make_unique<DirectoryLease>(spool_ / "sequence.lock");
      if (*lease) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!lease || !*lease) throw std::runtime_error("capture journal is busy; transaction not accepted");
    std::uint64_t sequence = 0;
    const auto counter = spool_ / "sequence.txt";
    if (std::filesystem::exists(counter))
    {
      std::ifstream input(counter);
      if (!(input >> sequence) || sequence == UINT64_MAX) throw std::runtime_error("invalid capture sequence");
    }
    ++sequence;
    write_text_file_atomic(std::to_string(sequence), counter);
    std::ostringstream name; name << std::setfill('0') << std::setw(20) << sequence << '-' << unique_storage_id() << ".pending.json";
    write_text_file_atomic(out.str(), spool_ / name.str());
    if (sequence > required_sequence_) required_sequence_ = sequence;
    cv_.notify_all(); return true;
  }
  catch (const std::exception& error) { set_error(error.what()); return false; }
}
void ReliableFileWriter::set_error(std::string error) { std::scoped_lock lock(error_mutex_); error_ = std::move(error); }
std::string ReliableFileWriter::last_error() const { std::scoped_lock lock(error_mutex_); return error_; }
std::size_t ReliableFileWriter::pending_count() const noexcept
{
  try { std::size_t count = 0; if (std::filesystem::exists(spool_)) for (const auto& file : std::filesystem::directory_iterator(spool_)) if (pending(file.path()) && ordinal(file.path()) <= required_sequence_) ++count; return count; }
  catch (...) { return 1; }
}
std::uintmax_t ReliableFileWriter::pending_bytes() const noexcept
{
  try { std::uintmax_t bytes = 0; if (std::filesystem::exists(spool_)) for (const auto& file : std::filesystem::directory_iterator(spool_)) if (pending(file.path()) && ordinal(file.path()) <= required_sequence_) bytes += file.file_size(); return bytes; }
  catch (...) { return 0; }
}
void ReliableFileWriter::run() noexcept
{
  set_atomic_write_hook(config_.worker_hook);
  for (;;)
  {
    if (stopping_ && pending_count() == 0) { set_error({}); break; }
    bool work = false, failed = false;
    try
    {
      ensure_output_directory(spool_);
      DirectoryLease lease(spool_ / "writer.lock");
      if (lease)
      {
        // One transaction in memory; directory size does not grow RAM usage.
        std::filesystem::path next;
        for (const auto& entry : std::filesystem::directory_iterator(spool_))
        {
          if (!pending(entry.path()) || !entry.is_regular_file()) continue;
          if (next.empty() || entry.path().filename() < next.filename()) next = entry.path();
        }
        if (!next.empty())
        {
          work = true;
          const auto writes = parse(read_bounded(next, config_.max_item_bytes));
          for (const auto& item : writes)
            write_text_file_atomic(item.content, checked_relative_target(config_.root, item.relative_path));
          if (!std::filesystem::remove(next)) throw std::runtime_error("pending acknowledgement failed");
          if (pending_count() == 0) set_error({});
        }
        if (!work) set_error({});
      }
      // A peer may be writing our earlier transaction. Wait for the owned
      // boundary instead of failing, or waiting for unrelated future work.
    }
    catch (const std::exception& error) { set_error(error.what()); failed = true; }
    catch (...) { set_error("unexpected file writer failure; pending transaction retained"); failed = true; }
    if (stopping_ && (failed || pending_count() == 0)) break;
    if (!work || failed)
    {
      std::unique_lock lock(state_mutex_);
      cv_.wait_for(lock, config_.retry_delay);
    }
  }
  running_ = false;
}
}
