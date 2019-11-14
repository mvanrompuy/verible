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

#include "verilog/formatting/token_annotator.h"

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <ostream>
#include <vector>

#include "gtest/gtest.h"
#include "absl/strings/string_view.h"
#include "common/formatting/format_token.h"
#include "common/formatting/unwrapped_line.h"
#include "common/formatting/unwrapped_line_test_utils.h"
#include "common/text/syntax_tree_context.h"
#include "common/text/tree_builder_test_util.h"
#include "common/util/casts.h"
#include "common/util/iterator_adaptors.h"
#include "verilog/CST/verilog_nonterminals.h"
#include "verilog/formatting/format_style.h"
#include "verilog/formatting/verilog_token.h"
#include "verilog/parser/verilog_parser.h"
#include "verilog/parser/verilog_token_enum.h"

namespace verilog {
namespace formatter {

using ::verible::InterTokenInfo;
using ::verible::PreFormatToken;
using ::verible::SpacingOptions;

// Private function with external linkage from token_annotator.cc.
extern void AnnotateFormatToken(const FormatStyle& style,
                                const PreFormatToken& prev_token,
                                PreFormatToken* curr_token,
                                const verible::SyntaxTreeContext& context);

namespace {

// TODO(fangism): Move much of this boilerplate to format_token_test_util.h.

// This test structure is a subset of InterTokenInfo.
// We do not want to compare break penalties, because that would be too
// change-detector-y.
struct ExpectedInterTokenInfo {
  int spaces_required = 0;
  SpacingOptions break_decision = SpacingOptions::Undecided;

  bool operator==(const InterTokenInfo& before) const {
    return spaces_required == before.spaces_required &&
           break_decision == before.break_decision;
  }

  bool operator!=(const InterTokenInfo& before) const {
    return !(*this == before);
  }
};

std::ostream& operator<<(std::ostream& stream,
                         const ExpectedInterTokenInfo& t) {
  stream << "{\n  spaces_required: " << t.spaces_required
         << "\n  break_decision: " << t.break_decision << "\n}";
  return stream;
}

// Returns false if all ExpectedFormattingCalculations are not equal and outputs
// the first difference.
// type T is any container or range over PreFormatTokens.
template <class T>
bool CorrectExpectedFormatTokens(
    const std::vector<ExpectedInterTokenInfo>& expected, const T& tokens) {
  EXPECT_EQ(expected.size(), tokens.size())
      << "Size of expected calculations and format tokens does not match.";
  if (expected.size() != tokens.size()) {
    return false;
  }

  const auto first_mismatch =
      std::mismatch(expected.cbegin(), expected.cend(), tokens.begin(),
                    [](const ExpectedInterTokenInfo& expected,
                       const PreFormatToken& token) -> bool {
                      return expected == token.before;
                    });
  const bool all_match = first_mismatch.first == expected.cend();
  const int mismatch_position =
      std::distance(expected.begin(), first_mismatch.first);
  EXPECT_TRUE(all_match) << "mismatch at [" << mismatch_position
                         << "], expected: " << *first_mismatch.first
                         << "\ngot: " << first_mismatch.second->before;
  return all_match;
}

struct AnnotateFormattingInformationTestCase {
  FormatStyle style;
  int uwline_indentation;
  std::initializer_list<ExpectedInterTokenInfo> expected_calculations;
  // This exists for the sake of forwarding to the UnwrappedLineMemoryHandler.
  // When passing token sequences for testing, use the tokens that are
  // recomputed in the UnwrappedLineMemoryHandler, which re-arranges
  // tokens' text into a contiguous string buffer in memory.
  std::initializer_list<verible::TokenInfo> input_tokens;

  // TODO(fangism): static_assert(expected_calculations.size() ==
  //                              input_tokens.size());
  //     or restructure using std::pair.
};

// Print input tokens' text for debugging.
std::ostream& operator<<(
    std::ostream& stream,
    const AnnotateFormattingInformationTestCase& test_case) {
  stream << '[';
  for (const auto& token : test_case.input_tokens) stream << ' ' << token.text;
  return stream << " ]";
}

// Pre-populates context stack for testing context-sensitive annotations.
// TODO(fangism): This class is easily made language-agnostic, and could
// move into a _test_util library.
class InitializedSyntaxTreeContext : public verible::SyntaxTreeContext {
 public:
  InitializedSyntaxTreeContext(std::initializer_list<NodeEnum> ancestors) {
    // Build up a "skinny" tree from the bottom-up, much like the parser does.
    std::vector<verible::SyntaxTreeNode*> parents;
    parents.reserve(ancestors.size());
    for (const auto ancestor : verible::reversed_view(ancestors)) {
      if (root_ == nullptr) {
        root_ = verible::MakeTaggedNode(ancestor);
      } else {
        root_ = verible::MakeTaggedNode(ancestor, root_);
      }
      parents.push_back(ABSL_DIE_IF_NULL(
          verible::down_cast<verible::SyntaxTreeNode*>(root_.get())));
    }
    for (const auto* parent : verible::reversed_view(parents)) {
      Push(*parent);
    }
  }

