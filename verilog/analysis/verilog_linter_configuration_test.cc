// Copyright 2017-2019 The Verible Authors.
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

#include "verilog/analysis/verilog_linter_configuration.h"

#include <cstddef>
#include <iosfwd>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/string_view.h"
#include "common/analysis/line_lint_rule.h"
#include "common/analysis/lint_rule_status.h"
#include "common/analysis/syntax_tree_lint_rule.h"
#include "common/analysis/text_structure_lint_rule.h"
#include "common/analysis/token_stream_lint_rule.h"
#include "common/text/concrete_syntax_leaf.h"
#include "common/text/concrete_syntax_tree.h"
#include "common/text/line_column_map.h"
#include "common/text/syntax_tree_context.h"
#include "common/text/text_structure.h"
#include "common/text/token_info.h"
#include "common/text/tree_builder_test_util.h"
#include "verilog/analysis/default_rules.h"
#include "verilog/analysis/descriptions.h"
#include "verilog/analysis/lint_rule_registry.h"
#include "verilog/analysis/verilog_linter.h"

namespace verilog {
namespace {

using verible::LineLintRule;
using verible::SyntaxTreeLintRule;
using verible::TextStructureLintRule;
using verible::TextStructureView;
using verible::TokenStreamLintRule;

using testing::IsEmpty;
using testing::SizeIs;

class TestRuleBase : public SyntaxTreeLintRule {
 public:
  void HandleLeaf(const verible::SyntaxTreeLeaf& leaf,
                  const verible::SyntaxTreeContext& context) override {}
  void HandleNode(const verible::SyntaxTreeNode& node,
                  const verible::SyntaxTreeContext& context) override {}
  verible::LintRuleStatus Report() const override {
    return verible::LintRuleStatus();
  }
};

class TestRule1 : public TestRuleBase {
 public:
  using rule_type = SyntaxTreeLintRule;
  static absl::string_view Name() { return "test-rule-1"; }
  static std::string GetDescription(analysis::DescriptionType) {
    return "TestRule1";
  }
};

class TestRule2 : public TestRuleBase {
 public:
  using rule_type = SyntaxTreeLintRule;
  static absl::string_view Name() { return "test-rule-2"; }
  static std::string GetDescription(analysis::DescriptionType) {
    return "TestRule2";
  }
};

class TestRule3 : public TokenStreamLintRule {
 public:
  using rule_type = TokenStreamLintRule;
  static absl::string_view Name() { return "test-rule-3"; }

  static std::string GetDescription(analysis::DescriptionType) {
    return "TestRule3";
  }

  void HandleToken(const verible::TokenInfo&) override {}

  verible::LintRuleStatus Report() const override {
    return verible::LintRuleStatus();
  }
};

class TestRule4 : public LineLintRule {
 public:
  using rule_type = LineLintRule;
  static absl::string_view Name() { return "test-rule-4"; }

  static std::string GetDescription(analysis::DescriptionType) {
    return "TestRule4";
  }

  void HandleLine(absl::string_view) override {}

  verible::LintRuleStatus Report() const override {
    return verible::LintRuleStatus();
  }
};

class TestRule5 : public TextStructureLintRule {
 public:
  using rule_type = TextStructureLintRule;
  static absl::string_view Name() { return "test-rule-5"; }

  static std::string GetDescription(analysis::DescriptionType) {
    return "TestRule1";
  }

  void Lint(const TextStructureView&, absl::string_view) override {}

