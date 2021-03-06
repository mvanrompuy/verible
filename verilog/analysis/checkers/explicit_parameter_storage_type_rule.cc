// Copyright 2017-2020 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "verilog/analysis/checkers/explicit_parameter_storage_type_rule.h"

#include <set>
#include <string>

#include "absl/strings/str_cat.h"
#include "common/analysis/citation.h"
#include "common/analysis/lint_rule_status.h"
#include "common/analysis/matcher/bound_symbol_manager.h"
#include "common/text/symbol.h"
#include "common/text/syntax_tree_context.h"
#include "common/util/logging.h"
#include "verilog/CST/parameters.h"
#include "verilog/analysis/descriptions.h"
#include "verilog/analysis/lint_rule_registry.h"

namespace verilog {
namespace analysis {

using verible::GetStyleGuideCitation;
using verible::LintRuleStatus;
using verible::LintViolation;
using verible::SyntaxTreeContext;

// Register ExplicitParameterStorageTypeRule
VERILOG_REGISTER_LINT_RULE(ExplicitParameterStorageTypeRule);

absl::string_view ExplicitParameterStorageTypeRule::Name() {
  return "explicit-parameter-storage-type";
}
const char ExplicitParameterStorageTypeRule::kTopic[] = "constants";
const char ExplicitParameterStorageTypeRule::kMessage[] =
    "Explicitly define a storage type for every parameter and localparam, ";

std::string ExplicitParameterStorageTypeRule::GetDescription(
    DescriptionType description_type) {
  return absl::StrCat("Checks that every ",
                      Codify("parameter", description_type), " and ",
                      Codify("localparam", description_type),
                      " is declared with an explicit storage type. See ",
                      GetStyleGuideCitation(kTopic), ".");
}

void ExplicitParameterStorageTypeRule::HandleSymbol(
    const verible::Symbol& symbol, const SyntaxTreeContext& context) {
  verible::matcher::BoundSymbolManager manager;
  if (matcher_.Matches(symbol, &manager)) {
    // 'parameter type' declarations have a storage type declared.
    if (IsParamTypeDeclaration(symbol)) return;

    const auto* type_info_symbol = GetParamTypeInfoSymbol(symbol);
    if (IsTypeInfoEmpty(*ABSL_DIE_IF_NULL(type_info_symbol))) {
      const verible::TokenInfo& param_name = GetParameterNameToken(symbol);
      violations_.insert(LintViolation(
          param_name, absl::StrCat(kMessage, "(", param_name.text, ")."),
          context));
    }
  }
}

LintRuleStatus ExplicitParameterStorageTypeRule::Report() const {
  return LintRuleStatus(violations_, Name(), GetStyleGuideCitation(kTopic));
}

}  // namespace analysis
}  // namespace verilog
