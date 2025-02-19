﻿//===--------------------------------------------------------------------------------*- C++ -*-===//
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

#include "dawn/Optimizer/PassTemporaryToStencilFunction.h"
#include "dawn/IIR/DependencyGraphAccesses.h"
#include "dawn/IIR/IIRNodeIterator.h"
#include "dawn/IIR/Stencil.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/Optimizer/AccessComputation.h"
#include "dawn/Optimizer/OptimizerContext.h"
#include "dawn/Optimizer/StatementMapper.h"
#include "dawn/SIR/AST.h"
#include "dawn/SIR/ASTVisitor.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/RemoveIf.hpp"

namespace dawn {

namespace {
sir::Interval intervalToSIRInterval(iir::Interval interval) {
  return sir::Interval(interval.lowerLevel(), interval.upperLevel(), interval.lowerOffset(),
                       interval.upperOffset());
}

iir::Interval sirIntervalToInterval(sir::Interval interval) {
  return iir::Interval(interval.LowerLevel, interval.UpperLevel, interval.LowerOffset,
                       interval.UpperOffset);
}

/// @brief some properties of the temporary being replaced
struct TemporaryFunctionProperties {
  std::shared_ptr<StencilFunCallExpr>
      stencilFunCallExpr_;        ///< stencil function call that will replace the tmp
  std::vector<int> accessIDArgs_; ///< access IDs of the args that are needed to compute the tmp
  std::shared_ptr<sir::StencilFunction>
      sirStencilFunction_; ///< sir stencil function of the tmp being created
  std::shared_ptr<FieldAccessExpr>
      tmpFieldAccessExpr_; ///< FieldAccessExpr of the tmp captured for replacement
  iir::Interval interval_; ///< interval for which the tmp definition is valid
};

///
/// @brief The LocalVariablePromotion class identifies local variables that need to be promoted to
/// temporaries because of a tmp->stencilfunction conversion
/// In the following example:
/// double a=0;
/// tmp = a*2;
/// local variable a will have to be promoted to temporary, since tmp will be evaluated on-the-fly
/// with extents
///
class LocalVariablePromotion : public ASTVisitorPostOrder, public NonCopyable {
protected:
  const iir::StencilMetaInformation& metadata_;
  const iir::Stencil& stencil_;
  const std::unordered_map<int, iir::Stencil::FieldInfo>& fields_;
  const SkipIDs& skipIDs_;
  std::unordered_set<int>& localVarAccessIDs_;
  bool activate_ = false;

public:
  LocalVariablePromotion(const iir::StencilMetaInformation& metadata, const iir::Stencil& stencil,
                         const std::unordered_map<int, iir::Stencil::FieldInfo>& fields,
                         const SkipIDs& skipIDs, std::unordered_set<int>& localVarAccessIDs)
      : metadata_(metadata), stencil_(stencil), fields_(fields), skipIDs_(skipIDs),
        localVarAccessIDs_(localVarAccessIDs) {}

  virtual ~LocalVariablePromotion() override {}

  virtual bool preVisitNode(std::shared_ptr<VarAccessExpr> const& expr) override {
    // TODO if inside stencil function we should get it from stencilfun

    // we process this var access only after activation, i.e. after a "tmp= ..." pattern has been
    // found. This is important to protect against var accesses in a var decl like "float var = "
    // that could occur before the visit of the assignment expression
    if(activate_) {
      localVarAccessIDs_.emplace(metadata_.getAccessIDFromExpr(expr));
    }
    return true;
  }