  verible::LintRuleStatus Report() const override {
    return verible::LintRuleStatus();
  }
};

VERILOG_REGISTER_LINT_RULE(TestRule1);
VERILOG_REGISTER_LINT_RULE(TestRule2);
VERILOG_REGISTER_LINT_RULE(TestRule3);
VERILOG_REGISTER_LINT_RULE(TestRule4);
VERILOG_REGISTER_LINT_RULE(TestRule5);

// Dummy text structure with a single empty root node for syntax tree.
class FakeTextStructureView : public TextStructureView {
 public:
  FakeTextStructureView() : TextStructureView("") {
    syntax_tree_ = verible::Node();
  }
};

// Don't care about line numbers for these tests.
static const verible::LineColumnMap dummy_map("");

// Don't care about file name for these tests.
static const char filename[] = "";

TEST(ProjectPolicyTest, MatchesAnyPath) {
  struct TestCase {
    ProjectPolicy policy;
    absl::string_view filename;
    const char* expected_match;
  };
  const TestCase kTestCases[] = {
      {{"policyX", {}, {}, {}, {}, {}}, "filename", nullptr},
      {{"policyX", {"file"}, {}, {}, {}, {}}, "filename", "file"},
      {{"policyX", {"not-a-match"}, {}, {}, {}, {}}, "filename", nullptr},
      {{"policyX", {"xxxx", "yyyy"}, {}, {}, {}, {}}, "file/name.txt", nullptr},
      {{"policyX", {"xxxx", "name"}, {}, {}, {}, {}}, "file/name.txt", "name"},
      {{"policyX", {"xxxx", "file"}, {}, {}, {}, {}}, "file/name.txt", "file"},
      {{"policyX", {"name", "file"}, {}, {}, {}, {}}, "file/name.txt", "name"},
  };
  for (const auto& test : kTestCases) {
    const char* match = test.policy.MatchesAnyPath(test.filename);
    if (test.expected_match != nullptr) {
      EXPECT_EQ(absl::string_view(match), test.expected_match);
    } else {
      EXPECT_EQ(match, nullptr);
    }
  }
}

TEST(ProjectPolicyTest, MatchesAnyExclusions) {
  struct TestCase {
    ProjectPolicy policy;
    absl::string_view filename;
    const char* expected_match;
  };
  const TestCase kTestCases[] = {
      {{"policyX", {}, {}, {}, {}, {}}, "filename", nullptr},
      {{"policyX", {}, {"file"}, {}, {}, {}}, "filename", "file"},
      {{"policyX", {}, {"not-a-match"}, {}, {}, {}}, "filename", nullptr},
      {{"policyX", {}, {"xxxx", "yyyy"}, {}, {}, {}}, "file/name.txt", nullptr},
      {{"policyX", {}, {"xxxx", "name"}, {}, {}, {}}, "file/name.txt", "name"},
      {{"policyX", {}, {"xxxx", "file"}, {}, {}, {}}, "file/name.txt", "file"},
      {{"policyX", {}, {"name", "file"}, {}, {}, {}}, "file/name.txt", "name"},
  };
  for (const auto& test : kTestCases) {
    const char* match = test.policy.MatchesAnyExclusions(test.filename);
    if (test.expected_match != nullptr) {
      EXPECT_EQ(absl::string_view(match), test.expected_match);
    } else {
      EXPECT_EQ(match, nullptr);
    }
  }
}

TEST(ProjectPolicyTest, IsValid) {
  const std::pair<ProjectPolicy, bool> kTestCases[] = {
      {{"policyX", {"path"}, {}, {"owner"}, {"test-rule-1"}, {}}, true},
      {{"policyX", {"path"}, {}, {"owner"}, {}, {"test-rule-1"}}, true},
      {{"policyX", {"path"}, {}, {"owner"}, {"test-rule-1"}, {"test-rule-2"}},
       true},
      {{"policyX", {"path"}, {}, {"owner"}, {"not-a-test-rule"}, {}}, false},
      {{"policyX", {"path"}, {}, {"owner"}, {}, {"not-a-test-rule"}}, false},
      {{"policyX",
        {"path"},
        {},
        {"owner"},
        {"test-rule-1", "not-a-test-rule"},
        {}},
       false},
      {{"policyX",
        {"path"},
        {},
        {"owner"},
        {},
        {"not-a-test-rule", "test-rule-1"}},
       false},
  };
  for (const auto& test : kTestCases) {
    EXPECT_EQ(test.first.IsValid(), test.second);
  }
}

TEST(ProjectPolicyTest, ListPathGlobs) {
  const std::pair<ProjectPolicy, absl::string_view> kTestCases[] = {
      {{"policyX", {}, {}, {}, {}, {}}, ""},
      {{"policyX", {"path"}, {}, {}, {}, {}}, "*path*"},
      {{"policyX", {"path1", "path2"}, {}, {}, {}, {}}, "*path1* | *path2*"},
      {{"policyX", {"pa/th1", "pa/th2"}, {}, {}, {}, {}},
       "*pa/th1* | *pa/th2*"},
  };
  for (const auto& test : kTestCases) {
    EXPECT_EQ(test.first.ListPathGlobs(), test.second);
  }
}

// Confirms that each syntax tree rule yields a set of results.
TEST(VerilogSyntaxTreeLinterConfigurationTest, AddsExpectedNumber) {
  LinterConfiguration config;
  EXPECT_FALSE(config.RuleIsOn("test-rule-1"));
  EXPECT_FALSE(config.RuleIsOn("test-rule-2"));

  config.TurnOn("test-rule-1");
  EXPECT_TRUE(config.RuleIsOn("test-rule-1"));
  EXPECT_FALSE(config.RuleIsOn("test-rule-2"));

  config.TurnOn("test-rule-2");
  EXPECT_TRUE(config.RuleIsOn("test-rule-1"));
  EXPECT_TRUE(config.RuleIsOn("test-rule-2"));
  EXPECT_THAT(config.ActiveRuleIds(), SizeIs(2));

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, SizeIs(2));
}

// Confirms that each token stream rule yields a set of results.
TEST(VerilogTokenStreamLinterConfigurationTest, AddsExpectedNumber) {
  LinterConfiguration config;
  EXPECT_FALSE(config.RuleIsOn("test-rule-3"));
  config.TurnOn("test-rule-3");
  EXPECT_TRUE(config.RuleIsOn("test-rule-3"));
  EXPECT_THAT(config.ActiveRuleIds(), SizeIs(1));

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, SizeIs(1));
}