 private:
  // Syntax tree synthesized from sequence of node enums.
  verible::SymbolPtr root_;
};

std::ostream& operator<<(std::ostream& stream,
                         const InitializedSyntaxTreeContext& context) {
  stream << "[ ";
  for (const auto* node : verible::make_range(context.begin(), context.end())) {
    stream << NodeEnumToString(NodeEnum(ABSL_DIE_IF_NULL(node)->Tag().tag))
           << " ";
  }
  return stream << ']';
}

struct AnnotateWithContextTestCase {
  FormatStyle style;
  verible::TokenInfo left_token;
  verible::TokenInfo right_token;
  InitializedSyntaxTreeContext context;
  ExpectedInterTokenInfo expected_annotation;
};

const FormatStyle DefaultStyle;

constexpr int kUnhandledSpaces = 1;
constexpr ExpectedInterTokenInfo kUnhandledSpacing{kUnhandledSpaces,
                                                   SpacingOptions::Preserve};

// This test is going to ensure that given an UnwrappedLine, the format
// tokens are propagated with the correct annotations and spaces_required.
// SpacingOptions::Preserve implies that the particular token pair combination
// was not explicitly handled and just defaulted.
// This test covers cases that are not context-sensitive.
TEST(TokenAnnotatorTest, AnnotateFormattingInfoTest) {
  const std::initializer_list<AnnotateFormattingInformationTestCase>
      kTestCases = {
          // (empty array of tokens)
          {DefaultStyle, 0, {}, {}},

          // //comment1
          // //comment2
          {DefaultStyle,
           0,
           // ExpectedInterTokenInfo:
           // spaces_required, break_decision
           {{0, SpacingOptions::Undecided},  //
            {2, SpacingOptions::MustWrap}},
           {{yytokentype::TK_EOL_COMMENT, "//comment1"},
            {yytokentype::TK_EOL_COMMENT, "//comment2"}}},

          // module foo();
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::TK_module, "module"},
            {yytokentype::SymbolIdentifier, "foo"},
            {'(', "("},
            {')', ")"},
            {';', ";"}}},

