//===--------------------------------------------------------------------------------*- C++ -*-===//
//                          _
//                         | |
//                       __| | __ ___      ___ ___
//                      / _` |/ _` \ \ /\ / / '_  |
//                     | (_| | (_| |\ V  V /| | | |
//                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
//
//
//  This file is distributed under the MIT License (MIT).
//  See LICENSE.txt for details.
//
//===------------------------------------------------------------------------------------------===//

#include "dawn/Compiler/DawnCompiler.h"
#include "dawn/CodeGen/CXXNaive/CXXNaiveCodeGen.h"
#include "dawn/CodeGen/CodeGen.h"
#include "dawn/CodeGen/Cuda/CudaCodeGen.h"
#include "dawn/CodeGen/GridTools/GTCodeGen.h"
#include "dawn/Optimizer/OptimizerContext.h"
#include "dawn/Optimizer/PassComputeStageExtents.h"
#include "dawn/Optimizer/PassDataLocalityMetric.h"
#include "dawn/Optimizer/PassFieldVersioning.h"
#include "dawn/Optimizer/PassInlining.h"
#include "dawn/Optimizer/PassMultiStageSplitter.h"
#include "dawn/Optimizer/PassPrintStencilGraph.h"
#include "dawn/Optimizer/PassSSA.h"
#include "dawn/Optimizer/PassSetBlockSize.h"
#include "dawn/Optimizer/PassSetBoundaryCondition.h"
#include "dawn/Optimizer/PassSetCaches.h"
#include "dawn/Optimizer/PassSetNonTempCaches.h"
#include "dawn/Optimizer/PassSetStageGraph.h"
#include "dawn/Optimizer/PassSetStageName.h"
#include "dawn/Optimizer/PassSetSyncStage.h"
#include "dawn/Optimizer/PassStageMerger.h"
#include "dawn/Optimizer/PassStageReordering.h"
#include "dawn/Optimizer/PassStageSplitter.h"
#include "dawn/Optimizer/PassStencilSplitter.h"
#include "dawn/Optimizer/PassTemporaryFirstAccess.h"
#include "dawn/Optimizer/PassTemporaryMerger.h"
#include "dawn/Optimizer/PassTemporaryToStencilFunction.h"
#include "dawn/Optimizer/PassTemporaryType.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Serialization/IIRSerializer.h"
#include "dawn/Support/EditDistance.h"
#include "dawn/Support/Logging.h"
#include "dawn/Support/StringSwitch.h"
#include "dawn/Support/StringUtil.h"
#include "dawn/Support/Unreachable.h"