// Confirms that each line-based rule yields a set of results.
TEST(VerilogLineLinterConfigurationTest, AddsExpectedNumber) {
  LinterConfiguration config;
  EXPECT_FALSE(config.RuleIsOn("test-rule-4"));
  config.TurnOn("test-rule-4");
  EXPECT_TRUE(config.RuleIsOn("test-rule-4"));
  EXPECT_THAT(config.ActiveRuleIds(), SizeIs(1));

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, SizeIs(1));
}

// Confirms that each text-structure rule yields a set of results.
TEST(VerilogTextStructureLinterConfigurationTest, AddsExpectedNumber) {
  LinterConfiguration config;
  EXPECT_FALSE(config.RuleIsOn("test-rule-5"));
  config.TurnOn("test-rule-5");
  EXPECT_TRUE(config.RuleIsOn("test-rule-5"));
  EXPECT_THAT(config.ActiveRuleIds(), SizeIs(1));

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, SizeIs(1));
}

// Verifies that turning on-off rules works.
TEST(VerilogSyntaxTreeLinterConfigurationTest, TurnOnTurnOff) {
  LinterConfiguration config;
  EXPECT_THAT(config.ActiveRuleIds(), IsEmpty());
  config.TurnOn("test-rule-1");
  EXPECT_TRUE(config.RuleIsOn("test-rule-1"));
  config.TurnOff("test-rule-1");
  EXPECT_FALSE(config.RuleIsOn("test-rule-1"));
  EXPECT_THAT(config.ActiveRuleIds(), IsEmpty());

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, IsEmpty());
}

TEST(LinterConfigurationTest, ComparisonOperatorSameElement) {
  LinterConfiguration config1, config2;
  EXPECT_EQ(config1, config2);
  config1.TurnOn("rule-x");
  EXPECT_NE(config1, config2);
  config2.TurnOn("rule-x");
  EXPECT_EQ(config1, config2);
  config1.TurnOff("rule-x");
  EXPECT_NE(config1, config2);
  config2.TurnOff("rule-x");
  EXPECT_EQ(config1, config2);
}

