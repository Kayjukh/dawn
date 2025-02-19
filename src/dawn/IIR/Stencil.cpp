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

#include "dawn/IIR/Stencil.h"
#include "dawn/IIR/DependencyGraphStage.h"
#include "dawn/IIR/IIR.h"
#include "dawn/IIR/IIRNodeIterator.h"
#include "dawn/IIR/StatementAccessesPair.h"
#include "dawn/Optimizer/Renaming.h"
#include "dawn/SIR/SIR.h"
#include "dawn/Support/StringUtil.h"
#include "dawn/Support/Unreachable.h"
#include <algorithm>
#include <iostream>
#include <numeric>

namespace dawn {

std::ostream& operator<<(std::ostream& os, const iir::Stencil::StagePosition& position);
std::ostream& operator<<(std::ostream& os, const iir::Stencil::StatementPosition& position);
std::ostream& operator<<(std::ostream& os, const iir::Stencil::Lifetime& lifetime);
std::ostream& operator<<(std::ostream& os, const iir::Stencil& stencil);

std::ostream& operator<<(std::ostream& os, const iir::Stencil::StagePosition& position) {
  return (os << "(" << position.MultiStageIndex << ", " << position.StageOffset << ")");
}

std::ostream& operator<<(std::ostream& os, const iir::Stencil::StatementPosition& position) {
  return (os << "(Stage=" << position.StagePos << ", DoMethod=" << position.DoMethodIndex
             << ", Statement=" << position.StatementIndex << ")");
}

std::ostream& operator<<(std::ostream& os, const iir::Stencil::Lifetime& lifetime) {
  return (os << "[Begin=" << lifetime.Begin << ", End=" << lifetime.End << "]");
}

namespace iir {

bool Stencil::StagePosition::operator<(const Stencil::StagePosition& other) const {
  return MultiStageIndex < other.MultiStageIndex ||
         (MultiStageIndex == other.MultiStageIndex && StageOffset < other.StageOffset);
}

bool Stencil::StagePosition::operator==(const Stencil::StagePosition& other) const {
  return MultiStageIndex == other.MultiStageIndex && StageOffset == other.StageOffset;
}

bool Stencil::StagePosition::operator!=(const Stencil::StagePosition& other) const {
  return !(*this == other);
}

bool Stencil::StatementPosition::operator<(const Stencil::StatementPosition& other) const {
  return StagePos < other.StagePos ||
         (StagePos == other.StagePos && DoMethodIndex == other.DoMethodIndex &&
          StatementIndex < other.StatementIndex);
}

bool Stencil::StatementPosition::operator<=(const Stencil::StatementPosition& other) const {
  return operator<(other) || operator==(other);
}

bool Stencil::StatementPosition::operator==(const Stencil::StatementPosition& other) const {
  return StagePos == other.StagePos && DoMethodIndex == other.DoMethodIndex &&
         StatementIndex == other.StatementIndex;
}

bool Stencil::StatementPosition::operator!=(const Stencil::StatementPosition& other) const {
  return !(*this == other);
}

bool Stencil::StatementPosition::inSameDoMethod(const Stencil::StatementPosition& other) const {
  return StagePos == other.StagePos && DoMethodIndex == other.DoMethodIndex;
}

json::json Stencil::FieldInfo::jsonDump() const {
  json::json node;
  node["dim"] = format("[%i,%i,%i]", Dimensions[0], Dimensions[1], Dimensions[2]);
  node["field"] = field.jsonDump();
  node["IsTemporary"] = IsTemporary;
  return node;
}

json::json Stencil::jsonDump() const {
  json::json node;
  node["ID"] = std::to_string(StencilID_);
  json::json fieldsJson;
  for(const auto& f : derivedInfo_.fields_) {
    fieldsJson[f.second.Name] = f.second.jsonDump();
  }
  node["Fields"] = fieldsJson;

  auto multiStagesArray = json::json::array();
  for(const auto& child : children_) {
    multiStagesArray.push_back(child->jsonDump());
  }
  node["MultiStages"] = multiStagesArray;
  return node;
}

bool Stencil::containsRedundantComputations() const {
  for(const auto& stage : iterateIIROver<Stage>(*this)) {
    if(!stage->getExtents().isHorizontalPointwise()) {
      return true;
    }
  }
  return false;
}

void Stencil::updateFromChildren() {
  derivedInfo_.fields_.clear();
  std::unordered_map<int, Field> fields;

  for(const auto& MSPtr : children_) {
    mergeFields(MSPtr->getFields(), fields);
  }

  for(const auto& fieldPair : fields) {
    const int accessID = fieldPair.first;
    const Field& field = fieldPair.second;

    std::string name = metadata_.getFieldNameFromAccessID(accessID);
    bool isTemporary = metadata_.isAccessType(iir::FieldAccessType::FAT_StencilTemporary, accessID);
    Array3i specifiedDimension = metadata_.getFieldDimensionsMask(accessID);

    derivedInfo_.fields_.emplace(
        std::make_pair(accessID, FieldInfo{isTemporary, name, specifiedDimension, field}));
  }
}

bool Stencil::Lifetime::overlaps(const Stencil::Lifetime& other) const {
  // Note: same stage but different Do-Method are treated as overlapping!

  bool lowerBoundOverlap = false;
  if(Begin.StagePos == other.End.StagePos && Begin.DoMethodIndex != other.End.DoMethodIndex)
    lowerBoundOverlap = true;
  else
    lowerBoundOverlap = Begin <= other.End;

  bool upperBoundOverlap = false;
  if(other.Begin.StagePos == End.StagePos && other.Begin.DoMethodIndex != End.DoMethodIndex)
    upperBoundOverlap = true;
  else
    upperBoundOverlap = other.Begin <= End;

  return lowerBoundOverlap && upperBoundOverlap;
}

sir::Attr& Stencil::getStencilAttributes() { return stencilAttributes_; }

Stencil::Stencil(const StencilMetaInformation& metadata, sir::Attr attributes, int StencilID)
    : metadata_(metadata), stencilAttributes_(attributes), StencilID_(StencilID) {}

void Stencil::DerivedInfo::clear() { fields_.clear(); }

void Stencil::clearDerivedInfo() { derivedInfo_.clear(); }

std::unordered_set<Interval> Stencil::getIntervals() const {
  std::unordered_set<Interval> intervals;

  for(const auto& doMethod : iterateIIROver<DoMethod>(*this)) {
    intervals.insert(doMethod->getInterval());
  }
  return intervals;
}

std::unique_ptr<Stencil> Stencil::clone() const {
  auto cloneStencil = make_unique<Stencil>(metadata_, stencilAttributes_, StencilID_);

  cloneStencil->derivedInfo_ = derivedInfo_;
  cloneStencil->cloneChildrenFrom(*this);
  return cloneStencil;
}

std::vector<std::string> Stencil::getGlobalVariables() const {
  std::set<int> globalVariableAccessIDs;
  for(const auto& stage : iterateIIROver<Stage>(*this)) {
    globalVariableAccessIDs.insert(stage->getAllGlobalVariables().begin(),
                                   stage->getAllGlobalVariables().end());
  }

  std::vector<std::string> globalVariables;
  for(const auto& AccessID : globalVariableAccessIDs)
    globalVariables.push_back(metadata_.getFieldNameFromAccessID(AccessID));

  return globalVariables;
}

int Stencil::getNumStages() const {
  return std::accumulate(childrenBegin(), childrenEnd(), int(0),
                         [](int numStages, const Stencil::MultiStageSmartPtr_t& MS) {
                           return numStages + MS->getChildren().size();
                         });
}

void Stencil::forEachStatementAccessesPair(
    std::function<void(ArrayRef<std::unique_ptr<StatementAccessesPair>>)> func, bool updateFields) {
  forEachStatementAccessesPairImpl(func, 0, getNumStages(), updateFields);
}

void Stencil::forEachStatementAccessesPair(
    std::function<void(ArrayRef<std::unique_ptr<StatementAccessesPair>>)> func,
    const Stencil::Lifetime& lifetime, bool updateFields) {
  int startStageIdx = getStageIndexFromPosition(lifetime.Begin.StagePos);
  int endStageIdx = getStageIndexFromPosition(lifetime.End.StagePos);
  forEachStatementAccessesPairImpl(func, startStageIdx, endStageIdx + 1, updateFields);
}

void Stencil::forEachStatementAccessesPairImpl(
    std::function<void(ArrayRef<std::unique_ptr<StatementAccessesPair>>)> func, int startStageIdx,
    int endStageIdx, bool updateFields) {
  for(int stageIdx = startStageIdx; stageIdx < endStageIdx; ++stageIdx) {
    const auto& stage = getStage(stageIdx);
    for(const auto& doMethodPtr : stage->getChildren()) {
      func(doMethodPtr->getChildren());
      if(updateFields) {
        doMethodPtr->update(iir::NodeUpdateType::level);
      }
    }
    if(updateFields) {
      stage->update(iir::NodeUpdateType::level);
    }
  }
}

void Stencil::updateFields(const Stencil::Lifetime& lifetime) {
  int startStageIdx = getStageIndexFromPosition(lifetime.Begin.StagePos);
  int endStageIdx = getStageIndexFromPosition(lifetime.End.StagePos);
  updateFieldsImpl(startStageIdx, endStageIdx + 1);
}

void Stencil::updateFields() { updateFieldsImpl(0, getNumStages()); }

void Stencil::updateFieldsImpl(int startStageIdx, int endStageIdx) {
  for(int stageIdx = startStageIdx; stageIdx < endStageIdx; ++stageIdx) {
    for(auto& doMethod : getStage(stageIdx)->getChildren()) {
      doMethod->update(iir::NodeUpdateType::level);
    }
    getStage(stageIdx)->update(iir::NodeUpdateType::level);
  }
}

std::unordered_map<int, Field> Stencil::computeFieldsOnTheFly() const {
  std::unordered_map<int, Field> fields;

  for(const auto& mssPtr : children_) {
    for(const auto& fieldPair : mssPtr->computeFieldsOnTheFly()) {
      const Field& field = fieldPair.second;
      auto it = fields.find(field.getAccessID());
      if(it != fields.end()) {
        // Adjust the Intend
        if(it->second.getIntend() == Field::IK_Input && field.getIntend() == Field::IK_Output)
          it->second.setIntend(Field::IK_InputOutput);
        else if(it->second.getIntend() == Field::IK_Output && field.getIntend() == Field::IK_Input)
          it->second.setIntend(Field::IK_InputOutput);

        // Merge the Extent
        it->second.mergeReadExtents(field.getReadExtents());
        it->second.mergeWriteExtents(field.getWriteExtents());
        it->second.mergeReadExtentsRB(field.getReadExtentsRB());
        it->second.mergeWriteExtentsRB(field.getWriteExtentsRB());

        it->second.extendInterval(field.getInterval());
      } else
        fields.emplace(field.getAccessID(), field);
    }
  }

  return fields;
}

bool Stencil::hasGlobalVariables() const {
  for(const auto& stage : iterateIIROver<Stage>(*this)) {
    if(stage->hasGlobalVariables())
      return true;
  }
  return false;
}
bool Stencil::compareDerivedInfo() const {
  auto fieldsOnTheFly = computeFieldsOnTheFly();

  bool equal = true;
  for(auto it : derivedInfo_.fields_) {
    const int accessID = it.first;
    const FieldInfo& fieldInfo = it.second;
    const Field& field = fieldInfo.field;
    const auto& extents = field.getExtents();
    const auto& extentsRB = field.getExtentsRB();
    if(!fieldsOnTheFly.count(accessID)) {
      dawn_unreachable("Error, accessID not found in the computed on the fly fields");
      return false;
    }
    if(fieldsOnTheFly.at(accessID).getExtentsRB() != extentsRB) {
      dawn_unreachable(
          std::string("ERROR: the redudant block extended Extents do not much in precomputed "
                      "derived info and computed on the fly fields:"
                      " field id " +
                      std::to_string(accessID) + ", on the fly [" +
                      fieldsOnTheFly.at(accessID).getExtentsRB().toString() +
                      "], derived info precomputed [" + extentsRB.toString())
              .c_str());
      return false;
    }
    if(fieldsOnTheFly.at(accessID).getExtents() != extents) {
      dawn_unreachable(std::string("ERROR: the field Extents do not much in precomputed "
                                   "derived info and computed on the fly fields:"
                                   " field id " +
                                   std::to_string(accessID) + ", on the fly [" +
                                   fieldsOnTheFly.at(accessID).getExtents().toString() +
                                   "], derived info precomputed [" + extents.toString())
                           .c_str());
      return false;
    }
  }
  return equal;
}
void Stencil::setStageDependencyGraph(const std::shared_ptr<DependencyGraphStage>& stageDAG) {
  derivedInfo_.stageDependencyGraph_ = stageDAG;
}

const std::shared_ptr<DependencyGraphStage>& Stencil::getStageDependencyGraph() const {
  return derivedInfo_.stageDependencyGraph_;
}

const std::unique_ptr<MultiStage>&
Stencil::getMultiStageFromMultiStageIndex(int multiStageIdx) const {
  DAWN_ASSERT_MSG(multiStageIdx < children_.size(), "invalid multi-stage index");
  auto msIt = children_.begin();
  std::advance(msIt, multiStageIdx);
  return *msIt;
}

const std::unique_ptr<MultiStage>& Stencil::getMultiStageFromStageIndex(int stageIdx) const {
  return getMultiStageFromMultiStageIndex(getPositionFromStageIndex(stageIdx).MultiStageIndex);
}

Stencil::StagePosition Stencil::getPositionFromStageIndex(int stageIdx) const {
  DAWN_ASSERT(!children_.empty());
  if(stageIdx == -1)
    return StagePosition(0, -1);

  int curIdx = 0, multiStageIdx = 0;
  for(const auto& MS : children_) {

    // Is our stage in this multi-stage?
    int numStages = MS->getChildren().size();
    if((curIdx + numStages) <= stageIdx) {
      curIdx += numStages;
      multiStageIdx++;
      continue;
    } else {
      int stageOffset = stageIdx - curIdx;
      DAWN_ASSERT_MSG(stageOffset < numStages, "invalid stage index");
      return StagePosition(multiStageIdx, stageOffset);
    }
  }
  dawn_unreachable("invalid stage index");
}

int Stencil::getStageIndexFromPosition(const Stencil::StagePosition& position) const {
  auto curMSIt = children_.begin();
  std::advance(curMSIt, position.MultiStageIndex);

  // Count the number of stages in the multistages before our current MS
  int numStagesInMSBeforeCurMS = std::accumulate(
      childrenBegin(), curMSIt, int(0), [&](int numStages, const MultiStageSmartPtr_t& MS) {
        return numStages + MS->getChildren().size();
      });

  // Add the current stage offset
  return numStagesInMSBeforeCurMS + position.StageOffset;
}

const std::unique_ptr<Stage>& Stencil::getStage(const StagePosition& position) const {
  // Get the multi-stage ...
  DAWN_ASSERT_MSG(position.MultiStageIndex < children_.size(), "invalid multi-stage index");
  auto msIt = children_.begin();
  std::advance(msIt, position.MultiStageIndex);
  const auto& MS = *msIt;

  // ... and the requested stage inside the given multi-stage
  DAWN_ASSERT_MSG(position.StageOffset == -1 || position.StageOffset < MS->getChildren().size(),
                  "invalid stage offset");
  auto stageIt = MS->childrenBegin();
  std::advance(stageIt, position.StageOffset == -1 ? 0 : position.StageOffset);
  return *stageIt;
}

const std::unique_ptr<Stage>& Stencil::getStage(int stageIdx) const {
  int curIdx = 0;
  for(const auto& MS : children_) {

    // Is our stage in this multi-stage?
    int numStages = MS->getChildren().size();

    if((curIdx + numStages) <= stageIdx) {
      // No... continue
      curIdx += numStages;
      continue;
    } else {
      // Yes... advance to our stage
      int stageOffset = stageIdx - curIdx;

      DAWN_ASSERT_MSG(stageOffset < MS->getChildren().size(), "invalid stage index");
      auto stageIt = MS->childrenBegin();
      std::advance(stageIt, stageOffset);

      return *stageIt;
    }
  }
  dawn_unreachable("invalid stage index");
}

void Stencil::insertStage(const StagePosition& position, std::unique_ptr<Stage>&& stage) {

  // Get the multi-stage ...
  DAWN_ASSERT_MSG(position.MultiStageIndex < children_.size(), "invalid multi-stage index");
  auto msIt = children_.begin();
  std::advance(msIt, position.MultiStageIndex);
  const auto& MS = *msIt;

  // ... and the requested stage inside the given multi-stage
  DAWN_ASSERT_MSG(position.StageOffset == -1 || position.StageOffset < MS->getChildren().size(),
                  "invalid stage offset");
  auto stageIt = MS->childrenBegin();

  // A stage offset of -1 indicates *before* the first element (thus nothing to do).
  // Otherwise we advance one beyond the requested stage as we insert *after* the specified
  // stage and `std::list::insert` inserts *before*.
  if(position.StageOffset != -1) {
    std::advance(stageIt, position.StageOffset);
    if(stageIt != MS->childrenEnd())
      stageIt++;
  }

  MS->insertChild(stageIt, std::move(stage));
}

Interval Stencil::getAxis(bool useExtendedInterval) const {
  int numStages = getNumStages();
  DAWN_ASSERT_MSG(numStages, "need atleast one stage");

  Interval axis = getStage(0)->getEnclosingExtendedInterval();
  for(int stageIdx = 1; stageIdx < numStages; ++stageIdx)
    axis.merge(useExtendedInterval ? getStage(stageIdx)->getEnclosingExtendedInterval()
                                   : getStage(stageIdx)->getEnclosingInterval());
  return axis;
}

void Stencil::renameAllOccurrences(int oldAccessID, int newAccessID) {
  for(const auto& multistage : getChildren()) {
    multistage->renameAllOccurrences(oldAccessID, newAccessID);
  }
}

std::unordered_map<int, Stencil::Lifetime>
Stencil::getLifetime(const std::unordered_set<int>& AccessIDs) const {
  std::unordered_map<int, Lifetime> lifetimeMap;
  for(int AccessID : AccessIDs) {
    lifetimeMap.emplace(AccessID, getLifetime(AccessID));
  }

  return lifetimeMap;
}

Stencil::Lifetime Stencil::getLifetime(const int AccessID) const {
  // use make_optional(false, ...) just to avoid a gcc warning
  boost::optional<StatementPosition> Begin = boost::make_optional(false, StatementPosition{});
  StatementPosition End;

  int multiStageIdx = 0;
  for(const auto& multistagePtr : children_) {

    int stageOffset = 0;
    for(const auto& stagePtr : multistagePtr->getChildren()) {

      int doMethodIndex = 0;
      for(const auto& doMethodPtr : stagePtr->getChildren()) {
        DoMethod& doMethod = *doMethodPtr;

        int statementIdx = 0;
        for(const auto& stmtAccessPair : doMethod.getChildren()) {
          const Accesses& accesses = *stmtAccessPair->getAccesses();

          auto processAccessMap = [&](const std::unordered_map<int, Extents>& accessMap) {
            if(!accessMap.count(AccessID))
              return;

            StatementPosition pos(StagePosition(multiStageIdx, stageOffset), doMethodIndex,
                                  statementIdx);

            if(!Begin.is_initialized())
              Begin = boost::make_optional(pos);
            End = pos;
          };

          processAccessMap(accesses.getWriteAccesses());
          processAccessMap(accesses.getReadAccesses());
          statementIdx++;
        }

        doMethodIndex++;
      }

      stageOffset++;
    }

    multiStageIdx++;
  }

  DAWN_ASSERT(Begin.is_initialized());

  return Lifetime(*Begin, End);
}

bool Stencil::isEmpty() const {
  for(const auto& MS : getChildren())
    for(const auto& stage : MS->getChildren())
      for(const auto& doMethod : stage->getChildren())
        if(!doMethod->childrenEmpty())
          return false;

  return true;
}

boost::optional<Interval> Stencil::getEnclosingIntervalTemporaries() const {
  boost::optional<Interval> tmpInterval;
  for(const auto& mss : getChildren()) {
    auto mssInterval = mss->getEnclosingAccessIntervalTemporaries();
    if(!mssInterval.is_initialized())
      continue;
    if(tmpInterval.is_initialized()) {
      tmpInterval->merge(*mssInterval);
    } else {
      tmpInterval = mssInterval;
    }
  }
  return tmpInterval;
}

void Stencil::accept(ASTVisitor& visitor) {
  for(const auto& stmtAccessesPairPtr : iterateIIROver<StatementAccessesPair>(*this)) {
    stmtAccessesPairPtr->getStatement()->ASTStmt->accept(visitor);
  }
}

} // namespace iir
} // namespace dawn