  /// @brief capture a tmp computation
  virtual bool preVisitNode(std::shared_ptr<AssignmentExpr> const& expr) override {

    if(isa<FieldAccessExpr>(*(expr->getLeft()))) {
      int accessID = metadata_.getAccessIDFromExpr(expr->getLeft());
      DAWN_ASSERT(fields_.count(accessID));
      const iir::Field& field = fields_.at(accessID).field;

      bool skip = true;
      // If at least in one ms, the id is not skipped, we will process the local var -> tmp
      // promotion
      for(const auto& ms : stencil_.getChildren()) {
        if(!ms->getFields().count(accessID)) {
          continue;
        }
        if(!skipIDs_.skipID(ms->getID(), accessID)) {
          skip = false;
          break;
        }
      }
      if(skip) {
        return false;
      }
      if(!metadata_.isAccessType(iir::FieldAccessType::FAT_StencilTemporary, accessID))
        return false;

      if(field.getExtents().isHorizontalPointwise())
        return false;

      activate_ = true;
      return true;
    }

    return false;
  }
};

static std::string offsetToString(int a) {
  return ((a < 0) ? "minus" : "") + std::to_string(std::abs(a));
}

/// @brief create the name of a newly created stencil function associated to a tmp computations
std::string makeOnTheFlyFunctionName(const std::shared_ptr<FieldAccessExpr>& expr,
                                     const iir::Interval& interval) {
  return expr->getName() + "_OnTheFly_" + interval.toStringGen() + "_i" +
         offsetToString(expr->getOffset()[0]) + "_j" + offsetToString(expr->getOffset()[1]) + "_k" +
         offsetToString(expr->getOffset()[2]);
}

/// @brief create the name of a newly created stencil function associated to a tmp computations
std::string makeOnTheFlyFunctionCandidateName(const std::shared_ptr<FieldAccessExpr>& expr,
                                              const iir::Interval& interval) {
  return expr->getName() + "_OnTheFly_" + interval.toStringGen();
}

std::string makeOnTheFlyFunctionCandidateName(const std::string fieldName,
                                              const sir::Interval& interval) {
  return fieldName + "_OnTheFly_" + sirIntervalToInterval(interval).toStringGen();
}

/// @brief visitor that will detect assignment (i.e. computations) to a temporary,
/// it will create a sir::StencilFunction out of this computation, and replace the assignment
/// expression in the AST by a NOExpr.
class TmpAssignment : public ASTVisitorPostOrder, public NonCopyable {
protected:
  const iir::StencilMetaInformation& metadata_;
  sir::Interval interval_; // interval where the function declaration will be defined
  std::shared_ptr<sir::StencilFunction>
      tmpFunction_;            // sir function with the declaration of the tmp computation
  std::vector<int> accessIDs_; // accessIDs of the accesses that form the tmp = ... expression, that
                               // will become arguments of the stencil fn

  std::shared_ptr<FieldAccessExpr> tmpFieldAccessExpr_ = nullptr; // the field access expr of the
                                                                  // temporary that is captured and
                                                                  // being replaced by stencil fn
  const std::set<int>& skipAccessIDsOfMS_; // list of ids that will be skipped, since they dont
                                           // fulfil the requirements, like they contain cycle
                                           // dependencies, etc

public:
  TmpAssignment(const iir::StencilMetaInformation& metadata, sir::Interval const& interval,
                const std::set<int>& skipAccessIDsOfMS)
      : metadata_(metadata), interval_(interval), tmpFunction_(nullptr),
        skipAccessIDsOfMS_(skipAccessIDsOfMS) {}

  virtual ~TmpAssignment() {}

  const std::vector<int>& getAccessIDs() const { return accessIDs_; }

  std::shared_ptr<FieldAccessExpr> getTemporaryFieldAccessExpr() { return tmpFieldAccessExpr_; }

  std::shared_ptr<sir::StencilFunction> temporaryStencilFunction() { return tmpFunction_; }

  bool foundTemporaryToReplace() { return (tmpFunction_ != nullptr); }

  /// @brief pre visit the node. The assignment expr visit will only continue processing the visitor
  /// for the right hand side of the =. Therefore all accesses capture here correspond to arguments
  /// for computing the tmp. They are captured as arguments of the stencil function being created
  virtual bool preVisitNode(std::shared_ptr<FieldAccessExpr> const& expr) override {
    DAWN_ASSERT(tmpFunction_);
    for(int idx : expr->getArgumentMap()) {
      DAWN_ASSERT(idx == -1);
    }
    for(int off : expr->getArgumentOffset())
      DAWN_ASSERT(off == 0);

    // record the field access as an argument to the stencil funcion
    if(!tmpFunction_->hasArg(expr->getName()) && expr != tmpFieldAccessExpr_) {

      int genLineKey = static_cast<std::underlying_type<SourceLocation::ReservedSL>::type>(
          SourceLocation::ReservedSL::SL_Generated);
      tmpFunction_->Args.push_back(
          std::make_shared<sir::Field>(expr->getName(), SourceLocation(genLineKey, genLineKey)));

      accessIDs_.push_back(metadata_.getAccessIDFromExpr(expr));
    }
    // continue traversing
    return true;
  }