TEST(LinterConfigurationTest, ComparisonSameDifferentElement) {
  LinterConfiguration config1, config2;
  config1.TurnOn("rule-x");
  EXPECT_NE(config1, config2);
  config2.TurnOn("rule-y");
  EXPECT_NE(config1, config2);
  config1.TurnOff("rule-x");
  EXPECT_NE(config1, config2);
  config2.TurnOff("rule-y");
  EXPECT_EQ(config1, config2);
}

TEST(LinterConfigurationTest, StreamOperator) {
  LinterConfiguration config;
  {
    std::ostringstream stream;
    stream << config;
    EXPECT_EQ(stream.str(), "{  }");
  }
  config.TurnOn("rule-abc");
  {
    std::ostringstream stream;
    stream << config;
    EXPECT_EQ(stream.str(), "{ rule-abc }");
  }
  config.TurnOn("rule-xyz");
  {
    std::ostringstream stream;
    stream << config;
    EXPECT_EQ(stream.str(), "{ rule-abc, rule-xyz }");
  }
  config.TurnOff("rule-abc");
  {
    std::ostringstream stream;
    stream << config;
    EXPECT_EQ(stream.str(), "{ rule-xyz }");
  }
  config.TurnOff("rule-xyz");
  {
    std::ostringstream stream;
    stream << config;
    EXPECT_EQ(stream.str(), "{  }");
  }
}

TEST(VerilogSyntaxTreeLinterConfigurationTest, DefaultEmpty) {
  LinterConfiguration config;
  EXPECT_THAT(config.ActiveRuleIds(), IsEmpty());

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, IsEmpty());
}

TEST(VerilogSyntaxTreeLinterConfigurationTest, UseRuleSetAll) {
  LinterConfiguration config;
  config.UseRuleSet(RuleSet::kAll);

  auto expected_size = analysis::RegisteredSyntaxTreeRulesNames().size() +
                       analysis::RegisteredTokenStreamRulesNames().size() +
                       analysis::RegisteredTextStructureRulesNames().size() +
                       analysis::RegisteredLineRulesNames().size();
  EXPECT_THAT(config.ActiveRuleIds(), SizeIs(expected_size));

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, SizeIs(expected_size));
}

TEST(VerilogSyntaxTreeLinterConfigurationTest, UseRuleSetNone) {
  LinterConfiguration config;
  config.UseRuleSet(RuleSet::kNone);
  EXPECT_THAT(config.ActiveRuleIds(), IsEmpty());

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, IsEmpty());
}

TEST(VerilogSyntaxTreeLinterConfigurationTest, NoneResets) {
  LinterConfiguration config;
  config.TurnOn("test-rule-1");
  config.TurnOn("test-rule-2");
  config.TurnOn("test-rule-3");
  config.TurnOn("test-rule-4");
  config.UseRuleSet(RuleSet::kNone);
  EXPECT_THAT(config.ActiveRuleIds(), IsEmpty());

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  EXPECT_THAT(status, IsEmpty());
}

TEST(VerilogSyntaxTreeLinterConfigurationTest, UseRuleSetDefault) {
  LinterConfiguration config;
  config.UseRuleSet(RuleSet::kDefault);

  VerilogLinter linter;
  linter.Configure(config);
  FakeTextStructureView text_structure;
  linter.Lint(text_structure, filename);

  auto status = linter.ReportStatus(dummy_map, text_structure.Contents());
  auto expected_size = std::extent<decltype(analysis::kDefaultRuleSet)>::value;
  EXPECT_THAT(config.ActiveRuleIds(), SizeIs(expected_size));
  EXPECT_THAT(status, SizeIs(expected_size));
}

// Tests that empty policy doesn't cause any change in configuration.
TEST(LinterConfigurationUseProjectPolicyTest, BlankPolicyBlankFilename) {
  LinterConfiguration config, default_config;
  ProjectPolicy policy;
  config.UseProjectPolicy(policy, "");
  EXPECT_EQ(config, default_config);
}

