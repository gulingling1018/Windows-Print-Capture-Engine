#pragma once

#include "core/data_contracts.h"

#include <string>
#include <string_view>

namespace wpce::storage
{

std::string json_escape(std::string_view value);
std::string to_json(const core::JobSnapshot& snapshot);
std::string to_json(const core::MetadataSnapshot& snapshot);
std::string to_json(const core::JobLifecycleEvent& event);
std::string to_json(const core::JobLifecycleSummary& summary);
std::string to_json(const core::PageArtifactIndex& index);
std::string to_json(const core::PricingRule& rule);
std::string to_json(const core::JobPricingEntry& entry);
std::string to_json(const core::SettlementOrder& order);
std::string to_json(const core::SheetBreakdown& breakdown);

}  // namespace wpce::storage