          // module foo(a, b);
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},  // "a"
            {0, SpacingOptions::Undecided},  // ','
            {1, SpacingOptions::Undecided},  // "b"
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::TK_module, "module"},
            {yytokentype::SymbolIdentifier, "foo"},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "a"},
            {',', ","},
            {yytokentype::SymbolIdentifier, "b"},
            {')', ")"},
            {';', ";"}}},

          // module with_params #() ();
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},   // with_params
            kUnhandledSpacing,                // #
            {0, SpacingOptions::Undecided},   // (
            {0, SpacingOptions::Undecided},   // )
            {1, SpacingOptions::Undecided},   // (
            {0, SpacingOptions::Undecided},   // )
            {0, SpacingOptions::Undecided}},  // ;
           {{yytokentype::TK_module, "module"},
            {yytokentype::SymbolIdentifier, "with_params"},
            {'#', "#"},
            {'(', "("},
            {')', ")"},
            {'(', "("},
            {')', ")"},
            {';', ";"}}},

          // a = b[c];
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::SymbolIdentifier, "b"},
            {'[', "["},
            {yytokentype::SymbolIdentifier, "c"},
            {']', "]"},
            {';', ";"}}},

          // b[c][d] (multi-dimensional spacing)
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "b"},
            {'[', "["},
            {yytokentype::SymbolIdentifier, "c"},
            {']', "]"},
            {'[', "["},
            {yytokentype::SymbolIdentifier, "d"},
            {']', "]"}}},

          // always @(posedge clk)
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},   // always
            {1, SpacingOptions::Undecided},   // @
            {0, SpacingOptions::Undecided},   // (
            {0, SpacingOptions::Undecided},   // posedge
            {1, SpacingOptions::Undecided},   // clk
            {0, SpacingOptions::Undecided}},  // )
           {{yytokentype::TK_always, "always"},
            {'@', "@"},
            {'(', "("},
            {yytokentype::TK_posedge, "TK_posedge"},
            {yytokentype::SymbolIdentifier, "clk"},
            {')', ")"}}},

          // `WIDTH'(s) (casting operator)
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::MacroIdItem, "`WIDTH"},
            {'\'', "'"},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "s"},
            {')', ")"}}},

          // string'(s) (casting operator)
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::TK_string, "string"},
            {'\'', "'"},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "s"},
            {')', ")"}}},

          // void'(f()) (casting operator)
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::TK_void, "void"},
            {'\'', "'"},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "f"},
            {'(', "("},
            {')', ")"},
            {')', ")"}}},

          // 12'{34}
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::TK_DecNumber, "12"},
            {'\'', "'"},
            {'{', "{"},
            {yytokentype::TK_DecNumber, "34"},
            {'}', "}"}}},

          // k()'(s) (casting operator)
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "k"},
            {'(', "("},
            {')', ")"},
            {'\'', "'"},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "s"},
            {')', ")"}}},

          // a = 16'hf00d;
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::TK_DecNumber, "16"},
            {yytokentype::TK_HexBase, "'h"},
            {yytokentype::TK_HexDigits, "c0ffee"},
            {';', ";"}}},

          // a = 8'b1001_0110;
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::TK_DecNumber, "8"},
            {yytokentype::TK_BinBase, "'b"},
            {yytokentype::TK_BinDigits, "1001_0110"},
            {';', ";"}}},

          // a = 4'd10;
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::TK_DecNumber, "4"},
            {yytokentype::TK_DecBase, "'d"},
            {yytokentype::TK_DecDigits, "10"},
            {';', ";"}}},

          // a = 8'o100;
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::TK_DecNumber, "8"},
            {yytokentype::TK_OctBase, "'o"},
            {yytokentype::TK_OctDigits, "100"},
            {';', ";"}}},

          // a = 'hc0ffee;
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::TK_HexBase, "'h"},
            {yytokentype::TK_HexDigits, "c0ffee"},
            {';', ";"}}},

          // a = funk('b0, 'd'8);
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::SymbolIdentifier, "funk"},
            {'(', "("},
            {yytokentype::TK_BinBase, "'b"},
            {yytokentype::TK_BinDigits, "0"},
            {',', ","},
            {yytokentype::TK_DecBase, "'d"},
            {yytokentype::TK_DecDigits, "8"},
            {')', ")"},
            {';', ";"}}},

          // a = 'b0 + 'd9;
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::TK_BinBase, "'b"},
            {yytokentype::TK_BinDigits, "0"},
            {'+', "+"},
            {yytokentype::TK_DecBase, "'d"},
            {yytokentype::TK_DecDigits, "9"},
            {';', ";"}}},

          // a = {3{4'd9, 1'bz}};
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},  //  3
            kUnhandledSpacing,
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},  //  ,
            {1, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},  //  z
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided},
            {0, SpacingOptions::Undecided}},
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {'{', "{"},
            {yytokentype::TK_DecDigits, "3"},
            {'{', "{"},
            {yytokentype::TK_DecDigits, "4"},
            {yytokentype::TK_DecBase, "'d"},
            {yytokentype::TK_DecDigits, "9"},
            {',', ","},
            {yytokentype::TK_DecDigits, "1"},
            {yytokentype::TK_BinBase, "'b"},
            {yytokentype::TK_XZDigits, "z"},
            {'}', "}"},
            {'}', "}"},
            {';', ";"}}},

          // a ? b : c
          {
              DefaultStyle,
              0,
              {
                  {0, SpacingOptions::Undecided},  //  a
                  {1, SpacingOptions::Undecided},  //  ?
                  {1, SpacingOptions::Undecided},  //  b
                  kUnhandledSpacing,               //  :
                  kUnhandledSpacing,               //  c
              },
              {
                  {yytokentype::SymbolIdentifier, "a"},
                  {'?', "?"},
                  {yytokentype::SymbolIdentifier, "b"},
                  {':', ":"},
                  {yytokentype::SymbolIdentifier, "c"},
              },
          },

          // 1 ? 2 : 3
          {
              DefaultStyle,
              0,
              {
                  {0, SpacingOptions::Undecided},  //  1
                  {1, SpacingOptions::Undecided},  //  ?
                  {1, SpacingOptions::Undecided},  //  2
                  kUnhandledSpacing,               //  :
                  kUnhandledSpacing,               //  3
              },
              {
                  {yytokentype::TK_DecNumber, "1"},
                  {'?', "?"},
                  {yytokentype::TK_DecNumber, "2"},
                  {':', ":"},
                  {yytokentype::TK_DecNumber, "3"},
              },
          },

          // "1" ? "2" : "3"
          {
              DefaultStyle,
              0,
              {
                  {0, SpacingOptions::Undecided},  //  "1"
                  {1, SpacingOptions::Undecided},  //  ?
                  {1, SpacingOptions::Undecided},  //  "2"
                  kUnhandledSpacing,               //  :
                  kUnhandledSpacing,               //  "3"
              },
              {
                  {yytokentype::TK_StringLiteral, "1"},
                  {'?', "?"},
                  {yytokentype::TK_StringLiteral, "2"},
                  {':', ":"},
                  {yytokentype::TK_StringLiteral, "3"},
              },
          },

          // assign a = b ? 8'o100 : '0;
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},   //  assign
            {1, SpacingOptions::Undecided},   //  a
            {1, SpacingOptions::Undecided},   //  =
            {1, SpacingOptions::Undecided},   //  b
            {1, SpacingOptions::Undecided},   //  ?
            {1, SpacingOptions::Undecided},   //  8
            {0, SpacingOptions::Undecided},   //  'o
            {0, SpacingOptions::Undecided},   //  100
            kUnhandledSpacing,                //  :
            kUnhandledSpacing,                //  '0
            {0, SpacingOptions::Undecided}},  //  ;
           {{yytokentype::TK_assign, "assign"},
            {yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {yytokentype::SymbolIdentifier, "b"},
            {'?', "?"},
            {yytokentype::TK_DecNumber, "8"},
            {yytokentype::TK_OctBase, "'o"},
            {yytokentype::TK_OctDigits, "100"},
            {':', ":"},
            {yytokentype::TK_UnBasedNumber, "'0"},
            {';', ";"}}},

          // a = (b + c);
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},   // a
            {1, SpacingOptions::Undecided},   // =
            {1, SpacingOptions::Undecided},   // (
            {0, SpacingOptions::Undecided},   // b
            {1, SpacingOptions::Undecided},   // +
            {1, SpacingOptions::Undecided},   // c
            {0, SpacingOptions::Undecided},   // )
            {0, SpacingOptions::Undecided}},  // ;
           {{yytokentype::SymbolIdentifier, "a"},
            {'=', "="},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "b"},
            {'+', "+"},
            {yytokentype::SymbolIdentifier, "c"},
            {')', ")"},
            {';', ";"}}},

          // function foo(name = "foo");
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},   //  function
            {1, SpacingOptions::Undecided},   //  foo
            {0, SpacingOptions::Undecided},   //  (
            {0, SpacingOptions::Undecided},   //  name
            {1, SpacingOptions::Undecided},   //  =
            {1, SpacingOptions::Undecided},   //  "foo"
            {0, SpacingOptions::Undecided},   //  )
            {0, SpacingOptions::Undecided}},  //  ;
           {{yytokentype::TK_function, "function"},
            {yytokentype::SymbolIdentifier, "foo"},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "name"},
            {'=', "="},
            {yytokentype::TK_StringLiteral, "\"foo\""},
            {')', ")"},
            {';', ";"}}},

          // `define FOO(name = "bar")
          {DefaultStyle,
           0,
           {{0, SpacingOptions::Undecided},   //  `define
            {1, SpacingOptions::Undecided},   //  FOO
            {0, SpacingOptions::Undecided},   //  (
            {0, SpacingOptions::Undecided},   //  name
            {1, SpacingOptions::Undecided},   //  =
            {1, SpacingOptions::Undecided},   //  "bar"
            {0, SpacingOptions::Undecided}},  //  )
           {{yytokentype::PP_define, "`define"},
            {yytokentype::SymbolIdentifier, "FOO"},
            {'(', "("},
            {yytokentype::SymbolIdentifier, "name"},
            {'=', "="},
            {yytokentype::TK_StringLiteral, "\"bar\""},
            {')', ")"}}},

          // endfunction : funk
          {DefaultStyle,
           1,
           {{0, SpacingOptions::Undecided},
            {1, SpacingOptions::Undecided},
            kUnhandledSpacing},
           {
               {yytokentype::TK_endfunction, "endfunction"},
               {':', ":"},
               {yytokentype::SymbolIdentifier, "funk"},
           }},

          // case (expr):
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {1, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               kUnhandledSpacing,  // TODO(fangism): no space before case colon
           },
           {
               {yytokentype::TK_case, "case"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "expr"},
               {')', ")"},
               {':', ":"},
           }},

          // return 0;
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {1, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::TK_return, "return"},
               {yytokentype::TK_UnBasedNumber, "0"},
               {';', ";"},
           }},

          // funk();
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::SymbolIdentifier, "funk"},
               {'(', "("},
               {')', ")"},
               {';', ";"},
           }},

          // funk(arg);
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::SymbolIdentifier, "funk"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "arg"},
               {')', ")"},
               {';', ";"},
           }},

          // funk("arg");
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::SymbolIdentifier, "funk"},
               {'(', "("},
               {yytokentype::TK_StringLiteral, "\"arg\""},
               {')', ")"},
               {';', ";"},
           }},

          // funk(arg1, arg2);
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {1, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::SymbolIdentifier, "funk"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "arg1"},
               {',', ","},
               {yytokentype::SymbolIdentifier, "arg2"},
               {')', ")"},
               {';', ";"},
           }},

          // instantiation with named ports
          // funky town(.f1(arg1), .f2(arg2));
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               kUnhandledSpacing,  // TODO(fangism): adjacent identifiers
               {0, SpacingOptions::Undecided},  // '('
               {0, SpacingOptions::Undecided},  // '.'
               {0, SpacingOptions::Undecided},  // "f1"
               {0, SpacingOptions::Undecided},  // '('
               {0, SpacingOptions::Undecided},  // "arg1"
               {0, SpacingOptions::Undecided},  // ')'
               {0, SpacingOptions::Undecided},  // ','
               {1, SpacingOptions::Undecided},  // '.'
               {0, SpacingOptions::Undecided},  // "f1"
               {0, SpacingOptions::Undecided},  // '('
               {0, SpacingOptions::Undecided},  // "arg1"
               {0, SpacingOptions::Undecided},  // ')'
               {0, SpacingOptions::Undecided},  // ')'
               {0, SpacingOptions::Undecided},  // ';'
           },
           {
               {yytokentype::SymbolIdentifier, "funky"},
               {yytokentype::SymbolIdentifier, "town"},
               {'(', "("},
               {'.', "."},
               {yytokentype::SymbolIdentifier, "f1"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "arg1"},
               {')', ")"},
               {',', ","},
               {'.', "."},
               {yytokentype::SymbolIdentifier, "f2"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "arg2"},
               {')', ")"},
               {')', ")"},
               {';', ";"},
           }},

          // `ID.`ID
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::MacroIdentifier, "`ID"},
               {'.', "."},
               {yytokentype::MacroIdentifier, "`ID"},
           }},

          // id.id
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::SymbolIdentifier, "id"},
               {'.', "."},
               {yytokentype::SymbolIdentifier, "id"},
           }},

          // super.id
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::TK_super, "super"},
               {'.', "."},
               {yytokentype::SymbolIdentifier, "id"},
           }},

          // this.id
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::TK_this, "this"},
               {'.', "."},
               {yytokentype::SymbolIdentifier, "id"},
           }},

          // option.id
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::TK_option, "option"},
               {'.', "."},
               {yytokentype::SymbolIdentifier, "id"},
           }},

          // `MACRO();
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::MacroCallId, "`MACRO"},
               {'(', "("},
               {yytokentype::MacroCallCloseToEndLine, ")"},
               {';', ";"},
           }},

          // `MACRO(x);
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::MacroCallId, "`MACRO"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "x"},
               {yytokentype::MacroCallCloseToEndLine, ")"},
               {';', ";"},
           }},

          // `MACRO(y, x);
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},  // "y"
               {0, SpacingOptions::Undecided},  // ','
               {1, SpacingOptions::Undecided},  // "x"
               {0, SpacingOptions::Undecided},
               {0, SpacingOptions::Undecided},
           },
           {
               {yytokentype::MacroCallId, "`MACRO"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "y"},
               {',', ","},
               {yytokentype::SymbolIdentifier, "x"},
               {yytokentype::MacroCallCloseToEndLine, ")"},
               {';', ";"},
           }},

          // `define FOO
          // `define BAR
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // `define
               {1, SpacingOptions::Undecided},  // FOO
               {0, SpacingOptions::Undecided},  // "" (empty definition body)
               {0, SpacingOptions::MustWrap},   // `define
               {1, SpacingOptions::Undecided},  // BAR
               {0, SpacingOptions::Undecided},  // "" (empty definition body)
           },
           {
               {yytokentype::PP_define, "`define"},
               {yytokentype::SymbolIdentifier, "FOO"},
               {yytokentype::PP_define_body, ""},
               {yytokentype::PP_define, "`define"},
               {yytokentype::SymbolIdentifier, "BAR"},
               {yytokentype::PP_define_body, ""},
           }},

          // `define FOO 1
          // `define BAR 2
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // `define
               {1, SpacingOptions::Undecided},  // FOO
               kUnhandledSpacing,               // 1
               {1, SpacingOptions::MustWrap},   // `define
               {1, SpacingOptions::Undecided},  // BAR
               kUnhandledSpacing,               // 2
           },
           {
               {yytokentype::PP_define, "`define"},
               {yytokentype::PP_Identifier, "FOO"},
               {yytokentype::PP_define_body, "1"},
               {yytokentype::PP_define, "`define"},
               {yytokentype::PP_Identifier, "BAR"},
               {yytokentype::PP_define_body, "2"},
           }},

          // `define FOO()
          // `define BAR(x)
          // `define BAZ(y,z)
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},   // `define
               {1, SpacingOptions::Undecided},   // FOO
               {0, SpacingOptions::MustAppend},  // (
               {0, SpacingOptions::Undecided},   // )
               {0, SpacingOptions::Undecided},   // "" (empty definition body)

               {0, SpacingOptions::MustWrap},    // `define
               {1, SpacingOptions::Undecided},   // BAR
               {0, SpacingOptions::MustAppend},  // (
               {0, SpacingOptions::Undecided},   // x
               {0, SpacingOptions::Undecided},   // )
               {0, SpacingOptions::Undecided},   // "" (empty definition body)

               {0, SpacingOptions::MustWrap},    // `define
               {1, SpacingOptions::Undecided},   // BAZ
               {0, SpacingOptions::MustAppend},  // (
               {0, SpacingOptions::Undecided},   // y
               {0, SpacingOptions::Undecided},   // ,
               {1, SpacingOptions::Undecided},   // z
               {0, SpacingOptions::Undecided},   // )
               {0, SpacingOptions::Undecided},   // "" (empty definition body)
           },
           {
               {yytokentype::PP_define, "`define"},
               {yytokentype::PP_Identifier, "FOO"},
               {'(', "("},
               {')', ")"},
               {yytokentype::PP_define_body, ""},

               {yytokentype::PP_define, "`define"},
               {yytokentype::PP_Identifier, "BAR"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "x"},
               {')', ")"},
               {yytokentype::PP_define_body, ""},

               {yytokentype::PP_define, "`define"},
               {yytokentype::PP_Identifier, "BAZ"},
               {'(', "("},
               {yytokentype::SymbolIdentifier, "y"},
               {',', ","},
               {yytokentype::SymbolIdentifier, "z"},
               {')', ")"},
               {yytokentype::PP_define_body, ""},
           }},

          // `define ADD(y,z) y+z
          {
              DefaultStyle,
              1,
              {
                  {0, SpacingOptions::Undecided},   // `define
                  {1, SpacingOptions::Undecided},   // ADD
                  {0, SpacingOptions::MustAppend},  // (
                  {0, SpacingOptions::Undecided},   // y
                  {0, SpacingOptions::Undecided},   // ,
                  {1, SpacingOptions::Undecided},   // z
                  {0, SpacingOptions::Undecided},   // )
                  kUnhandledSpacing,                // "y+z"
              },
              {
                  {yytokentype::PP_define, "`define"},
                  {yytokentype::PP_Identifier, "ADD"},
                  {'(', "("},
                  {yytokentype::SymbolIdentifier, "y"},
                  {',', ","},
                  {yytokentype::SymbolIdentifier, "z"},
                  {')', ")"},
                  {yytokentype::PP_define_body, "y+z"},
              },
          },

          // function new;
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // function
               {1, SpacingOptions::Undecided},  // new
               {0, SpacingOptions::Undecided},  // ;
           },
           {
               {yytokentype::TK_function, "function"},
               {yytokentype::TK_new, "new"},
               {';', ";"},
           }},

          // function new();
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // function
               {1, SpacingOptions::Undecided},  // new
               {0, SpacingOptions::Undecided},  // (
               {0, SpacingOptions::Undecided},  // )
               {0, SpacingOptions::Undecided},  // ;
           },
           {
               {yytokentype::TK_function, "function"},
               {yytokentype::TK_new, "new"},
               {'(', "("},
               {')', ")"},
               {';', ";"},
           }},

          // escaped identifier
          // baz.\FOO .bar
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // baz
               {0, SpacingOptions::Undecided},  // .
               {0, SpacingOptions::Undecided},  // \FOO
               {1, SpacingOptions::Undecided},  // .
               {0, SpacingOptions::Undecided},  // bar
           },
           {
               {yytokentype::SymbolIdentifier, "baz"},
               {'.', "."},
               {yytokentype::EscapedIdentifier, "\\FOO"},
               {'.', "."},
               {yytokentype::SymbolIdentifier, "bar"},
           }},

          // escaped identifier inside macro call
          // `BAR(\FOO )
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // `BAR
               {0, SpacingOptions::Undecided},  // (
               {0, SpacingOptions::Undecided},  // \FOO
               {1, SpacingOptions::Undecided},  // )
           },
           {
               {yytokentype::MacroCallId, "`BAR"},
               {'(', "("},
               {yytokentype::EscapedIdentifier, "\\FOO"},
               {')', ")"},
           }},

          // import foo_pkg::symbol;
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // import
               {1, SpacingOptions::Undecided},  // foo_pkg
               {0, SpacingOptions::Undecided},  // ::
               {0, SpacingOptions::Undecided},  // symbol
               {0, SpacingOptions::Undecided},  // ;
           },
           {
               {yytokentype::TK_import, "import"},
               {yytokentype::SymbolIdentifier, "foo_pkg"},
               {yytokentype::TK_SCOPE_RES, "::"},
               {yytokentype::SymbolIdentifier, "symbol"},
               {';', ";"},
           }},

          // import foo_pkg::*;
          {DefaultStyle,
           1,
           {
               {0, SpacingOptions::Undecided},  // import
               {1, SpacingOptions::Undecided},  // foo_pkg
               {0, SpacingOptions::Undecided},  // ::
               {0, SpacingOptions::Undecided},  // *
               {0, SpacingOptions::Undecided},  // ;
           },
           {
               {yytokentype::TK_import, "import"},
               {yytokentype::SymbolIdentifier, "foo_pkg"},
               {yytokentype::TK_SCOPE_RES, "::"},
               {'*', "*"},
               {';', ";"},
           }},
      };

  int test_index = 0;
  for (const auto& test_case : kTestCases) {
    verible::UnwrappedLineMemoryHandler handler;
    handler.CreateTokenInfos(test_case.input_tokens);
    verible::UnwrappedLine unwrapped_line(test_case.uwline_indentation,
                                          handler.GetPreFormatTokensBegin());
    handler.AddFormatTokens(&unwrapped_line);
    // The format_token_enums are not yet set by AddFormatTokens.
    for (auto& ftoken : handler.pre_format_tokens_) {
      ftoken.format_token_enum =
          GetFormatTokenType(yytokentype(ftoken.TokenEnum()));
    }

    auto& ftokens_range = handler.pre_format_tokens_;
    // nullptr buffer_start is needed because token text do not belong to the
    // same contiguous string buffer.
    // Pass an empty/fake tree, which will not be used for testing
    // context-insensitive annotation rules.
    // Since we're using the joined string buffer inside handler,
    // we need to pass an EOF token that points to the end of that buffer.
    AnnotateFormattingInformation(test_case.style, nullptr, nullptr,
                                  handler.EOFToken(), ftokens_range.begin(),
                                  ftokens_range.end());
    EXPECT_TRUE(CorrectExpectedFormatTokens(test_case.expected_calculations,
                                            ftokens_range))
        << "mismatch at test case " << test_index << " of " << kTestCases.size()
        << ", tokens " << test_case;
    ++test_index;
  }
}  // NOLINT(readability/fn_size)