// Test single rule can be enabled with path matching.
TEST(LinterConfigurationUseProjectPolicyTest, EnableRule) {
  LinterConfiguration config;
  ProjectPolicy policy{"policyX", {"path"}, {}, {"owner"}, {}, {"wanted-rule"}};
  EXPECT_FALSE(config.RuleIsOn("wanted-rule"));
  config.UseProjectPolicy(policy, "some/path/foo");
  EXPECT_TRUE(config.RuleIsOn("wanted-rule"));
}

// Test that rule is not enabled because path does not match.
TEST(LinterConfigurationUseProjectPolicyTest, EnableFilePathNotMatched) {
  LinterConfiguration config;
  ProjectPolicy policy{"policyX", {"not-gonna-match"}, {}, {"owner"},
                       {},        {"wanted-rule"}};
  EXPECT_FALSE(config.RuleIsOn("wanted-rule"));
  config.UseProjectPolicy(policy, "some/path/foo");
  EXPECT_FALSE(config.RuleIsOn("wanted-rule"));
}

// Test single rule can be disabled with path matching.
TEST(LinterConfigurationUseProjectPolicyTest, DisableRule) {
  LinterConfiguration config;
  config.TurnOn("unwanted-rule");
  ProjectPolicy policy{"policyX", {"path"},          {},
                       {"owner"}, {"unwanted-rule"}, {}};
  EXPECT_TRUE(config.RuleIsOn("unwanted-rule"));
  config.UseProjectPolicy(policy, "some/path/foo");
  EXPECT_FALSE(config.RuleIsOn("unwanted-rule"));
}

// Test that rule remains enabled because path does not match.
TEST(LinterConfigurationUseProjectPolicyTest, DisableRulePathNotMatched) {
  LinterConfiguration config;
  config.TurnOn("unwanted-rule");
  ProjectPolicy policy{"policyX", {"does-not-match"}, {},
                       {"owner"}, {"unwanted-rule"},  {}};
  EXPECT_TRUE(config.RuleIsOn("unwanted-rule"));
  config.UseProjectPolicy(policy, "some/path/foo");
  EXPECT_TRUE(config.RuleIsOn("unwanted-rule"));
}

// Test that enabling a rule takes precedence over disabling.
TEST(LinterConfigurationUseProjectPolicyTest, EnableRuleWins) {
  LinterConfiguration config;
  // Same rule is disabled and enabled.
  ProjectPolicy policy{"policyX", {"path"},        {},
                       {"owner"}, {"wanted-rule"}, {"wanted-rule"}};
  EXPECT_FALSE(config.RuleIsOn("wanted-rule"));
  config.UseProjectPolicy(policy, "some/path/foo");
  EXPECT_TRUE(config.RuleIsOn("wanted-rule"));
}

//
// Tests for parse/unparse on RuleSet
//
TEST(RuleSetTest, ParseRuleSetSuccess) {
  std::string none_text = "none";
  std::string all_text = "all";
  std::string default_text = "default";

  std::string none_error = "";
  std::string all_error = "";
  std::string default_error = "";

  RuleSet none_destination, all_destination, default_destination;

  bool none_result = AbslParseFlag(none_text, &none_destination, &none_error);
  bool all_result = AbslParseFlag(all_text, &all_destination, &all_error);
  bool default_result =
      AbslParseFlag(default_text, &default_destination, &default_error);

  EXPECT_TRUE(none_result);
  EXPECT_EQ(none_error, "");
  EXPECT_EQ(none_destination, RuleSet::kNone);

  EXPECT_TRUE(all_result);
  EXPECT_EQ(all_error, "");
  EXPECT_EQ(all_destination, RuleSet::kAll);

  EXPECT_TRUE(default_result);
  EXPECT_EQ(default_error, "");
  EXPECT_EQ(default_destination, RuleSet::kDefault);
}

TEST(RuleSetTest, ParseRuleSetError) {
  std::string bad_text = "fdsfdfds";
  std::string error = "";

  RuleSet rule_result;
  bool result = AbslParseFlag(bad_text, &rule_result, &error);
  EXPECT_FALSE(result);
  EXPECT_NE(error, "");
}