  virtual bool preVisitNode(std::shared_ptr<VarAccessExpr> const& expr) override {
    DAWN_ASSERT(tmpFunction_);
    if(!metadata_.isAccessType(iir::FieldAccessType::FAT_GlobalVariable,
                               metadata_.getAccessIDFromExpr(expr))) {
      // record the var access as an argument to the stencil funcion
      dawn_unreachable_internal("All the var access should have been promoted to temporaries");
    }
    return true;
  }

  virtual bool preVisitNode(std::shared_ptr<VarDeclStmt> const& stmt) override {
    // a vardecl is assigning to a local variable, since the local variable promotion did not
    // promoted this to a tmp, we assume here the rules for tmp->function replacement were not
    // fulfil
    return false;
  }

  /// @brief capture a tmp computation
  virtual bool preVisitNode(std::shared_ptr<AssignmentExpr> const& expr) override {
    if(isa<FieldAccessExpr>(*(expr->getLeft()))) {
      // return and stop traversing the AST if the left hand of the =  is not a temporary
      int accessID = metadata_.getAccessIDFromExpr(expr->getLeft());
      if(skipAccessIDsOfMS_.count(accessID)) {
        return false;
      }
      tmpFieldAccessExpr_ = std::dynamic_pointer_cast<FieldAccessExpr>(expr->getLeft());

      // otherwise we create a new stencil function
      std::string tmpFieldName = metadata_.getFieldNameFromAccessID(accessID);
      tmpFunction_ = std::make_shared<sir::StencilFunction>();

      tmpFunction_->Name = makeOnTheFlyFunctionCandidateName(tmpFieldName, interval_);
      tmpFunction_->Loc = expr->getSourceLocation();
      tmpFunction_->Intervals.push_back(std::make_shared<sir::Interval>(interval_));

      return true;
    }
    return false;
  }
  ///@brief once the "tmp= fn(args)" has been captured, the new stencil function to compute the
  /// temporary is finalized and the assignment is replaced by a NOExpr
  virtual std::shared_ptr<Expr>
  postVisitNode(std::shared_ptr<AssignmentExpr> const& expr) override {
    if(isa<FieldAccessExpr>(*(expr->getLeft()))) {
      DAWN_ASSERT(tmpFieldAccessExpr_);
      const int accessID = metadata_.getAccessIDFromExpr(tmpFieldAccessExpr_);
      if(!metadata_.isAccessType(iir::FieldAccessType::FAT_StencilTemporary, accessID))
        return expr;

      DAWN_ASSERT(tmpFunction_);

      auto functionExpr = expr->getRight()->clone();
      auto retStmt = std::make_shared<ReturnStmt>(functionExpr);

      std::shared_ptr<BlockStmt> root = std::make_shared<BlockStmt>();
      root->push_back(retStmt);
      std::shared_ptr<AST> ast = std::make_shared<AST>(root);
      tmpFunction_->Asts.push_back(ast);

      return std::make_shared<NOPExpr>();
    }
    return expr;
  }
};

/// @brief visitor that will capture all read accesses to the temporary. The offset used to access
/// the temporary is extracted and pass to all the stencil function arguments. A new stencil
/// function instantiation is then created and the field access expression replaced by a stencil
/// function call
class TmpReplacement : public ASTVisitorPostOrder, public NonCopyable {
  // struct to store the stencil function instantiation of each parameter of a stencil function that
  // is at the same time instantiated as a stencil function
  struct NestedStencilFunctionArgs {
    int currentPos_ = 0;
    std::unordered_map<int, std::shared_ptr<iir::StencilFunctionInstantiation>> stencilFun_;
  };

protected:
  const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation_;
  iir::StencilMetaInformation& metadata_;
  std::unordered_map<int, TemporaryFunctionProperties> const& temporaryFieldAccessIDToFunctionCall_;
  const iir::Interval interval_;
  const sir::Interval sirInterval_;
  std::shared_ptr<std::vector<sir::StencilCall*>> stackTrace_;
  std::shared_ptr<Expr> skip_;

