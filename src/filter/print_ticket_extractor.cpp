#include "filter/print_ticket_extractor.h"
#include "common/com_ptr.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace wpce::filter
{

std::string read_print_stream(IUnknown* pStream)
{
  if (pStream == nullptr)
  {
    return {};
  }

  IPrintReadStream* read_stream = nullptr;
  HRESULT hr = pStream->QueryInterface(
    __uuidof(IPrintReadStream),
    reinterpret_cast<void**>(&read_stream));

  if (FAILED(hr) || read_stream == nullptr)
  {
    return {};
  }

  std::string result;
  constexpr std::size_t kChunkSize = 4096;
  constexpr std::size_t kMaxReadSize = 100ULL * 1024 * 1024;  // 100 MB
  std::vector<BYTE> buffer(kChunkSize);

  while (true)
  {
    ULONG bytes_read = 0;
    BOOL eof = FALSE;

    hr = read_stream->ReadBytes(
      buffer.data(),
      static_cast<ULONG>(buffer.size()),
      &bytes_read,
      &eof);

    if (FAILED(hr))
    {
      read_stream->Release();
      return {};
    }

    if (bytes_read > 0)
    {
      if (result.size() + bytes_read > kMaxReadSize)
      {
        ::OutputDebugStringA("[WPCE] read_print_stream: stream exceeds 100 MB limit, aborting\n");
        read_stream->Release();
        return {};
      }
      result.append(reinterpret_cast<const char*>(buffer.data()), bytes_read);
    }

    if (eof)
    {
      break;
    }
  }

  read_stream->Release();
  return result;
}

std::string extract_print_ticket_xml(IPartPrintTicket* pPrintTicket)
{
  if (pPrintTicket == nullptr)
  {
    return {};
  }

  common::com_ptr<IPrintReadStream> stream;
  {
    IPrintReadStream* raw = nullptr;
    HRESULT hr = pPrintTicket->GetStream(&raw);
    if (FAILED(hr) || raw == nullptr)
    {
      return {};
    }
    stream.attach(raw);
  }

  return read_print_stream(stream.get());
}

std::string read_istream_to_string(IStream* pStream)
{
  if (pStream == nullptr)
  {
    return {};
  }

  // Seek to beginning to get total size.
  LARGE_INTEGER zero{};
  ULARGE_INTEGER end{};
  HRESULT hr = pStream->Seek(zero, STREAM_SEEK_END, &end);
  if (FAILED(hr))
  {
    return {};
  }

  // Log stream size for diagnostics; guard against OOM on large XPS pages.
  {
    char dbg[128];
    std::snprintf(dbg, sizeof(dbg),
                  "[WPCE] read_istream_to_string: size=%llu bytes\n",
                  static_cast<unsigned long long>(end.QuadPart));
    ::OutputDebugStringA(dbg);
  }

  constexpr std::uint64_t kMaxStreamSize = 100ULL * 1024 * 1024;  // 100 MB
  if (end.QuadPart > kMaxStreamSize)
  {
    ::OutputDebugStringA("[WPCE] read_istream_to_string: OVERSIZED stream, aborting to prevent OOM\n");
    return {};
  }

  // Safe: end.QuadPart <= 100 MB, fits in ULONG on all supported platforms.
  const ULONG size = static_cast<ULONG>(end.QuadPart);
  std::string result;
  result.resize(size);

  hr = pStream->Seek(zero, STREAM_SEEK_SET, nullptr);
  if (FAILED(hr))
  {
    return {};
  }

  ULONG bytes_read = 0;
  hr = pStream->Read(result.data(), size, &bytes_read);
  if (FAILED(hr))
  {
    return {};
  }

  result.resize(bytes_read);
  return result;
}

}  // namespace wpce::filter
