#pragma once

#include "core/data_contracts.h"

#include <string>
#include <string_view>

namespace wpce::storage
{

std::string xml_escape(std::string_view value);
std::string to_xml(const core::JobSnapshot& snapshot);
std::string to_xml(const core::MetadataSnapshot& snapshot);
std::string to_xml(const core::PageArtifactIndex& index);

}  // namespace wpce::storage