  // the prop of the arguments of nested stencil functions
  std::stack<bool> replaceInNestedFun_;

  unsigned int numTmpReplaced_ = 0;

  std::unordered_map<std::shared_ptr<FieldAccessExpr>,
                     std::shared_ptr<iir::StencilFunctionInstantiation>>
      tmpToStencilFunctionMap_;

public:
  TmpReplacement(const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation,
                 std::unordered_map<int, TemporaryFunctionProperties> const&
                     temporaryFieldAccessIDToFunctionCall,
                 const iir::Interval& interval,
                 std::shared_ptr<std::vector<sir::StencilCall*>> stackTrace)
      : stencilInstantiation_(stencilInstantiation), metadata_(stencilInstantiation->getMetaData()),
        temporaryFieldAccessIDToFunctionCall_(temporaryFieldAccessIDToFunctionCall),
        interval_(interval), sirInterval_(intervalToSIRInterval(interval)),
        stackTrace_(stackTrace) {}

  virtual ~TmpReplacement() {}

  unsigned int getNumTmpReplaced() { return numTmpReplaced_; }
  void resetNumTmpReplaced() { numTmpReplaced_ = 0; }

  virtual bool preVisitNode(std::shared_ptr<StencilFunCallExpr> const& expr) override {
    // we check which arguments of the stencil function are also a stencil function call
    bool doReplaceTmp = false;
    for(auto arg : expr->getArguments()) {
      if(isa<FieldAccessExpr>(*arg)) {
        int accessID = metadata_.getAccessIDFromExpr(arg);
        if(temporaryFieldAccessIDToFunctionCall_.count(accessID)) {
          doReplaceTmp = true;
        }
      }
    }
    if(doReplaceTmp)
      replaceInNestedFun_.push(true);
    else
      replaceInNestedFun_.push(false);

    return true;
  }

  virtual std::shared_ptr<Expr>
  postVisitNode(std::shared_ptr<StencilFunCallExpr> const& expr) override {
    // at the post visit of a stencil function node, we will replace the arguments to "tmp" fields
    // by stecil function calls
    std::shared_ptr<iir::StencilFunctionInstantiation> thisStencilFun =
        metadata_.getStencilFunctionInstantiation(expr);

    if(!replaceInNestedFun_.top())
      return expr;

    // we need to remove the previous stencil function that had "tmp" field as argument from the
    // registry, before we replace it with a StencilFunCallExpr (that computes "tmp") argument
    metadata_.deregisterStencilFunction(thisStencilFun);
    // reset the use of nested function calls to continue using the visitor
    replaceInNestedFun_.pop();

    return expr;
  }

  /// @brief previsit the access to a temporary. Finalize the stencil function instantiation and
  /// recompute its <statement,accesses> pairs
  virtual bool preVisitNode(std::shared_ptr<AssignmentExpr> const& expr) override {
    // we would like to identify fields that are lhs of an assignment expr, so that we skip them and
    // dont replace them
    if(isa<FieldAccessExpr>(expr->getLeft().get())) {
      skip_ = expr->getLeft();
    }
    return true;
  }

  bool replaceFieldByFunction(const std::shared_ptr<FieldAccessExpr>& expr) {
    int accessID = metadata_.getAccessIDFromExpr(expr);
    if(!temporaryFieldAccessIDToFunctionCall_.count(accessID)) {
      return false;
    }
    const auto& tempFuncProperties = temporaryFieldAccessIDToFunctionCall_.at(accessID);

    return (expr != skip_) && tempFuncProperties.interval_.contains(interval_);
  }

