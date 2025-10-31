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

#include "velox/experimental/cudf/CudfConfig.h"
#include "velox/experimental/cudf/connectors/hive/CudfHiveConnector.h"
#include "velox/experimental/cudf/connectors/hive/CudfHiveDataSource.h"
#include "velox/experimental/cudf/exec/CudfAssignUniqueId.h"
#include "velox/experimental/cudf/exec/CudfConversion.h"
#include "velox/experimental/cudf/exec/CudfFilterProject.h"
#include "velox/experimental/cudf/exec/CudfHashAggregation.h"
#include "velox/experimental/cudf/exec/CudfHashJoin.h"
#include "velox/experimental/cudf/exec/CudfLimit.h"
#include "velox/experimental/cudf/exec/CudfLocalPartition.h"
#include "velox/experimental/cudf/exec/CudfOrderBy.h"
#include "velox/experimental/cudf/exec/ToCudf.h"
#include "velox/experimental/cudf/exec/Utilities.h"
#include "velox/experimental/cudf/expression/AstExpression.h"
#include "velox/experimental/cudf/expression/ExpressionEvaluator.h"

#include "folly/Conv.h"
#include "velox/exec/AssignUniqueId.h"
#include "velox/exec/CallbackSink.h"
#include "velox/exec/Driver.h"
#include "velox/exec/FilterProject.h"
#include "velox/exec/HashAggregation.h"
#include "velox/exec/HashBuild.h"
#include "velox/exec/HashProbe.h"
#include "velox/exec/Limit.h"
#include "velox/exec/Operator.h"
#include "velox/exec/OrderBy.h"
#include "velox/exec/StreamingAggregation.h"
#include "velox/exec/TableScan.h"
#include "velox/exec/TopN.h"
#include "velox/exec/Values.h"

#include <cudf/detail/nvtx/ranges.hpp>

#include <cuda.h>

#include <iostream>

static const std::string kCudfAdapterName = "cuDF";

