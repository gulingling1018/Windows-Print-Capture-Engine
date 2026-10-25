#pragma once
#include "common/physical_job_identity.h"
#include <windows.h>
#include <winspool.h>
#include <filterpipeline.h>
#include <iomanip>
#include <sstream>
#include <vector>

namespace wpce::filter
{
inline std::string capture_utf8(const wchar_t* value)
{
  if (!value) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string text(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), size, nullptr, nullptr);
  text.pop_back(); return text;
}
inline core::JobSnapshot physical_snapshot(const JOB_INFO_2W& info)
{
  core::JobSnapshot job;
  job.job_id = info.JobId; job.printer_name = capture_utf8(info.pPrinterName);
  job.user = capture_utf8(info.pUserName); job.document_name = capture_utf8(info.pDocument);
  const auto& time = info.Submitted;
  if (time.wYear)
  {
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << time.wYear << '-' << std::setw(2) << time.wMonth << '-'
        << std::setw(2) << time.wDay << 'T' << std::setw(2) << time.wHour << ':' << std::setw(2)
        << time.wMinute << ':' << std::setw(2) << time.wSecond;
    job.submit_time_utc = out.str();
  }
  return job;
}
inline core::JobSnapshot pipeline_physical_snapshot(IPrintPipelinePropertyBag* bag, std::uint32_t id)
{
  if (!bag || !id) return {};
  struct Value { VARIANT value; Value() { VariantInit(&value); } ~Value() { VariantClear(&value); } } property;
  if (FAILED(bag->GetProperty(XPS_FP_PRINTER_HANDLE, &property.value)) || property.value.vt != VT_BYREF || !property.value.byref) return {};
  const auto printer = static_cast<HANDLE>(property.value.byref); // borrowed handle: never close it
  DWORD needed = 0;
  GetJobW(printer, id, 2, nullptr, 0, &needed);
  if (!needed || needed > 1024 * 1024) return {};
  std::vector<BYTE> bytes(needed);
  if (!GetJobW(printer, id, 2, bytes.data(), needed, &needed)) return {};
  return physical_snapshot(*reinterpret_cast<const JOB_INFO_2W*>(bytes.data()));
}
}