  /// @brief previsit the access to a temporary. Finalize the stencil function instantiation and
  /// recompute its <statement,accesses> pairs
  virtual bool preVisitNode(std::shared_ptr<FieldAccessExpr> const& expr) override {
    int accessID = metadata_.getAccessIDFromExpr(expr);

    if(!replaceFieldByFunction(expr)) {
      return true;
    }

    const auto& tempFuncProperties = temporaryFieldAccessIDToFunctionCall_.at(accessID);

    // TODO we need to version to tmp function generation, in case tmp is recomputed multiple
    // times
    std::string callee = makeOnTheFlyFunctionCandidateName(expr, interval_);
    std::shared_ptr<iir::StencilFunctionInstantiation> stencilFun =
        metadata_.getStencilFunctionInstantiationCandidate(callee, interval_);

    std::string fnClone = makeOnTheFlyFunctionName(expr, interval_);

    // retrieve the sir stencil function definition
    std::shared_ptr<sir::StencilFunction> sirStencilFunction =
        tempFuncProperties.sirStencilFunction_;

    // we create a new sir stencil function, since its name is demangled from the offsets.
    // for example, for a tmp(i+1) the stencil function is named as tmp_OnTheFly_iplus1
    std::shared_ptr<sir::StencilFunction> sirStencilFunctionInstance =
        std::make_shared<sir::StencilFunction>(*sirStencilFunction);

    sirStencilFunctionInstance->Name = fnClone;

    // TODO is this really needed, we only change the name, can we map multiple function
    // instantiations (i.e. different offsets) to the same sir stencil function
    // insert the sir::stencilFunction into the StencilInstantiation
    stencilInstantiation_->getIIR()->insertStencilFunction(sirStencilFunctionInstance);

    // we clone the stencil function instantiation of the candidate so that each instance of the st
    // function has its own private copy of the expressions (i.e. ast)
    std::shared_ptr<iir::StencilFunctionInstantiation> cloneStencilFun =
        metadata_.cloneStencilFunctionCandidate(stencilFun, fnClone);

    auto& accessIDsOfArgs = tempFuncProperties.accessIDArgs_;

    // here we create the arguments of the stencil function instantiation.
    // find the accessID of all args, and create a new FieldAccessExpr with an offset
    // corresponding
    // to the offset used to access the temporary
    for(auto accessID_ : (accessIDsOfArgs)) {
      std::shared_ptr<FieldAccessExpr> arg = std::make_shared<FieldAccessExpr>(
          metadata_.getFieldNameFromAccessID(accessID_), expr->getOffset());
      cloneStencilFun->getExpression()->insertArgument(arg);

      metadata_.insertExprToAccessID(arg, accessID_);
    }

    for(int idx : expr->getArgumentMap()) {
      DAWN_ASSERT(idx == -1);
    }
    for(int off : expr->getArgumentOffset())
      DAWN_ASSERT(off == 0);

    const auto& argToAccessIDMap = stencilFun->ArgumentIndexToCallerAccessIDMap();
    for(auto pair : argToAccessIDMap) {
      int accessID_ = pair.second;
      cloneStencilFun->setCallerInitialOffsetFromAccessID(accessID_, expr->getOffset());
    }

    metadata_.finalizeStencilFunctionSetup(cloneStencilFun);
    std::unordered_map<std::string, int> fieldsMap;

    const auto& arguments = cloneStencilFun->getArguments();
    for(std::size_t argIdx = 0; argIdx < arguments.size(); ++argIdx) {
      if(sir::Field* field = dyn_cast<sir::Field>(arguments[argIdx].get())) {
        int AccessID = cloneStencilFun->getCallerAccessIDOfArgField(argIdx);
        fieldsMap[field->Name] = AccessID;
      }
    }

    auto asir = std::make_shared<SIR>();
    for(const auto& sf : stencilInstantiation_->getIIR()->getStencilFunctions()) {
      asir->StencilFunctions.push_back(sf);
    }

    // recompute the list of <statement, accesses> pairs
    StatementMapper statementMapper(asir, stencilInstantiation_.get(), stackTrace_,
                                    *(cloneStencilFun->getDoMethod()), interval_, fieldsMap,
                                    cloneStencilFun);

    cloneStencilFun->getAST()->accept(statementMapper);

    // final checks
    cloneStencilFun->checkFunctionBindings();

    // register the FieldAccessExpr -> StencilFunctionInstantation into a map for future
    // replacement
    // of the node in the post visit
    DAWN_ASSERT(!tmpToStencilFunctionMap_.count(expr));
    tmpToStencilFunctionMap_[expr] = cloneStencilFun;

    return true;
  }