namespace facebook::velox::cudf_velox {

namespace {

template <class... Deriveds, class Base>
bool isAnyOf(const Base* p) {
  return ((dynamic_cast<const Deriveds*>(p) != nullptr) || ...);
}

} // namespace

bool CompileState::compile(bool allowCpuFallback) {
  auto operators = driver_.operators();

  if (CudfConfig::getInstance().debugEnabled) {
    LOG(INFO) << "Operators before adapting for cuDF: count ["
              << operators.size() << "]" << std::endl;
    for (auto& op : operators) {
      LOG(INFO) << "  Operator: ID " << op->operatorId() << ": "
                << op->toString() << std::endl;
    }
    LOG(INFO) << "allowCpuFallback = " << allowCpuFallback << std::endl;
  }

  bool replacementsMade = false;
  auto ctx = driver_.driverCtx();

  // Get plan node by id lookup.
  auto getPlanNode = [&](const core::PlanNodeId& id) {
    auto& nodes = driverFactory_.planNodes;
    auto it =
        std::find_if(nodes.cbegin(), nodes.cend(), [&id](const auto& node) {
          return node->id() == id;
        });
    if (it != nodes.end()) {
      return *it;
    }
    VELOX_CHECK(driverFactory_.consumerNode->id() == id);
    return driverFactory_.consumerNode;
  };

  auto isTableScanSupported = [getPlanNode](const exec::Operator* op) {
    if (!isAnyOf<exec::TableScan>(op)) {
      return false;
    }
    auto tableScanNode = std::dynamic_pointer_cast<const core::TableScanNode>(
        getPlanNode(op->planNodeId()));
    VELOX_CHECK(tableScanNode != nullptr);
    auto const& connector = velox::connector::getConnector(
        tableScanNode->tableHandle()->connectorId());
    auto cudfHiveConnector = std::dynamic_pointer_cast<
        facebook::velox::cudf_velox::connector::hive::CudfHiveConnector>(
        connector);
    if (!cudfHiveConnector) {
      return false;
    }
    // TODO (dm): we need to ask CudfHiveConnector whether this table handle is
    // supported by it. It may choose to produce a HiveDatasource.
    return true;
  };

  auto isFilterProjectSupported = [getPlanNode](const exec::Operator* op) {
    if (auto filterProjectOp = dynamic_cast<const exec::FilterProject*>(op)) {
      auto projectPlanNode = std::dynamic_pointer_cast<const core::ProjectNode>(
          getPlanNode(filterProjectOp->planNodeId()));
      auto filterNode = filterProjectOp->filterNode();
      bool canBeEvaluated = true;
      if (projectPlanNode &&
          !canBeEvaluatedByCudf(projectPlanNode->projections())) {
        canBeEvaluated = false;
      }
      if (canBeEvaluated && filterNode &&
          !canBeEvaluatedByCudf({filterNode->filter()})) {
        canBeEvaluated = false;
      }
      return canBeEvaluated;
    }
    return false;
  };

  auto isJoinSupported = [getPlanNode](const exec::Operator* op) {
    if (!isAnyOf<exec::HashBuild, exec::HashProbe>(op)) {
      return false;
    }
    auto planNode = std::dynamic_pointer_cast<const core::HashJoinNode>(
        getPlanNode(op->planNodeId()));
    if (!planNode) {
      return false;
    }
    if (!CudfHashJoinProbe::isSupportedJoinType(planNode->joinType())) {
      return false;
    }
    // disabling null-aware anti join with filter until we implement it right
    if (planNode->joinType() == core::JoinType::kAnti and
        planNode->isNullAware() and planNode->filter()) {
      return false;
    }
    return true;
  };

  auto isSupportedGpuOperator =
      [isFilterProjectSupported, isJoinSupported, isTableScanSupported](
          const exec::Operator* op) {
        return isAnyOf<
                   exec::OrderBy,
                   exec::HashAggregation,
                   exec::Limit,
                   exec::LocalPartition,
                   exec::LocalExchange,
                   exec::AssignUniqueId>(op) ||
            isFilterProjectSupported(op) || isJoinSupported(op) ||
            isTableScanSupported(op);
      };

  std::vector<bool> isSupportedGpuOperators(operators.size());
  std::transform(
      operators.begin(),
      operators.end(),
      isSupportedGpuOperators.begin(),
      isSupportedGpuOperator);
  auto acceptsGpuInput = [isFilterProjectSupported,
                          isJoinSupported](const exec::Operator* op) {
    return isAnyOf<
               exec::OrderBy,
               exec::HashAggregation,
               exec::Limit,
               exec::LocalPartition,
               exec::AssignUniqueId>(op) ||
        isFilterProjectSupported(op) || isJoinSupported(op);
  };
  auto producesGpuOutput = [isFilterProjectSupported,
                            isJoinSupported,
                            isTableScanSupported](const exec::Operator* op) {
    return isAnyOf<
               exec::OrderBy,
               exec::HashAggregation,
               exec::Limit,
               exec::LocalExchange,
               exec::AssignUniqueId>(op) ||
        isFilterProjectSupported(op) ||
        (isAnyOf<exec::HashProbe>(op) && isJoinSupported(op)) ||
        (isTableScanSupported(op));
  };

  int32_t operatorsOffset = 0;
  for (int32_t operatorIndex = 0; operatorIndex < operators.size();
       ++operatorIndex) {
    std::vector<std::unique_ptr<exec::Operator>> replaceOp;

    exec::Operator* oper = operators[operatorIndex];
    auto replacingOperatorIndex = operatorIndex + operatorsOffset;
    VELOX_CHECK(oper);

    const bool previousOperatorIsNotGpu =
        (operatorIndex > 0 and !isSupportedGpuOperators[operatorIndex - 1]);
    const bool nextOperatorIsNotGpu =
        (operatorIndex < operators.size() - 1 and
         !isSupportedGpuOperators[operatorIndex + 1]);
    const bool isLastOperatorOfTask =
        driverFactory_.outputDriver and operatorIndex == operators.size() - 1;

    auto id = oper->operatorId();
    if (previousOperatorIsNotGpu and acceptsGpuInput(oper)) {
      auto planNode = getPlanNode(oper->planNodeId());
      replaceOp.push_back(
          std::make_unique<CudfFromVelox>(
              id, planNode->outputType(), ctx, planNode->id() + "-from-velox"));
    }
    if (not replaceOp.empty()) {
      // from-velox only, because need to inserted before current operator.
      operatorsOffset += replaceOp.size();
      [[maybe_unused]] auto replaced = driverFactory_.replaceOperators(
          driver_,
          replacingOperatorIndex,
          replacingOperatorIndex,
          std::move(replaceOp));
      replacingOperatorIndex = operatorIndex + operatorsOffset;
      replaceOp.clear();
      replacementsMade = true;
    }

    // This is used to denote if the current operator is kept or replaced.
    auto keepOperator = 0;
    // TableScan
    if (isTableScanSupported(oper)) {
      auto planNode = std::dynamic_pointer_cast<const core::TableScanNode>(
          getPlanNode(oper->planNodeId()));
      VELOX_CHECK(planNode != nullptr);
      keepOperator = 1;
    } else if (isJoinSupported(oper)) {
      if (auto joinBuildOp = dynamic_cast<exec::HashBuild*>(oper)) {
        auto planNode = std::dynamic_pointer_cast<const core::HashJoinNode>(
            getPlanNode(joinBuildOp->planNodeId()));
        VELOX_CHECK(planNode != nullptr);
        // From-Velox (optional)
        replaceOp.push_back(
            std::make_unique<CudfHashJoinBuild>(id, ctx, planNode));
      } else if (auto joinProbeOp = dynamic_cast<exec::HashProbe*>(oper)) {
        auto planNode = std::dynamic_pointer_cast<const core::HashJoinNode>(
            getPlanNode(joinProbeOp->planNodeId()));
        VELOX_CHECK(planNode != nullptr);
        // From-Velox (optional)
        replaceOp.push_back(
            std::make_unique<CudfHashJoinProbe>(id, ctx, planNode));
        // To-Velox (optional)
      }
    } else if (auto orderByOp = dynamic_cast<exec::OrderBy*>(oper)) {
      auto id = orderByOp->operatorId();
      auto planNode = std::dynamic_pointer_cast<const core::OrderByNode>(
          getPlanNode(orderByOp->planNodeId()));
      VELOX_CHECK(planNode != nullptr);
      replaceOp.push_back(std::make_unique<CudfOrderBy>(id, ctx, planNode));
    } else if (auto hashAggOp = dynamic_cast<exec::HashAggregation*>(oper)) {
      auto planNode = std::dynamic_pointer_cast<const core::AggregationNode>(
          getPlanNode(hashAggOp->planNodeId()));
      VELOX_CHECK(planNode != nullptr);
      replaceOp.push_back(
          std::make_unique<CudfHashAggregation>(id, ctx, planNode));
    } else if (isFilterProjectSupported(oper)) {
      auto filterProjectOp = dynamic_cast<exec::FilterProject*>(oper);
      auto projectPlanNode = std::dynamic_pointer_cast<const core::ProjectNode>(
          getPlanNode(filterProjectOp->planNodeId()));
      // When filter and project both exist, the FilterProject planNodeId id is
      // project node id, so we need FilterProject to report the FilterNode.
      auto filterPlanNode = filterProjectOp->filterNode();
      VELOX_CHECK(projectPlanNode != nullptr or filterPlanNode != nullptr);
      replaceOp.push_back(
          std::make_unique<CudfFilterProject>(
              id, ctx, filterPlanNode, projectPlanNode));
    } else if (auto limitOp = dynamic_cast<exec::Limit*>(oper)) {
      auto planNode = std::dynamic_pointer_cast<const core::LimitNode>(
          getPlanNode(limitOp->planNodeId()));
      VELOX_CHECK(planNode != nullptr);
      replaceOp.push_back(std::make_unique<CudfLimit>(id, ctx, planNode));
    } else if (
        auto localPartitionOp = dynamic_cast<exec::LocalPartition*>(oper)) {
      auto planNode = std::dynamic_pointer_cast<const core::LocalPartitionNode>(
          getPlanNode(localPartitionOp->planNodeId()));
      VELOX_CHECK(planNode != nullptr);
      if (CudfLocalPartition::shouldReplace(planNode)) {
        replaceOp.push_back(
            std::make_unique<CudfLocalPartition>(id, ctx, planNode));
      } else {
        // Round Robin batch-wise Partitioning is supported by CPU operator with
        // GPU Vector.
        keepOperator = 1;
      }
    } else if (
        auto localExchangeOp = dynamic_cast<exec::LocalExchange*>(oper)) {
      keepOperator = 1;
    } else if (
        auto assignUniqueIdOp = dynamic_cast<exec::AssignUniqueId*>(oper)) {
      auto planNode = std::dynamic_pointer_cast<const core::AssignUniqueIdNode>(
          getPlanNode(assignUniqueIdOp->planNodeId()));
      VELOX_CHECK(planNode != nullptr);
      replaceOp.push_back(
          std::make_unique<CudfAssignUniqueId>(
              id,
              ctx,
              planNode,
              planNode->taskUniqueId(),
              planNode->uniqueIdCounter()));
    } else {
      keepOperator = 1;
    }

    if (producesGpuOutput(oper) and
        (nextOperatorIsNotGpu or isLastOperatorOfTask)) {
      auto planNode = getPlanNode(oper->planNodeId());
      replaceOp.push_back(
          std::make_unique<CudfToVelox>(
              id, planNode->outputType(), ctx, planNode->id() + "-to-velox"));
    }

    if (CudfConfig::getInstance().debugEnabled) {
      LOG(INFO) << "Operator: ID " << oper->operatorId() << ": "
                << oper->toString().c_str()
                << ", keepOperator = " << keepOperator
                << ", replaceOp.size() = " << replaceOp.size() << "\n";
    }
    auto GpuReplacedOperator = [](const exec::Operator* op) {
      return isAnyOf<
          exec::OrderBy,
          exec::TopN,
          exec::HashAggregation,
          exec::HashProbe,
          exec::HashBuild,
          exec::StreamingAggregation,
          exec::Limit,
          exec::LocalPartition,
          exec::LocalExchange,
          exec::FilterProject,
          exec::AssignUniqueId>(op);
    };
    auto GpuRetainedOperator =
        [isTableScanSupported](const exec::Operator* op) {
          return isAnyOf<exec::Values, exec::LocalExchange, exec::CallbackSink>(
                     op) ||
              (isAnyOf<exec::TableScan>(op) && isTableScanSupported(op));
        };
    // If GPU operator is supported, then replaceOp should be non-empty and
    // the operator should not be retained Else the velox operator is retained
    // as-is
    auto condition = (GpuReplacedOperator(oper) && !replaceOp.empty() &&
                      keepOperator == 0) ||
        (GpuRetainedOperator(oper) && replaceOp.empty() && keepOperator == 1);
    if (CudfConfig::getInstance().debugEnabled) {
      LOG(INFO) << "GpuReplacedOperator = " << GpuReplacedOperator(oper)
                << ", GpuRetainedOperator = " << GpuRetainedOperator(oper)
                << std::endl;
      LOG(INFO) << "GPU operator condition = " << condition << std::endl;
    }
    if (!allowCpuFallback) {
      VELOX_CHECK(condition, "Replacement with cuDF operator failed");
    } else if (!condition) {
      LOG(WARNING)
          << "Replacement with cuDF operator failed. Falling back to CPU execution";
    }

    if (not replaceOp.empty()) {
      // ReplaceOp, to-velox.
      operatorsOffset += replaceOp.size() - 1 + keepOperator;
      [[maybe_unused]] auto replaced = driverFactory_.replaceOperators(
          driver_,
          replacingOperatorIndex + keepOperator,
          replacingOperatorIndex + 1,
          std::move(replaceOp));
      replacementsMade = true;
    }
  }

  if (CudfConfig::getInstance().debugEnabled) {
    operators = driver_.operators();
    LOG(INFO) << "Operators after adapting for cuDF: count ["
              << operators.size() << "]" << std::endl;
    for (auto& op : operators) {
      LOG(INFO) << "  Operator: ID " << op->operatorId() << ": "
                << op->toString() << std::endl;
    }
  }

  return replacementsMade;
}

std::shared_ptr<rmm::mr::device_memory_resource> mr_;

struct CudfDriverAdapter {
  CudfDriverAdapter(bool allowCpuFallback)
      : allowCpuFallback_{allowCpuFallback} {}