TEST(TokenAnnotatorTest, AnnotateFormattingWithContextTest) {
  const std::initializer_list<AnnotateWithContextTestCase> kTestCases = {
      // //comment1
      // //comment2
      {
          DefaultStyle,
          {yytokentype::TK_EOL_COMMENT, "//comment1"},  // left token
          {yytokentype::TK_EOL_COMMENT, "//comment2"},  // right token
          {},                                           // context
          // ExpectedInterTokenInfo:
          // spaces_required, break_decision
          {2, SpacingOptions::MustWrap},
      },

      // Without context, default is to treat '-' as binary.
      {
          DefaultStyle,
          {'-', "-"},                         // left token
          {yytokentype::TK_DecNumber, "42"},  // right token
          {},                                 // context
          {1, SpacingOptions::Undecided},
      },
      {
          DefaultStyle,
          {'-', "-"},
          {yytokentype::TK_DecNumber, "42"},
          {NodeEnum::kBinaryExpression},
          {1, SpacingOptions::Undecided},
      },

      // Handle '-' as a unary prefix expression.
      {
          DefaultStyle,
          {'-', "-"},                          // left token
          {yytokentype::TK_DecNumber, "42"},   // right token
          {NodeEnum::kUnaryPrefixExpression},  // context
          {0, SpacingOptions::MustAppend},
      },
      {
          DefaultStyle,
          {'-', "-"},
          {yytokentype::SymbolIdentifier, "xyz"},
          {NodeEnum::kUnaryPrefixExpression},
          {0, SpacingOptions::MustAppend},
      },
      {
          DefaultStyle,
          {'-', "-"},
          {'(', "("},
          {NodeEnum::kUnaryPrefixExpression},
          {0, SpacingOptions::MustAppend},
      },
      {
          DefaultStyle,
          {'-', "-"},
          {yytokentype::MacroIdItem, "`FOO"},
          {NodeEnum::kUnaryPrefixExpression},
          {0, SpacingOptions::MustAppend},
      },

      // Inside dimension ranges, force space preservation
      {
          DefaultStyle,
          {'*', "*"},
          {yytokentype::SymbolIdentifier, "foo"},
          {},
          {1, SpacingOptions::Undecided},
      },
      {
          DefaultStyle,
          {yytokentype::SymbolIdentifier, "foo"},
          {'*', "*"},
          {},
          {1, SpacingOptions::Undecided},
      },
      {
          DefaultStyle,
          {'*', "*"},
          {yytokentype::SymbolIdentifier, "foo"},
          {NodeEnum::kDimensionRange},
          {1, SpacingOptions::Preserve},
      },
      {
          DefaultStyle,
          {yytokentype::SymbolIdentifier, "foo"},
          {'*', "*"},
          {NodeEnum::kDimensionRange},
          {1, SpacingOptions::Preserve},
      },
      {
          DefaultStyle,
          {':', ":"},
          {yytokentype::SymbolIdentifier, "foo"},
          {NodeEnum::kDimensionRange},
          {1, SpacingOptions::Preserve},
      },
      {
          DefaultStyle,
          {yytokentype::SymbolIdentifier, "foo"},
          {':', ":"},
          {NodeEnum::kDimensionRange},
          {1, SpacingOptions::Preserve},
      },

      // spacing between ranges of multi-dimension arrays
      {
          DefaultStyle,
          {']', "]"},
          {'[', "["},
          {},  // any context
          {0, SpacingOptions::Undecided},
      },

      // spacing before first '[' of packed arrays in declarations
      {
          DefaultStyle,
          {yytokentype::TK_logic, "logic"},
          {'[', "["},
          {},  // unspecified context
          {0, SpacingOptions::Undecided},
      },
      {
          DefaultStyle,
          {yytokentype::SymbolIdentifier, "mytype1"},
          {'[', "["},
          {},  // unspecified context, this covers index expressions
          {0, SpacingOptions::Undecided},
      },
      {
          DefaultStyle,
          {yytokentype::TK_logic, "logic"},
          {'[', "["},
          {NodeEnum::kPackedDimensions},
          {1, SpacingOptions::Undecided},
      },
      {
          DefaultStyle,
          {yytokentype::SymbolIdentifier, "mytype2"},
          {'[', "["},
          {NodeEnum::kPackedDimensions},
          {1, SpacingOptions::Undecided},
      },
      {
          DefaultStyle,
          {yytokentype::SymbolIdentifier, "id1"},
          {'[', "["},
          {NodeEnum::kPackedDimensions, NodeEnum::kExpression},
          {0, SpacingOptions::Undecided},
      },

      // spacing after last ']' of packed arrays in declarations
      {
          DefaultStyle,
          {']', "]"},
          {yytokentype::SymbolIdentifier, "id_a"},
          {},                 // unspecified context
          kUnhandledSpacing,  // TODO(fangism): pick reasonable default
      },
      {
          DefaultStyle,
          {']', "]"},
          {yytokentype::SymbolIdentifier, "id_b"},
          {NodeEnum::kUnqualifiedId},  // unspecified context
          kUnhandledSpacing,           // TODO(fangism): pick reasonable default
      },
      {
          DefaultStyle,
          {']', "]"},
          {yytokentype::SymbolIdentifier, "id_c"},
          {NodeEnum::kDataTypeImplicitBasicIdDimensions,
           NodeEnum::kUnqualifiedId},  // unspecified context
          {1, SpacingOptions::Undecided},
      },
  };
  int test_index = 0;
  for (const auto& test_case : kTestCases) {
    VLOG(1) << "test_index[" << test_index << "]:";
    PreFormatToken left(&test_case.left_token);
    PreFormatToken right(&test_case.right_token);
    // Classify token type into major category
    left.format_token_enum = GetFormatTokenType(yytokentype(left.TokenEnum()));
    right.format_token_enum =
        GetFormatTokenType(yytokentype(right.TokenEnum()));

    VLOG(1) << "context: " << test_case.context;
    AnnotateFormatToken(test_case.style, left, &right, test_case.context);
    EXPECT_EQ(test_case.expected_annotation, right.before)
        << " with left=" << left.Text() << " and right=" << right.Text();
    ++test_index;
  }
}

}  // namespace
}  // namespace formatter
}  // namespace verilog