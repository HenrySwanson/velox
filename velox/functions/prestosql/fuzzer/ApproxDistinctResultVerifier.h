/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <string>

#include "velox/core/PlanNode.h"
#include "velox/exec/fuzzer/ResultVerifier.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/vector/ComplexVector.h"

namespace facebook::velox::exec::test {

// Compares results of approx_distinct(x[, e]) with count(distinct x).
// For each group calculates the difference between 2 values and counts number
// of groups where difference is > 2e. If total number of groups is >= 50,
// allows 2 groups > 2e. If number of groups is small (< 50),
// expects all groups to be under 2e.
class ApproxDistinctResultVerifier : public ResultVerifier {
 public:
  bool supportsCompare() override {
    return false;
  }

  bool supportsVerify() override {
    return true;
  }

  // Compute count(distinct x) over 'input'.
  void initialize(
      const std::vector<RowVectorPtr>& input,
      const std::vector<std::string>& groupingKeys,
      const core::AggregationNode::Aggregate& aggregate,
      const std::string& aggregateName) override {
    auto plan =
        PlanBuilder()
            .values(input)
            .singleAggregation(groupingKeys, {makeCountDistinctCall(aggregate)})
            .planNode();

    expected_ = AssertQueryBuilder(plan).copyResults(input[0]->pool());
    groupingKeys_ = groupingKeys;
    name_ = aggregateName;
    error_ = extractError(aggregate, input[0]);
  }

  bool compare(
      const RowVectorPtr& /*result*/,
      const RowVectorPtr& /*altResult*/) override {
    VELOX_UNSUPPORTED();
  }

  bool verify(const RowVectorPtr& result) override {
    // Union 'result' with 'expected_', group by on 'groupingKeys_' and produce
    // pairs of actual and expected values per group. We cannot use join because
    // grouping keys may have nulls.
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto expectedSource = PlanBuilder(planNodeIdGenerator)
                              .values({expected_})
                              .appendColumns({"'expected' as label"})
                              .planNode();

    auto actualSource = PlanBuilder(planNodeIdGenerator)
                            .values({result})
                            .appendColumns({"'actual' as label"})
                            .planNode();

    auto mapAgg = fmt::format("map_agg(label, {}) as m", name_);
    auto plan = PlanBuilder(planNodeIdGenerator)
                    .localPartition({}, {expectedSource, actualSource})
                    .singleAggregation(groupingKeys_, {mapAgg})
                    .project({"m['actual'] as a", "m['expected'] as e"})
                    .planNode();
    auto combined = AssertQueryBuilder(plan).copyResults(result->pool());

    auto* actual = combined->childAt(0)->as<SimpleVector<int64_t>>();
    auto* expected = combined->childAt(1)->as<SimpleVector<int64_t>>();

    const auto numGroups = result->size();
    VELOX_CHECK_EQ(numGroups, combined->size());

    std::vector<double> largeGaps;
    for (auto i = 0; i < numGroups; ++i) {
      VELOX_CHECK(!actual->isNullAt(i))
      VELOX_CHECK(!expected->isNullAt(i))

      const auto actualCnt = actual->valueAt(i);
      const auto expectedCnt = expected->valueAt(i);
      if (actualCnt != expectedCnt) {
        if (expectedCnt > 0) {
          const auto gap =
              std::abs(actualCnt - expectedCnt) * 1.0 / expectedCnt;
          if (gap > 2 * error_) {
            largeGaps.push_back(gap);
            LOG(ERROR) << fmt::format(
                "approx_distinct(x, {}) is more than 2 stddev away from "
                "count(distinct x). Difference: {}, approx_distinct: {}, "
                "count(distinct): {}. This is unusual, but doesn't necessarily "
                "indicate a bug.",
                error_,
                gap,
                actualCnt,
                expectedCnt);
          }
        } else {
          LOG(ERROR) << fmt::format(
              "count(distinct x) returned 0, but approx_distinct(x, {}) is {}",
              error_,
              actualCnt);
          return false;
        }
      }
    }

    // We expect large deviations (>2 stddev) in < 5% of values.
    if (numGroups >= 50) {
      return largeGaps.size() <= 3;
    }

    return largeGaps.empty();
  }

  void reset() override {
    expected_.reset();
  }

 private:
  static constexpr double kDefaultError = 0.023;

  static double extractError(
      const core::AggregationNode::Aggregate& aggregate,
      const RowVectorPtr& input) {
    const auto& args = aggregate.call->inputs();

    if (args.size() == 1) {
      return kDefaultError;
    }

    auto field = core::TypedExprs::asFieldAccess(args[1]);
    VELOX_CHECK_NOT_NULL(field);
    auto errorVector =
        input->childAt(field->name())->as<SimpleVector<double>>();
    return errorVector->valueAt(0);
  }

  static std::string makeCountDistinctCall(
      const core::AggregationNode::Aggregate& aggregate) {
    const auto& args = aggregate.call->inputs();
    VELOX_CHECK_GE(args.size(), 1)

    auto inputField = core::TypedExprs::asFieldAccess(args[0]);
    VELOX_CHECK_NOT_NULL(inputField)

    std::string countDistinctCall =
        fmt::format("count(distinct {})", inputField->name());

    if (aggregate.mask != nullptr) {
      countDistinctCall +=
          fmt::format(" filter (where {})", aggregate.mask->name());
    }

    return countDistinctCall;
  }

  RowVectorPtr expected_;
  std::vector<std::string> groupingKeys_;
  std::string name_;
  double error_;
};

} // namespace facebook::velox::exec::test
