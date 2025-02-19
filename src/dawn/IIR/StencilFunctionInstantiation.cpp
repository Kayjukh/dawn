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

#include "dawn/IIR/StencilFunctionInstantiation.h"
#include "dawn/IIR/Field.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/Optimizer/AccessUtils.h"
#include "dawn/Optimizer/Renaming.h"
#include "dawn/Support/Casting.h"
#include "dawn/Support/Logging.h"
#include "dawn/Support/Printing.h"
#include "dawn/Support/Unreachable.h"
#include <iostream>
#include <numeric>

namespace dawn {
namespace iir {

using ::dawn::operator<<;

StencilFunctionInstantiation::StencilFunctionInstantiation(
    StencilInstantiation* context, const std::shared_ptr<StencilFunCallExpr>& expr,
    const std::shared_ptr<sir::StencilFunction>& function, const std::shared_ptr<AST>& ast,
    const Interval& interval, bool isNested)
    : stencilInstantiation_(context), metadata_(context->getMetaData()), expr_(expr),
      function_(function), ast_(ast), interval_(interval), hasReturn_(false), isNested_(isNested),
      doMethod_(make_unique<DoMethod>(interval, context->getMetaData())) {
  DAWN_ASSERT(context);
  DAWN_ASSERT(function);
}

StencilFunctionInstantiation StencilFunctionInstantiation::clone() const {
  // The SIR object function_ is not cloned, but copied, since the SIR is considered immuatble
  StencilFunctionInstantiation stencilFun(
      stencilInstantiation_, std::static_pointer_cast<StencilFunCallExpr>(expr_->clone()),
      function_, ast_->clone(), interval_, isNested_);

  stencilFun.hasReturn_ = hasReturn_;
  stencilFun.argsBound_ = argsBound_;
  stencilFun.ArgumentIndexToCallerAccessIDMap_ = ArgumentIndexToCallerAccessIDMap_;
  stencilFun.ArgumentIndexToStencilFunctionInstantiationMap_ =
      ArgumentIndexToStencilFunctionInstantiationMap_;
  stencilFun.ArgumentIndexToCallerDirectionMap_ = ArgumentIndexToCallerDirectionMap_;
  stencilFun.ArgumentIndexToCallerOffsetMap_ = ArgumentIndexToCallerOffsetMap_;
  stencilFun.CallerAcceessIDToInitialOffsetMap_ = CallerAcceessIDToInitialOffsetMap_;
  stencilFun.ExprToCallerAccessIDMap_ = ExprToCallerAccessIDMap_;
  stencilFun.StmtToCallerAccessIDMap_ = StmtToCallerAccessIDMap_;
  stencilFun.AccessIDToNameMap_ = AccessIDToNameMap_;
  stencilFun.LiteralAccessIDToNameMap_ = LiteralAccessIDToNameMap_;
  stencilFun.ExprToStencilFunctionInstantiationMap_ = ExprToStencilFunctionInstantiationMap_;
  stencilFun.calleeFields_ = calleeFields_;
  stencilFun.callerFields_ = callerFields_;
  stencilFun.unusedFields_ = unusedFields_;
  stencilFun.GlobalVariableAccessIDSet_ = GlobalVariableAccessIDSet_;

  stencilFun.doMethod_ = doMethod_->clone();

  return stencilFun;
}

Array3i StencilFunctionInstantiation::evalOffsetOfFieldAccessExpr(
    const std::shared_ptr<FieldAccessExpr>& expr, bool applyInitialOffset) const {

  // Get the offsets we know so far (i.e the constant offset)
  Array3i offset = expr->getOffset();

  // Apply the initial offset (e.g if we call a function `avg(in(i+1))` we have to shift all
  // accesses of the field `in` by [1, 0, 0])
  if(applyInitialOffset) {
    const Array3i& initialOffset = getCallerInitialOffsetFromAccessID(getAccessIDFromExpr(expr));
    offset[0] += initialOffset[0];
    offset[1] += initialOffset[1];
    offset[2] += initialOffset[2];
  }

  int sign = expr->negateOffset() ? -1 : 1;

  // Iterate the argument map (if index is *not* -1, we have to lookup the dimension or offset of
  // the directional or offset argument)
  for(int i = 0; i < expr->getArgumentMap().size(); ++i) {
    const int argIndex = expr->getArgumentMap()[i];

    if(argIndex != -1) {
      const int argOffset = expr->getArgumentOffset()[i];

      // Resolve the directions and offsets
      if(isArgDirection(argIndex))
        offset[getCallerDimensionOfArgDirection(argIndex)] += sign * argOffset;
      else {
        const auto& instantiatedOffset = getCallerOffsetOfArgOffset(argIndex);
        offset[instantiatedOffset[0]] += sign * (argOffset + instantiatedOffset[1]);
      }
    }
  }

  return offset;
}

std::vector<std::shared_ptr<sir::StencilFunctionArg>>&
StencilFunctionInstantiation::getArguments() {
  return function_->Args;
}

const std::vector<std::shared_ptr<sir::StencilFunctionArg>>&
StencilFunctionInstantiation::getArguments() const {
  return function_->Args;
}

//===------------------------------------------------------------------------------------------===//
//     Argument Maps
//===------------------------------------------------------------------------------------------===//

int StencilFunctionInstantiation::getCallerDimensionOfArgDirection(int argumentIndex) const {
  DAWN_ASSERT(ArgumentIndexToCallerDirectionMap_.count(argumentIndex));
  return ArgumentIndexToCallerDirectionMap_.find(argumentIndex)->second;
}

void StencilFunctionInstantiation::setCallerDimensionOfArgDirection(int argumentIndex,
                                                                    int dimension) {
  ArgumentIndexToCallerDirectionMap_[argumentIndex] = dimension;
}

bool StencilFunctionInstantiation::isArgBoundAsOffset(int argumentIndex) const {
  return ArgumentIndexToCallerOffsetMap_.count(argumentIndex);
}

bool StencilFunctionInstantiation::isArgBoundAsDirection(int argumentIndex) const {
  return ArgumentIndexToCallerDirectionMap_.count(argumentIndex);
}

bool StencilFunctionInstantiation::isArgBoundAsFunctionInstantiation(int argumentIndex) const {
  return ArgumentIndexToStencilFunctionInstantiationMap_.count(argumentIndex);
}

bool StencilFunctionInstantiation::isArgBoundAsFieldAccess(int argumentIndex) const {
  return ArgumentIndexToCallerAccessIDMap_.count(argumentIndex);
}

const Array2i& StencilFunctionInstantiation::getCallerOffsetOfArgOffset(int argumentIndex) const {
  DAWN_ASSERT(ArgumentIndexToCallerOffsetMap_.count(argumentIndex));
  return ArgumentIndexToCallerOffsetMap_.find(argumentIndex)->second;
}

void StencilFunctionInstantiation::setCallerOffsetOfArgOffset(int argumentIndex,
                                                              const Array2i& offset) {
  ArgumentIndexToCallerOffsetMap_[argumentIndex] = offset;
}

int StencilFunctionInstantiation::getCallerAccessIDOfArgField(int argumentIndex) const {
  return ArgumentIndexToCallerAccessIDMap_.at(argumentIndex);
}

void StencilFunctionInstantiation::setCallerAccessIDOfArgField(int argumentIndex,
                                                               int callerAccessID) {
  ArgumentIndexToCallerAccessIDMap_[argumentIndex] = callerAccessID;
}

std::shared_ptr<StencilFunctionInstantiation>
StencilFunctionInstantiation::getFunctionInstantiationOfArgField(int argumentIndex) const {
  DAWN_ASSERT(ArgumentIndexToStencilFunctionInstantiationMap_.count(argumentIndex));
  return ArgumentIndexToStencilFunctionInstantiationMap_.find(argumentIndex)->second;
}

void StencilFunctionInstantiation::setFunctionInstantiationOfArgField(
    int argumentIndex, const std::shared_ptr<StencilFunctionInstantiation>& func) {
  ArgumentIndexToStencilFunctionInstantiationMap_[argumentIndex] = func;
}

const Array3i&
StencilFunctionInstantiation::getCallerInitialOffsetFromAccessID(int callerAccessID) const {
  DAWN_ASSERT(CallerAcceessIDToInitialOffsetMap_.count(callerAccessID));
  return CallerAcceessIDToInitialOffsetMap_.find(callerAccessID)->second;
}

void StencilFunctionInstantiation::setCallerInitialOffsetFromAccessID(int callerAccessID,
                                                                      const Array3i& offset) {
  CallerAcceessIDToInitialOffsetMap_[callerAccessID] = offset;
}

bool StencilFunctionInstantiation::isProvidedByStencilFunctionCall(int callerAccessID) const {
  auto pos = std::find_if(ArgumentIndexToCallerAccessIDMap_.begin(),
                          ArgumentIndexToCallerAccessIDMap_.end(),
                          [&](const std::pair<int, int>& p) { return p.second == callerAccessID; });

  // accessID is not an argument to stencil function
  if(pos == ArgumentIndexToCallerAccessIDMap_.end())
    return false;
  return isArgStencilFunctionInstantiation(pos->first);
}

int StencilFunctionInstantiation::getArgumentIndexFromCallerAccessID(int callerAccessID) const {
  for(std::size_t argIdx = 0; argIdx < function_->Args.size(); ++argIdx)
    if(isArgField(argIdx) || isArgStencilFunctionInstantiation(callerAccessID))
      if(getCallerAccessIDOfArgField(argIdx) == callerAccessID)
        return argIdx;
  dawn_unreachable("invalid AccessID");
}

const std::string&
StencilFunctionInstantiation::getOriginalNameFromCallerAccessID(int callerAccessID) const {
  for(std::size_t argIdx = 0; argIdx < function_->Args.size(); ++argIdx) {
    if(sir::Field* field = dyn_cast<sir::Field>(function_->Args[argIdx].get())) {
      if(getCallerAccessIDOfArgField(argIdx) == callerAccessID)
        return field->Name;
    }
  }
  dawn_unreachable("invalid AccessID");
}

const Field&
StencilFunctionInstantiation::getCallerFieldFromArgumentIndex(int argumentIndex) const {
  int callerAccessID = getCallerAccessIDOfArgField(argumentIndex);

  for(const Field& field : callerFields_)
    if(field.getAccessID() == callerAccessID)
      return field;

  dawn_unreachable("invalid argument index of field");
}

const std::vector<Field>& StencilFunctionInstantiation::getCallerFields() const {
  return callerFields_;
}

const std::vector<Field>& StencilFunctionInstantiation::getCalleeFields() const {
  return calleeFields_;
}

bool StencilFunctionInstantiation::isArgOffset(int argumentIndex) const {
  return isa<sir::Offset>(function_->Args[argumentIndex].get());
}

bool StencilFunctionInstantiation::isArgDirection(int argumentIndex) const {
  return isa<sir::Direction>(function_->Args[argumentIndex].get());
}

bool StencilFunctionInstantiation::isArgField(int argumentIndex) const {
  return isa<sir::Field>(function_->Args[argumentIndex].get());
}

bool StencilFunctionInstantiation::isArgStencilFunctionInstantiation(int argumentIndex) const {
  return ArgumentIndexToStencilFunctionInstantiationMap_.count(argumentIndex);
}

template <class MapType, class KeyType>
static void replaceKeyInMap(MapType& map, KeyType oldKey, KeyType newKey) {
  auto it = map.find(oldKey);
  if(it != map.end()) {
    std::swap(map[newKey], it->second);
    map.erase(it);
  }
}

void StencilFunctionInstantiation::renameCallerAccessID(int oldAccessID, int newAccessID) {
  // Update argument maps
  for(auto& argumentAccessIDPair : ArgumentIndexToCallerAccessIDMap_) {
    int& AccessID = argumentAccessIDPair.second;
    if(AccessID == oldAccessID)
      AccessID = newAccessID;
  }
  replaceKeyInMap(CallerAcceessIDToInitialOffsetMap_, oldAccessID, newAccessID);

  // Update AccessID to name map
  replaceKeyInMap(AccessIDToNameMap_, oldAccessID, newAccessID);

  // Update statements
  renameAccessIDInStmts(this, oldAccessID, newAccessID, doMethod_->getChildren());

  // Update accesses
  renameAccessIDInAccesses(this, oldAccessID, newAccessID, doMethod_->getChildren());

  // Recompute the fields
  update();
}

//===----------------------------------------------------------------------------------------===//
//     Expr/Stmt to Caller AccessID Maps
//===----------------------------------------------------------------------------------------===//

std::string StencilFunctionInstantiation::getFieldNameFromAccessID(int AccessID) const {
  // As we store the caller accessIDs, we have to get the name of the field from the context!
  // TODO have a check for what is a literal range
  if(AccessID < 0)
    return getNameFromLiteralAccessID(AccessID);
  else if(metadata_.isAccessType(FieldAccessType::FAT_Field, AccessID) ||
          metadata_.isAccessType(iir::FieldAccessType::FAT_GlobalVariable, AccessID))
    return metadata_.getFieldNameFromAccessID(AccessID);
  else {
    DAWN_ASSERT(AccessIDToNameMap_.count(AccessID));
    return AccessIDToNameMap_.find(AccessID)->second;
  }
}

void StencilFunctionInstantiation::setAccessIDOfGlobalVariable(int AccessID) {
  //  setAccessIDNamePair(AccessID, name);
  GlobalVariableAccessIDSet_.insert(AccessID);
}

const std::string& StencilFunctionInstantiation::getNameFromLiteralAccessID(int AccessID) const {
  auto it = LiteralAccessIDToNameMap_.find(AccessID);
  DAWN_ASSERT_MSG(it != LiteralAccessIDToNameMap_.end(), "Invalid Literal");
  return it->second;
}

std::string StencilFunctionInstantiation::getNameFromAccessID(int accessID) const {
  if(isLiteral(accessID)) {
    return getNameFromLiteralAccessID(accessID);
  } else if(metadata_.isAccessType(FieldAccessType::FAT_Field, accessID) ||
            isProvidedByStencilFunctionCall(accessID)) {
    return getOriginalNameFromCallerAccessID(accessID);
  } else {
    return getFieldNameFromAccessID(accessID);
  }
}

int StencilFunctionInstantiation::getAccessIDFromExpr(const std::shared_ptr<Expr>& expr) const {
  auto it = ExprToCallerAccessIDMap_.find(expr);
  /// HACK for Literals (inserted from Globals) that are not found in SFI
  if(it == ExprToCallerAccessIDMap_.end()) {
    return metadata_.getAccessIDFromExpr(expr);
  }
  DAWN_ASSERT_MSG(it != ExprToCallerAccessIDMap_.end(), "Invalid Expr");
  return it->second;
}

int StencilFunctionInstantiation::getAccessIDFromStmt(const std::shared_ptr<Stmt>& stmt) const {
  auto it = StmtToCallerAccessIDMap_.find(stmt);
  DAWN_ASSERT_MSG(it != StmtToCallerAccessIDMap_.end(), "Invalid Stmt");
  return it->second;
}

void StencilFunctionInstantiation::setAccessIDOfExpr(const std::shared_ptr<Expr>& expr,
                                                     const int accessID) {
  ExprToCallerAccessIDMap_[expr] = accessID;
}

void StencilFunctionInstantiation::mapExprToAccessID(const std::shared_ptr<Expr>& expr,
                                                     int accessID) {
  DAWN_ASSERT(!ExprToCallerAccessIDMap_.count(expr) ||
              ExprToCallerAccessIDMap_.at(expr) == accessID);

  ExprToCallerAccessIDMap_.emplace(expr, accessID);
}

void StencilFunctionInstantiation::setAccessIDOfStmt(const std::shared_ptr<Stmt>& stmt,
                                                     const int accessID) {
  DAWN_ASSERT(StmtToCallerAccessIDMap_.count(stmt));
  StmtToCallerAccessIDMap_[stmt] = accessID;
}

void StencilFunctionInstantiation::mapStmtToAccessID(const std::shared_ptr<Stmt>& stmt,
                                                     int accessID) {
  StmtToCallerAccessIDMap_.emplace(stmt, accessID);
}

std::unordered_map<int, std::string>& StencilFunctionInstantiation::getLiteralAccessIDToNameMap() {
  return LiteralAccessIDToNameMap_;
}
const std::unordered_map<int, std::string>&
StencilFunctionInstantiation::getLiteralAccessIDToNameMap() const {
  return LiteralAccessIDToNameMap_;
}

std::unordered_map<int, std::string>& StencilFunctionInstantiation::getAccessIDToNameMap() {
  return AccessIDToNameMap_;
}

const std::unordered_map<int, std::string>&
StencilFunctionInstantiation::getAccessIDToNameMap() const {
  return AccessIDToNameMap_;
}

const std::unordered_map<std::shared_ptr<StencilFunCallExpr>,
                         std::shared_ptr<StencilFunctionInstantiation>>&
StencilFunctionInstantiation::getExprToStencilFunctionInstantiationMap() const {
  return ExprToStencilFunctionInstantiationMap_;
}

void StencilFunctionInstantiation::insertExprToStencilFunction(
    const std::shared_ptr<StencilFunctionInstantiation>& stencilFun) {
  DAWN_ASSERT(!ExprToStencilFunctionInstantiationMap_.count(stencilFun->getExpression()));

  ExprToStencilFunctionInstantiationMap_.emplace(stencilFun->getExpression(), stencilFun);
}

void StencilFunctionInstantiation::removeStencilFunctionInstantiation(
    const std::shared_ptr<StencilFunCallExpr>& expr) {
  ExprToStencilFunctionInstantiationMap_.erase(expr);
}

std::shared_ptr<StencilFunctionInstantiation>
StencilFunctionInstantiation::getStencilFunctionInstantiation(
    const std::shared_ptr<StencilFunCallExpr>& expr) const {
  auto it = ExprToStencilFunctionInstantiationMap_.find(expr);
  DAWN_ASSERT_MSG(it != ExprToStencilFunctionInstantiationMap_.end(), "Invalid stencil function");
  return it->second;
}

bool StencilFunctionInstantiation::hasStencilFunctionInstantiation(
    const std::shared_ptr<StencilFunCallExpr>& expr) const {
  return (ExprToStencilFunctionInstantiationMap_.find(expr) !=
          ExprToStencilFunctionInstantiationMap_.end());
}

const std::vector<std::unique_ptr<StatementAccessesPair>>&
StencilFunctionInstantiation::getStatementAccessesPairs() const {
  return doMethod_->getChildren();
}

//===------------------------------------------------------------------------------------------===//
//     Accesses & Fields
//===------------------------------------------------------------------------------------------===//

void StencilFunctionInstantiation::update() {
  callerFields_.clear();
  calleeFields_.clear();
  unusedFields_.clear();

  // Compute the fields and their intended usage. Fields can be in one of three states: `Output`,
  // `InputOutput` or `Input` which implements the following state machine:
  //
  //    +-------+                               +--------+
  //    | Input |                               | Output |
  //    +-------+                               +--------+
  //        |                                       |
  //        |            +-------------+            |
  //        +----------> | InputOutput | <----------+
  //                     +-------------+
  //
  std::unordered_map<int, Field> inputOutputFields;
  std::unordered_map<int, Field> inputFields;
  std::unordered_map<int, Field> outputFields;

  for(const auto& statementAccessesPair : doMethod_->getChildren()) {
    auto access = statementAccessesPair->getAccesses();
    DAWN_ASSERT(access);

    for(const auto& accessPair : access->getWriteAccesses()) {
      int AccessID = accessPair.first;

      // Does this AccessID correspond to a field access?
      if(!isProvidedByStencilFunctionCall(AccessID) &&
         !metadata_.isAccessType(FieldAccessType::FAT_Field, AccessID))
        continue;

      AccessUtils::recordWriteAccess(inputOutputFields, inputFields, outputFields, AccessID,
                                     boost::optional<Extents>(), interval_);
    }

    for(const auto& accessPair : access->getReadAccesses()) {
      int AccessID = accessPair.first;

      // Does this AccessID correspond to a field access?
      if(!isProvidedByStencilFunctionCall(AccessID) &&
         !metadata_.isAccessType(FieldAccessType::FAT_Field, AccessID))
        continue;

      AccessUtils::recordReadAccess(inputOutputFields, inputFields, outputFields, AccessID,
                                    boost::optional<Extents>(), interval_);
    }
  }

  // Add AccessIDs of unused fields i.e fields which are passed as arguments but never referenced.
  for(const auto& argIdxCallerAccessIDPair : ArgumentIndexToCallerAccessIDMap_) {
    int AccessID = argIdxCallerAccessIDPair.second;
    if(!inputFields.count(AccessID) && !outputFields.count(AccessID) &&
       !inputOutputFields.count(AccessID)) {
      inputFields.emplace(AccessID, Field(AccessID, Field::IK_Input, Extents{0, 0, 0, 0, 0, 0},
                                          Extents{0, 0, 0, 0, 0, 0}, interval_));
      unusedFields_.insert(AccessID);
    }
  }

  std::vector<Field> calleeFieldsUnordered;
  std::vector<Field> callerFieldsUnordered;

  // Merge inputFields, outputFields and fields. Note that caller and callee fields are the same,
  // the only difference is that in the caller fields we apply the inital offset to the extents
  // while in the callee fields we do not.
  for(auto AccessIDFieldPair : outputFields) {
    calleeFieldsUnordered.push_back(AccessIDFieldPair.second);
    callerFieldsUnordered.push_back(AccessIDFieldPair.second);
  }

  for(auto AccessIDFieldPair : inputOutputFields) {
    calleeFieldsUnordered.push_back(AccessIDFieldPair.second);
    callerFieldsUnordered.push_back(AccessIDFieldPair.second);
  }

  for(auto AccessIDFieldPair : inputFields) {
    calleeFieldsUnordered.push_back(AccessIDFieldPair.second);
    callerFieldsUnordered.push_back(AccessIDFieldPair.second);
  }

  if(calleeFieldsUnordered.empty() || callerFieldsUnordered.empty()) {
    DAWN_LOG(WARNING) << "no fields referenced in this stencil function";
  } else {

    // Accumulate the extent of the fields (note that here the callee and caller fields differ
    // as the caller fields have the *initial* extent (e.g in `avg(u(i+1))` u has an initial extent
    // of [1, 0, 0])
    auto computeAccesses = [&](std::vector<Field>& fields, bool callerAccesses) {
      // Index to speedup lookup into fields map
      std::unordered_map<int, std::vector<Field>::iterator> AccessIDToFieldMap;
      for(auto it = fields.begin(), end = fields.end(); it != end; ++it)
        AccessIDToFieldMap.insert(std::make_pair(it->getAccessID(), it));

      // Accumulate the extents of each field in this stage
      for(const auto& statementAccessesPair : doMethod_->getChildren()) {
        const auto& access = callerAccesses ? statementAccessesPair->getCallerAccesses()
                                            : statementAccessesPair->getCalleeAccesses();

        // first => AccessID, second => Extent
        for(auto& accessPair : access->getWriteAccesses()) {
          if(!isProvidedByStencilFunctionCall(accessPair.first) &&
             !metadata_.isAccessType(FieldAccessType::FAT_Field, accessPair.first))
            continue;

          AccessIDToFieldMap[accessPair.first]->mergeWriteExtents(accessPair.second);
        }

        for(const auto& accessPair : access->getReadAccesses()) {
          if(!isProvidedByStencilFunctionCall(accessPair.first) &&
             !metadata_.isAccessType(FieldAccessType::FAT_Field, accessPair.first))
            continue;

          AccessIDToFieldMap[accessPair.first]->mergeReadExtents(accessPair.second);
        }
      }
    };

    computeAccesses(callerFieldsUnordered, true);
    computeAccesses(calleeFieldsUnordered, false);
  }

  // Reorder the fields s.t they match the order in which they were decalred in the stencil-function
  for(int argIdx = 0; argIdx < getArguments().size(); ++argIdx) {
    if(isArgField(argIdx)) {
      int AccessID = getCallerAccessIDOfArgField(argIdx);

      auto insertField = [&](std::vector<Field>& fieldsOrdered,
                             std::vector<Field>& fieldsUnordered) {
        auto it = std::find_if(fieldsUnordered.begin(), fieldsUnordered.end(),
                               [&](const Field& field) { return field.getAccessID() == AccessID; });
        DAWN_ASSERT(it != fieldsUnordered.end());
        fieldsOrdered.push_back(*it);
      };

      insertField(callerFields_, callerFieldsUnordered);
      insertField(calleeFields_, calleeFieldsUnordered);
    }
  }
}

bool StencilFunctionInstantiation::isFieldUnused(int AccessID) const {
  return unusedFields_.count(AccessID);
}

//===------------------------------------------------------------------------------------------===//
//     Miscellaneous
//===------------------------------------------------------------------------------------------===//

std::string
StencilFunctionInstantiation::makeCodeGenName(const StencilFunctionInstantiation& stencilFun) {
  std::string name = stencilFun.getName();

  for(std::size_t argIdx = 0; argIdx < stencilFun.getStencilFunction()->Args.size(); ++argIdx) {
    if(stencilFun.isArgOffset(argIdx)) {
      const auto& offset = stencilFun.getCallerOffsetOfArgOffset(argIdx);
      name += "_" + dim2str(offset[0]) + "_";
      if(offset[1] != 0)
        name += (offset[1] > 0 ? "plus_" : "minus_");
      name += std::to_string(std::abs(offset[1]));

    } else if(stencilFun.isArgDirection(argIdx)) {
      name += "_" + dim2str(stencilFun.getCallerDimensionOfArgDirection(argIdx));
    } else if(stencilFun.isArgStencilFunctionInstantiation(argIdx)) {
      StencilFunctionInstantiation& argFunction =
          *(stencilFun.getFunctionInstantiationOfArgField(argIdx));
      name += "_" + makeCodeGenName(argFunction);
    }
  }

  name += "_" + Interval::makeCodeGenName(stencilFun.getInterval());
  return name;
}

void StencilFunctionInstantiation::setReturn(bool hasReturn) { hasReturn_ = hasReturn; }

bool StencilFunctionInstantiation::hasReturn() const { return hasReturn_; }

bool StencilFunctionInstantiation::isNested() const { return isNested_; }

size_t StencilFunctionInstantiation::numArgs() const { return function_->Args.size(); }

std::string StencilFunctionInstantiation::getArgNameFromFunctionCall(std::string fnCallName) const {

  for(std::size_t argIdx = 0; argIdx < numArgs(); ++argIdx) {
    if(!isArgField(argIdx) || !isArgStencilFunctionInstantiation(argIdx))
      continue;

    if(fnCallName == getFunctionInstantiationOfArgField(argIdx)->getName()) {
      sir::Field* field = dyn_cast<sir::Field>(function_->Args[argIdx].get());
      return field->Name;
    }
  }
  DAWN_ASSERT_MSG(0, "arg field of callee being a stencial function at caller not found");
  return "";
}

void StencilFunctionInstantiation::dump() const {
  std::cout << "\nStencilFunction : " << getName() << " " << getInterval() << "\n";
  std::cout << MakeIndent<1>::value << "Arguments:\n";

  for(std::size_t argIdx = 0; argIdx < numArgs(); ++argIdx) {

    std::cout << MakeIndent<2>::value << "arg(" << argIdx << ") : ";

    if(isArgOffset(argIdx)) {
      int dim = getCallerOffsetOfArgOffset(argIdx)[0];
      int offset = getCallerOffsetOfArgOffset(argIdx)[1];
      std::cout << "Offset : " << dim2str(dim);
      if(offset != 0)
        std::cout << (offset > 0 ? "+" : "") << offset;
    } else if(isArgField(argIdx)) {
      sir::Field* field = dyn_cast<sir::Field>(function_->Args[argIdx].get());
      std::cout << "Field : " << field->Name << " -> ";
      if(isArgStencilFunctionInstantiation(argIdx)) {
        std::cout << "stencil-function-call:"
                  << getFunctionInstantiationOfArgField(argIdx)->getName();
      } else {
        int callerAccessID = getCallerAccessIDOfArgField(argIdx);
        std::cout << metadata_.getFieldNameFromAccessID(callerAccessID) << "  "
                  << getCallerInitialOffsetFromAccessID(callerAccessID);
      }

    } else {
      std::cout << "Direction : " << dim2str(getCallerDimensionOfArgDirection(argIdx));
    }
    std::cout << "\n";
  }

  std::cout << MakeIndent<1>::value << "Accesses (including initial offset):\n";

  const auto& statements = getAST()->getRoot()->getStatements();
  for(std::size_t i = 0; i < statements.size(); ++i) {
    std::cout << "\e[1m" << ASTStringifer::toString(statements[i], 2 * DAWN_PRINT_INDENT)
              << "\e[0m";
    if(doMethod_->getChild(i)->getCallerAccesses())
      std::cout << doMethod_->getChild(i)->getCallerAccesses()->toString(this,
                                                                         3 * DAWN_PRINT_INDENT)
                << "\n";
  }
  std::cout.flush();
}

void StencilFunctionInstantiation::closeFunctionBindings() {
  std::vector<int> arglist(getArguments().size());
  std::iota(arglist.begin(), arglist.end(), 0);

  closeFunctionBindings(arglist);
}
void StencilFunctionInstantiation::closeFunctionBindings(const std::vector<int>& arglist) {
  // finalize the bindings of some of the arguments that are not yet instantiated
  const auto& arguments = getArguments();

  for(int argIdx : arglist) {
    if(isa<sir::Field>(*arguments[argIdx])) {
      if(isArgStencilFunctionInstantiation(argIdx)) {

        // The field is provided by a stencil function call, we create a new AccessID for this
        // "temporary" field
        int AccessID = stencilInstantiation_->nextUID();

        setCallerAccessIDOfArgField(argIdx, AccessID);
        setCallerInitialOffsetFromAccessID(AccessID, Array3i{{0, 0, 0}});
      }
    }
  }

  argsBound_ = true;
}

void StencilFunctionInstantiation::checkFunctionBindings() const {

  const auto& arguments = getArguments();

  for(std::size_t argIdx = 0; argIdx < arguments.size(); ++argIdx) {
    // check that all arguments of all possible types are assigned
    if(isa<sir::Field>(*arguments[argIdx])) {
      DAWN_ASSERT_MSG(
          (isArgBoundAsFieldAccess(argIdx) || isArgBoundAsFunctionInstantiation(argIdx)),
          std::string("Field access arg not bound for function " + function_->Name).c_str());
    } else if(isa<sir::Direction>(*arguments[argIdx])) {
      DAWN_ASSERT_MSG(
          (isArgBoundAsDirection(argIdx)),
          std::string("Direction arg not bound for function " + function_->Name).c_str());
    } else if(isa<sir::Offset>(*arguments[argIdx])) {
      DAWN_ASSERT_MSG((isArgBoundAsOffset(argIdx)),
                      std::string("Offset arg not bound for function " + function_->Name).c_str());
    } else
      dawn_unreachable("Argument not supported");
  }

  // check that the list of <statement,access> are set for all statements
  DAWN_ASSERT_MSG((getAST()->getRoot()->getStatements().size() == doMethod_->getChildren().size()),
                  "AST has different number of statements as the statement accesses pairs");
}

} // namespace iir
} // namespace dawn
