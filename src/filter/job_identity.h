#pragma once

#include <windows.h>
#include <filterpipeline.h>
#include <cstdint>
#include <optional>

namespace wpce::filter
{
// The pipeline contract uses VT_UI4. Accept old positive VT_I4 producers only.
inline std::optional<std::uint32_t> read_job_identity(IPrintPipelinePropertyBag* bag)
{
  if (!bag) return std::nullopt;
  VARIANT value;
  VariantInit(&value);
  const HRESULT result = bag->GetProperty(XPS_FP_JOB_ID, &value);
  std::optional<std::uint32_t> id;
  if (SUCCEEDED(result))
  {
    if (value.vt == VT_UI4 && value.ulVal != 0) id = value.ulVal;
    else if (value.vt == VT_I4 && value.lVal > 0) id = static_cast<std::uint32_t>(value.lVal);
  }
  VariantClear(&value);
  return id;
}
}