TEST(RuleSetTest, UnparseRuleSetSuccess) {
  EXPECT_EQ("none", AbslUnparseFlag(RuleSet::kNone));
  EXPECT_EQ("default", AbslUnparseFlag(RuleSet::kDefault));
  EXPECT_EQ("all", AbslUnparseFlag(RuleSet::kAll));
}

//
// Tests for parse / unparse on RuleBundle
//
TEST(RuleBundleTest, UnparseRuleBundleSeveral) {
  RuleBundle bundle = {
      {{"flag1", RuleSetting::kRuleOn}, {"flag2", RuleSetting::kRuleOn}}};
  std::string expected = "flag2,flag1";
  std::string result = AbslUnparseFlag(bundle);
  EXPECT_EQ(result, expected);
}

TEST(RuleBundleTest, UnparseRuleBundleSeveralTurnOff) {
  RuleBundle bundle = {
      {{"flag1", RuleSetting::kRuleOff}, {"flag2", RuleSetting::kRuleOn}}};
  std::string expected = "flag2,-flag1";
  std::string result = AbslUnparseFlag(bundle);
  EXPECT_EQ(result, expected);
}

TEST(RuleBundleTest, UnparseRuleBundleEmpty) {
  RuleBundle bundle = {};
  std::string expected = "";
  std::string result = AbslUnparseFlag(bundle);
  EXPECT_EQ(result, expected);
}

TEST(RuleBundleTest, ParseRuleBundleEmpty) {
  std::string text = "";
  RuleBundle bundle;
  std::string error;
  bool success = AbslParseFlag(text, &bundle, &error);
  EXPECT_TRUE(success);
  EXPECT_TRUE(bundle.rules.empty());
}

TEST(RuleBundleTest, ParseRuleBundleAcceptSeveral) {
  std::string text = "test-rule-1,test-rule-2";
  RuleBundle bundle;
  std::string error;
  bool success = AbslParseFlag(text, &bundle, &error);
  ASSERT_TRUE(success);
  ASSERT_THAT(bundle.rules, SizeIs(2));
  EXPECT_EQ(bundle.rules["test-rule-1"], RuleSetting::kRuleOn);
  EXPECT_EQ(bundle.rules["test-rule-2"], RuleSetting::kRuleOn);
}

TEST(RuleBundleTest, ParseRuleBundleAcceptOne) {
  std::string text = "test-rule-1";
  RuleBundle bundle;
  std::string error;
  bool success = AbslParseFlag(text, &bundle, &error);
  ASSERT_TRUE(success);
  ASSERT_THAT(bundle.rules, SizeIs(1));
  EXPECT_EQ(bundle.rules["test-rule-1"], RuleSetting::kRuleOn);
}

TEST(RuleBundleTest, ParseRuleBundleAcceptSeveralTurnOff) {
  std::string text = "test-rule-1,-test-rule-2";
  RuleBundle bundle;
  std::string error;
  bool success = AbslParseFlag(text, &bundle, &error);
  ASSERT_TRUE(success);
  ASSERT_THAT(bundle.rules, SizeIs(2));
  EXPECT_EQ(bundle.rules["test-rule-1"], RuleSetting::kRuleOn);
  EXPECT_EQ(bundle.rules["test-rule-2"], RuleSetting::kRuleOff);
}

TEST(RuleBundleTest, ParseRuleBundleAcceptOneTurnOff) {
  std::string text = "-test-rule-1";
  RuleBundle bundle;
  std::string error;
  bool success = AbslParseFlag(text, &bundle, &error);
  ASSERT_TRUE(success);
  ASSERT_THAT(bundle.rules, SizeIs(1));
  EXPECT_EQ(bundle.rules["test-rule-1"], RuleSetting::kRuleOff);
}

TEST(RuleBundleTest, ParseRuleBundleReject) {
  std::string text = "test-rule-1,bad-flag";
  RuleBundle bundle;
  std::string error;
  bool success = AbslParseFlag(text, &bundle, &error);
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "invalid flag \"bad-flag\"");
}

}  // namespace
}  // namespace verilog