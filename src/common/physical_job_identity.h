#pragma once
#include "core/data_contracts.h"
namespace wpce::common
{
inline std::string physical_job_identity(const core::JobSnapshot& job)
{
  if (job.printer_name.empty() || job.document_name.empty() || job.submit_time_utc.empty()) return {};
  return job.printer_name + "\x1f" + job.user + "\x1f" + job.document_name + "\x1f" + job.submit_time_utc;
}
}