  /// @brief replace the access to a temporary by a stencil function call expression
  virtual std::shared_ptr<Expr>
  postVisitNode(std::shared_ptr<FieldAccessExpr> const& expr) override {
    // if the field access is not identified as a temporary being replaced just return without
    // substitution
    if(!replaceFieldByFunction(expr)) {
      return expr;
    }

    // TODO we need to version to tmp function generation, in case tmp is recomputed multiple
    // times
    std::string callee = makeOnTheFlyFunctionName(expr, interval_);

    DAWN_ASSERT(tmpToStencilFunctionMap_.count(expr));

    auto stencilFunCall = tmpToStencilFunctionMap_.at(expr)->getExpression();

    numTmpReplaced_++;
    return stencilFunCall;
  }
};

} // anonymous namespace

PassTemporaryToStencilFunction::PassTemporaryToStencilFunction()
    : Pass("PassTemporaryToStencilFunction") {}

SkipIDs PassTemporaryToStencilFunction::computeSkipAccessIDs(
    const std::unique_ptr<iir::Stencil>& stencilPtr,
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) const {

  const auto& metadata = stencilInstantiation->getMetaData();
  SkipIDs skipIDs;
  // Iterate multi-stages backwards in order to identify local variables that need to be promoted
  // to temporaries
  for(const auto& multiStage : stencilPtr->getChildren()) {
    iir::DependencyGraphAccesses graph(stencilInstantiation->getMetaData());
    for(const auto& doMethod : iterateIIROver<iir::DoMethod>(*multiStage)) {
      for(const auto& stmt : doMethod->getChildren()) {
        graph.insertStatementAccessesPair(stmt);
      }
    }
    // TODO this is crashing for the divergene helper
    //    graph.toDot("PP");

    // all the fields with self-dependencies are discarded, e.g. w += w[k+1]
    skipIDs.insertAccessIDsOfMS(multiStage->getID(), graph.computeIDsWithCycles());
    for(const auto& fieldPair : multiStage->getFields()) {
      const auto& field = fieldPair.second;

      // we dont consider non temporary fields
      if(!metadata.isAccessType(iir::FieldAccessType::FAT_StencilTemporary, field.getAccessID())) {
        skipIDs.appendAccessIDsToMS(multiStage->getID(), field.getAccessID());
        continue;
      }
      // The scope of the temporary has to be a MS.
      // TODO Note the algorithm is not mathematically
      // complete here. We need to make sure that first access is always a write
      if(field.getIntend() != iir::Field::IK_InputOutput) {
        skipIDs.appendAccessIDsToMS(multiStage->getID(), field.getAccessID());
        continue;
      }
      // we require that there are no vertical extents, otherwise the definition of a tmp might be
      // in a different interval than where it is used
      auto extents = field.getExtents();
      if(!extents.isVerticalPointwise()) {
        skipIDs.appendAccessIDsToMS(multiStage->getID(), field.getAccessID());
        continue;
      }
      if(extents.isHorizontalPointwise()) {
        skipIDs.appendAccessIDsToMS(multiStage->getID(), field.getAccessID());
        continue;
      }
    }
  }

  return skipIDs;
}

bool PassTemporaryToStencilFunction::run(
    const std::shared_ptr<iir::StencilInstantiation>& stencilInstantiation) {

  const auto& metadata = stencilInstantiation->getMetaData();
  OptimizerContext* context = stencilInstantiation->getOptimizerContext();

  if(!context->getOptions().PassTmpToFunction)
    return true;

  DAWN_ASSERT(context);

  for(const auto& stencilPtr : stencilInstantiation->getStencils()) {
    const auto& fields = stencilPtr->getFields();

    SkipIDs skipIDs = computeSkipAccessIDs(stencilPtr, stencilInstantiation);

    std::unordered_set<int> localVarAccessIDs;
    LocalVariablePromotion localVariablePromotion(metadata, *stencilPtr, fields, skipIDs,
                                                  localVarAccessIDs);

    for(auto multiStageIt = stencilPtr->childrenRBegin();
        multiStageIt != stencilPtr->childrenREnd(); ++multiStageIt) {

      for(auto stageIt = (*multiStageIt)->childrenRBegin();
          stageIt != (*multiStageIt)->childrenREnd(); ++stageIt) {

        for(auto doMethodIt = (*stageIt)->childrenRBegin();
            doMethodIt != (*stageIt)->childrenREnd(); doMethodIt++) {
          for(auto stmtAccessPairIt = (*doMethodIt)->childrenRBegin();
              stmtAccessPairIt != (*doMethodIt)->childrenREnd(); stmtAccessPairIt++) {
            const std::shared_ptr<Statement> stmt = (*stmtAccessPairIt)->getStatement();

            stmt->ASTStmt->acceptAndReplace(localVariablePromotion);
          }
        }
      }
    }

    // perform the promotion "local var"->temporary
    for(auto varID : localVarAccessIDs) {
      if(metadata.isAccessType(iir::FieldAccessType::FAT_GlobalVariable, varID))
        continue;

      stencilInstantiation->promoteLocalVariableToTemporaryField(
          stencilPtr.get(), varID, stencilPtr->getLifetime(varID),
          iir::TemporaryScope::TS_StencilTemporary);
    }

    skipIDs = computeSkipAccessIDs(stencilPtr, stencilInstantiation);

    // Iterate multi-stages for the replacement of temporaries by stencil functions
    for(const auto& multiStage : stencilPtr->getChildren()) {
      auto multiInterval = multiStage->computePartitionOfIntervals();
      for(const auto& interval : multiInterval.getIntervals()) {

        auto skipAccessIDsOfMS = skipIDs.accessIDs.at(multiStage->getID());

        std::unordered_map<int, TemporaryFunctionProperties> temporaryFieldExprToFunction;

        for(const auto& stagePtr : multiStage->getChildren()) {
          bool isATmpReplaced = false;
          for(const auto& doMethodPtr : stagePtr->getChildren()) {
            if(!doMethodPtr->getInterval().overlaps(interval)) {
              continue;
            }

            for(const auto& stmtAccessPair : doMethodPtr->getChildren()) {
              const std::shared_ptr<Statement> stmt = stmtAccessPair->getStatement();

              DAWN_ASSERT((stmt->ASTStmt->getKind() != Stmt::SK_ReturnStmt) &&
                          (stmt->ASTStmt->getKind() != Stmt::SK_StencilCallDeclStmt) &&
                          (stmt->ASTStmt->getKind() != Stmt::SK_VerticalRegionDeclStmt) &&
                          (stmt->ASTStmt->getKind() != Stmt::SK_BoundaryConditionDeclStmt));

              // We exclude blocks or If/Else stmt
              if((stmt->ASTStmt->getKind() != Stmt::SK_ExprStmt) &&
                 (stmt->ASTStmt->getKind() != Stmt::SK_VarDeclStmt)) {
                continue;
              }

              {
                // TODO catch a temp expr
                const iir::Interval& doMethodInterval = doMethodPtr->getInterval();
                const sir::Interval sirInterval = intervalToSIRInterval(interval);

                // run the replacer visitor
                TmpReplacement tmpReplacement(stencilInstantiation, temporaryFieldExprToFunction,
                                              interval, stmt->StackTrace);
                stmt->ASTStmt->acceptAndReplace(tmpReplacement);

                // flag if a least a tmp has been replaced within this stage
                isATmpReplaced = isATmpReplaced || (tmpReplacement.getNumTmpReplaced() != 0);

                if(tmpReplacement.getNumTmpReplaced() != 0) {

                  iir::DoMethod tmpStmtDoMethod(doMethodInterval, metadata);

                  auto asir = std::make_shared<SIR>();
                  for(const auto sf : stencilInstantiation->getIIR()->getStencilFunctions()) {
                    asir->StencilFunctions.push_back(sf);
                  }
                  StatementMapper statementMapper(
                      asir, stencilInstantiation.get(), stmt->StackTrace, tmpStmtDoMethod,
                      sirInterval, stencilInstantiation->getMetaData().getNameToAccessIDMap(),
                      nullptr);

                  std::shared_ptr<BlockStmt> blockStmt = std::make_shared<BlockStmt>(
                      std::vector<std::shared_ptr<Stmt>>{stmt->ASTStmt});
                  blockStmt->accept(statementMapper);

                  DAWN_ASSERT(tmpStmtDoMethod.getChildren().size() == 1);

                  std::unique_ptr<iir::StatementAccessesPair>& stmtPair =
                      *(tmpStmtDoMethod.childrenBegin());
                  computeAccesses(stencilInstantiation.get(), stmtPair);

                  doMethodPtr->replace(stmtAccessPair, stmtPair);
                  doMethodPtr->update(iir::NodeUpdateType::level);
                }

                // find patterns like tmp = fn(args)...;
                TmpAssignment tmpAssignment(metadata, sirInterval, skipAccessIDsOfMS);
                stmt->ASTStmt->acceptAndReplace(tmpAssignment);
                if(tmpAssignment.foundTemporaryToReplace()) {
                  std::shared_ptr<sir::StencilFunction> stencilFunction =
                      tmpAssignment.temporaryStencilFunction();
                  std::shared_ptr<AST> ast = stencilFunction->getASTOfInterval(sirInterval);

                  DAWN_ASSERT(ast);
                  DAWN_ASSERT(stencilFunction);

                  std::shared_ptr<StencilFunCallExpr> stencilFunCallExpr =
                      std::make_shared<StencilFunCallExpr>(stencilFunction->Name);

                  // all the temporary computations captured are stored in this map of <ID, tmp
                  // properties>
                  // for later use of the replacer visitor
                  const int accessID =
                      metadata.getAccessIDFromExpr(tmpAssignment.getTemporaryFieldAccessExpr());

                  if(!temporaryFieldExprToFunction.count(accessID)) {
                    temporaryFieldExprToFunction.emplace(
                        accessID,
                        TemporaryFunctionProperties{
                            stencilFunCallExpr, tmpAssignment.getAccessIDs(), stencilFunction,
                            tmpAssignment.getTemporaryFieldAccessExpr(), doMethodInterval});
                  } else {
                    // emplace_and_assign is available only with c++17
                    temporaryFieldExprToFunction.erase(accessID);
                    temporaryFieldExprToFunction.emplace(
                        accessID,
                        TemporaryFunctionProperties{
                            stencilFunCallExpr, tmpAssignment.getAccessIDs(), stencilFunction,
                            tmpAssignment.getTemporaryFieldAccessExpr(), doMethodInterval});
                  }

                  // first instantiation of the stencil function that is inserted in the IIR as a
                  // candidate stencil function
                  // notice we clone the ast, so that every stencil function instantiation has a
                  // private copy of the ast (so that it can be transformed). However that is not
                  // enough since this function is inserted as a candidate, and a candidate can be
                  // inserted as multiple st function instances. Later when the candidate
                  // is finalized in a concrete instance, the ast will have to be cloned again
                  ast = ast->clone();
                  auto stencilFun = stencilInstantiation->makeStencilFunctionInstantiation(
                      stencilFunCallExpr, stencilFunction, ast, sirInterval, nullptr);

                  int argID = 0;
                  for(auto accessID_ : tmpAssignment.getAccessIDs()) {
                    stencilFun->setCallerAccessIDOfArgField(argID++, accessID_);
                  }
                }
              }
            }
          }
          if(isATmpReplaced) {
            stagePtr->update(iir::NodeUpdateType::level);
          }
        }

        std::cout << "\nPASS: " << getName() << "; stencil: " << stencilInstantiation->getName();

        if(temporaryFieldExprToFunction.empty())
          std::cout << "no replacement found";

        for(auto tmpFieldPair : temporaryFieldExprToFunction) {
          int accessID = tmpFieldPair.first;
          auto tmpProperties = tmpFieldPair.second;
          if(context->getOptions().ReportPassTmpToFunction)
            std::cout << " [ replace tmp:" << metadata.getFieldNameFromAccessID(accessID)
                      << "; line : " << tmpProperties.tmpFieldAccessExpr_->getSourceLocation().Line
                      << " ] ";
        }
      }
    }
    // eliminate empty stages or stages with only NOPExpr statements
    stencilPtr->childrenEraseIf(
        [](const std::unique_ptr<iir::MultiStage>& m) -> bool { return m->isEmptyOrNullStmt(); });
    for(const auto& multiStage : stencilPtr->getChildren()) {
      multiStage->childrenEraseIf(
          [](const std::unique_ptr<iir::Stage>& s) -> bool { return s->isEmptyOrNullStmt(); });
    }
    for(const auto& multiStage : stencilPtr->getChildren()) {
      multiStage->update(iir::NodeUpdateType::levelAndTreeAbove);
    }
  }

  return true;
}

} // namespace dawn