  // Call operator needed by DriverAdapter
  bool operator()(const exec::DriverFactory& factory, exec::Driver& driver) {
    if (!driver.driverCtx()->queryConfig().get<bool>(
            CudfConfig::kCudfEnabled, CudfConfig::getInstance().enabled) &&
        allowCpuFallback_) {
      return false;
    }
    auto state = CompileState(factory, driver);
    auto res = state.compile(allowCpuFallback_);
    return res;
  }

 private:
  bool allowCpuFallback_;
};

static bool isCudfRegistered = false;

bool cudfIsRegistered() {
  return isCudfRegistered;
}

void registerCudf() {
  if (cudfIsRegistered()) {
    return;
  }

  registerBuiltinFunctions(CudfConfig::getInstance().functionNamePrefix);

  CUDF_FUNC_RANGE();
  cudaFree(nullptr); // Initialize CUDA context at startup

  const std::string mrMode = CudfConfig::getInstance().memoryResource;
  auto mr = cudf_velox::createMemoryResource(
      mrMode, CudfConfig::getInstance().memoryPercent);
  cudf::set_current_device_resource(mr.get());
  mr_ = mr;

  exec::Operator::registerOperator(
      std::make_unique<CudfHashJoinBridgeTranslator>());
  CudfDriverAdapter cda{CudfConfig::getInstance().allowCpuFallback};
  exec::DriverAdapter cudfAdapter{kCudfAdapterName, {}, cda};
  exec::DriverFactory::registerAdapter(cudfAdapter);

  if (CudfConfig::getInstance().astExpressionEnabled) {
    registerAstEvaluator(CudfConfig::getInstance().astExpressionPriority);
  }

  isCudfRegistered = true;
}

void unregisterCudf() {
  mr_ = nullptr;
  exec::DriverFactory::adapters.erase(
      std::remove_if(
          exec::DriverFactory::adapters.begin(),
          exec::DriverFactory::adapters.end(),
          [](const exec::DriverAdapter& adapter) {
            return adapter.label == kCudfAdapterName;
          }),
      exec::DriverFactory::adapters.end());

  isCudfRegistered = false;
}

CudfConfig& CudfConfig::getInstance() {
  static CudfConfig instance;
  return instance;
}

void CudfConfig::initialize(
    std::unordered_map<std::string, std::string>&& config) {
  if (config.find(kCudfEnabled) != config.end()) {
    enabled = folly::to<bool>(config[kCudfEnabled]);
  }
  if (config.find(kCudfDebugEnabled) != config.end()) {
    debugEnabled = folly::to<bool>(config[kCudfDebugEnabled]);
  }
  if (config.find(kCudfMemoryResource) != config.end()) {
    memoryResource = config[kCudfMemoryResource];
  }
  if (config.find(kCudfMemoryPercent) != config.end()) {
    memoryPercent = folly::to<int32_t>(config[kCudfMemoryPercent]);
  }
  if (config.find(kCudfFunctionNamePrefix) != config.end()) {
    functionNamePrefix = config[kCudfFunctionNamePrefix];
  }
  if (config.find(kCudfAstExpressionEnabled) != config.end()) {
    astExpressionEnabled = folly::to<bool>(config[kCudfAstExpressionEnabled]);
  }
  if (config.find(kCudfAstExpressionPriority) != config.end()) {
    astExpressionPriority =
        folly::to<int32_t>(config[kCudfAstExpressionPriority]);
  }
  if (config.find(kCudfAllowCpuFallback) != config.end()) {
    allowCpuFallback = folly::to<bool>(config[kCudfAllowCpuFallback]);
  }
  if (config.find(kCudfLogFallback) != config.end()) {
    logFallback = folly::to<bool>(config[kCudfLogFallback]);
  }
}

} // namespace facebook::velox::cudf_velox