namespace dawn {

namespace {

/// @brief Make a suggestion to the user if there is a small typo (only works with string options)
template <class T>
struct ComputeEditDistance {
  static std::string getSuggestion(const T& value, const std::vector<T>& possibleValues) {
    return "";
  }
};

template <>
struct ComputeEditDistance<std::string> {
  static std::string getSuggestion(const std::string& value,
                                   const std::vector<std::string>& possibleValues) {
    if(possibleValues.empty())
      return "";

    std::vector<unsigned> editDistances;
    std::transform(possibleValues.begin(), possibleValues.end(), std::back_inserter(editDistances),
                   [&](const std::string& val) { return computeEditDistance(value, val); });

    // Find minimum edit distance
    unsigned minEditDistance = editDistances[0], minEditDistanceIdx = 0;
    for(unsigned i = 1; i < editDistances.size(); ++i)
      if(editDistances[i] < minEditDistance) {
        minEditDistance = editDistances[i];
        minEditDistanceIdx = i;
      }

    return minEditDistance <= 2 ? "did you mean '" + possibleValues[minEditDistanceIdx] + "' ?"
                                : "";
  }
};
} // anonymous namespace

/// @brief Report a diagnostic concering an invalid Option
template <class T>
static DiagnosticsBuilder buildDiag(const std::string& option, const T& value, std::string reason,
                                    std::vector<T> possibleValues = std::vector<T>{}) {
  DiagnosticsBuilder diag(DiagnosticsKind::Error, SourceLocation());
  diag << "invalid value '" << value << "' of option '" << option << "'";

  if(!reason.empty()) {
    diag << ", " << reason;
  } else {
    auto suggestion = ComputeEditDistance<T>::getSuggestion(value, possibleValues);

    if(!suggestion.empty())
      diag << ", " << suggestion;
    else if(!possibleValues.empty())
      diag << ", possible values " << RangeToString()(possibleValues);
  }
  return diag;
}

static std::string remove_fileextension(std::string fullName, std::string extension) {
  std::string truncation = "";
  std::size_t pos = 0;
  while((pos = fullName.find(extension)) != std::string::npos) {
    truncation += fullName.substr(0, pos);
    fullName.erase(0, pos + extension.length());
  }
  return truncation;
}

DawnCompiler::DawnCompiler(Options* options) : diagnostics_(make_unique<DiagnosticsEngine>()) {
  options_ = options ? make_unique<Options>(*options) : make_unique<Options>();
}

std::unique_ptr<OptimizerContext> DawnCompiler::runOptimizer(std::shared_ptr<SIR> const& SIR) {
  // -reorder
  using ReorderStrategyKind = ReorderStrategy::ReorderStrategyKind;
  ReorderStrategyKind reorderStrategy = StringSwitch<ReorderStrategyKind>(options_->ReorderStrategy)
                                            .Case("none", ReorderStrategyKind::RK_None)
                                            .Case("greedy", ReorderStrategyKind::RK_Greedy)
                                            .Case("scut", ReorderStrategyKind::RK_Partitioning)
                                            .Default(ReorderStrategyKind::RK_Unknown);

  if(reorderStrategy == ReorderStrategyKind::RK_Unknown) {
    diagnostics_->report(
        buildDiag("-reorder", options_->ReorderStrategy, "", {"none", "greedy", "scut"}));
    return nullptr;
  }

  using MultistageSplitStrategy = PassMultiStageSplitter::MultiStageSplittingStrategy;
  MultistageSplitStrategy mssSplitStrategy;
  if(options_->MaxCutMSS) {
    mssSplitStrategy = MultistageSplitStrategy::SS_MaxCut;
  } else {
    mssSplitStrategy = MultistageSplitStrategy::SS_Optimized;
  }

  // -max-fields
  int maxFields = options_->MaxFieldsPerStencil;

  IIRSerializer::SerializationKind serializationKind = IIRSerializer::SK_Json;
  if(options_->SerializeIIR) { /*|| (options_->LoadSerialized != "")) {*/
    if(options_->IIRFormat == "json") {
      serializationKind = IIRSerializer::SK_Json;
    } else if(options_->IIRFormat == "byte") {
      serializationKind = IIRSerializer::SK_Byte;

    } else {
      dawn_unreachable("Unknown SIRFormat option");
    }
  }

  // Initialize optimizer
  std::unique_ptr<OptimizerContext> optimizer =
      make_unique<OptimizerContext>(getDiagnostics(), getOptions(), SIR);
  PassManager& passManager = optimizer->getPassManager();

  // Setup pass interface
  optimizer->checkAndPushBack<PassInlining>(true, PassInlining::IK_InlineProcedures);
  // This pass is currently broken and needs to be redesigned before it can be enabled
  //  optimizer->checkAndPushBack<PassTemporaryFirstAccss>();
  optimizer->checkAndPushBack<PassFieldVersioning>();
  optimizer->checkAndPushBack<PassSSA>();
  optimizer->checkAndPushBack<PassMultiStageSplitter>(mssSplitStrategy);
  optimizer->checkAndPushBack<PassStageSplitter>();
  optimizer->checkAndPushBack<PassPrintStencilGraph>();
  optimizer->checkAndPushBack<PassTemporaryType>();
  optimizer->checkAndPushBack<PassSetStageName>();
  optimizer->checkAndPushBack<PassSetStageGraph>();
  optimizer->checkAndPushBack<PassStageReordering>(reorderStrategy);
  optimizer->checkAndPushBack<PassStageMerger>();
  optimizer->checkAndPushBack<PassStencilSplitter>(maxFields);
  optimizer->checkAndPushBack<PassTemporaryType>();
  optimizer->checkAndPushBack<PassTemporaryMerger>();
  optimizer->checkAndPushBack<PassInlining>(
      (getOptions().InlineSF || getOptions().PassTmpToFunction),
      PassInlining::IK_ComputationsOnTheFly);
  optimizer->checkAndPushBack<PassTemporaryToStencilFunction>();
  optimizer->checkAndPushBack<PassSetNonTempCaches>();
  optimizer->checkAndPushBack<PassSetCaches>();
  optimizer->checkAndPushBack<PassComputeStageExtents>();
  optimizer->checkAndPushBack<PassSetBoundaryCondition>();
  optimizer->checkAndPushBack<PassSetBlockSize>();
  optimizer->checkAndPushBack<PassDataLocalityMetric>();
  optimizer->checkAndPushBack<PassSetSyncStage>();

  DAWN_LOG(INFO) << "All the passes ran with the current command line arugments:";
  for(const auto& a : passManager.getPasses()) {
    DAWN_LOG(INFO) << a->getName();
  }
  // Run optimization passes
  for(auto& stencil : optimizer->getStencilInstantiationMap()) {
    std::shared_ptr<iir::StencilInstantiation> instantiation = stencil.second;
    DAWN_LOG(INFO) << "Starting Optimization and Analysis passes for `" << instantiation->getName()
                   << "` ...";
    if(!passManager.runAllPassesOnStecilInstantiation(instantiation))
      return nullptr;
    DAWN_LOG(INFO) << "Done with Optimization and Analysis passes for `" << instantiation->getName()
                   << "`";

    if(options_->SerializeIIR) {
      IIRSerializer::serialize(
          remove_fileextension(instantiation->getMetaData().getFileName(), ".cpp") + ".iir",
          instantiation, serializationKind);
    }

    stencil.second = instantiation;
  }

  return optimizer;
}

std::unique_ptr<codegen::TranslationUnit> DawnCompiler::compile(const std::shared_ptr<SIR>& SIR) {
  diagnostics_->clear();
  diagnostics_->setFilename(SIR->Filename);

  // Check if options are valid

  // -max-halo
  if(options_->MaxHaloPoints < 0) {
    diagnostics_->report(buildDiag("-max-halo", options_->MaxHaloPoints,
                                   "maximum number of allowed halo points must be >= 0"));
    return nullptr;
  }

  // Initialize optimizer
  auto optimizer = runOptimizer(SIR);

  if(diagnostics_->hasErrors()) {
    DAWN_LOG(INFO) << "Errors occured. Skipping code generation.";
    return nullptr;
  }

  // Generate code
  std::unique_ptr<codegen::CodeGen> CG;

  if(options_->Backend == "gridtools") {
    CG = make_unique<codegen::gt::GTCodeGen>(optimizer.get());
  } else if(options_->Backend == "c++-naive") {
    CG = make_unique<codegen::cxxnaive::CXXNaiveCodeGen>(optimizer.get());
  } else if(options_->Backend == "cuda") {
    CG = make_unique<codegen::cuda::CudaCodeGen>(optimizer.get());
  } else if(options_->Backend == "c++-opt") {
    dawn_unreachable("GTClangOptCXX not supported yet");
  } else {
    diagnostics_->report(buildDiag("-backend", options_->Backend,
                                   "backend options must be : " +
                                       dawn::RangeToString(", ", "", "")(std::vector<std::string>{
                                           "gridtools", "c++-naive", "c++-opt"})));
    return nullptr;
  }

  return CG->generateCode();
}

const DiagnosticsEngine& DawnCompiler::getDiagnostics() const { return *diagnostics_.get(); }
DiagnosticsEngine& DawnCompiler::getDiagnostics() { return *diagnostics_.get(); }

const Options& DawnCompiler::getOptions() const { return *options_.get(); }
Options& DawnCompiler::getOptions() { return *options_.get(); }

} // namespace dawn
