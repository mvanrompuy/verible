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

#include "verilog/analysis/checkers/signal_name_style_rule.h"

#include <initializer_list>

#include "gtest/gtest.h"
#include "common/analysis/linter_test_utils.h"
#include "common/analysis/syntax_tree_linter_test_utils.h"
#include "common/text/symbol.h"
#include "verilog/CST/verilog_nonterminals.h"
#include "verilog/analysis/verilog_analyzer.h"

namespace verilog {
namespace analysis {
namespace {

using verible::LintTestCase;
using verible::RunLintTestCases;

TEST(SignalNameStyleRuleTest, ModulePortTests) {
  constexpr int kToken = SymbolIdentifier;
  const std::initializer_list<LintTestCase> kTestCases = {
      {""},
      {"module foo(input logic b_a_r); endmodule"},
      {"module foo(input wire hello_world1); endmodule"},
      {"module foo(wire ", {kToken, "HelloWorld"}, "); endmodule"},
      {"module foo(input logic ", {kToken, "_bar"}, "); endmodule"},
      {"module foo(input logic [3:0] ", {kToken, "Foo_bar"}, "); endmodule"},
      {"module foo(input logic b_a_r [3:0]); endmodule"},
      {"module foo(input logic [3:0] ",
       {kToken, "Bar"},
       ", input logic ",
       {kToken, "Bar2"},
       " [4]); endmodule"},
      {"module foo(input logic hello_world, input bar); endmodule"},
      {"module foo(input logic hello_world, input ",
       {kToken, "b_A_r"},
       "); endmodule"},
      {"module foo(input logic ",
       {kToken, "HelloWorld"},
       ", output ",
       {kToken, "Bar"},
       "); endmodule"},
      {"module foo(input logic ",
       {kToken, "hello_World"},
       ", wire b_a_r = 1); endmodule"},
      {"module foo(input hello_world, output b_a_r, input wire bar2); "
       "endmodule"},
      {"module foo(input hello_world, output b_a_r, input wire ",
       {kToken, "Bad"},
       "); "
       "endmodule"},
  };
  RunLintTestCases<VerilogAnalyzer, SignalNameStyleRule>(kTestCases);
}

}  // namespace
}  // namespace analysis
}  // namespace verilog