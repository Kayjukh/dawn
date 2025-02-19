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

#include "dawn/Compiler/DiagnosticsEngine.h"
#include "dawn/Compiler/Options.h"
#include "dawn/IIR/IIR.h"
#include "dawn/IIR/StatementAccessesPair.h"
#include "dawn/IIR/StencilInstantiation.h"
#include "dawn/Optimizer/OptimizerContext.h"
#include "dawn/Serialization/IIRSerializer.h"
#include <gtest/gtest.h>

using namespace dawn;

namespace {
#define IIR_EARLY_EXIT(value)                                                                      \
  if(!value)                                                                                       \
    return false;

#define IIR_EXPECT_IMPL(iir1, iir2, VALUE)                                                         \
  do {                                                                                             \
    EXPECT_##VALUE(compareStencilInstantiations(iir1, iir2));                                      \
  } while(0);

#define IIR_EXPECT_EQ(iir1, iir2) IIR_EXPECT_IMPL((iir1), (iir2), TRUE)
#define IIR_EXPECT_NE(iir1, iir2) IIR_EXPECT_IMPL((iir1), (iir2), FALSE)

bool compareIIRs(iir::IIR* lhs, iir::IIR* rhs) {
  IIR_EARLY_EXIT(lhs->checkTreeConsistency());
  IIR_EARLY_EXIT(rhs->checkTreeConsistency());
  // checking the stencils
  for(int stencils = 0, size = lhs->getChildren().size(); stencils < size; ++stencils) {
    const auto& lhsStencil = lhs->getChild(stencils);
    const auto& rhsStencil = rhs->getChild(stencils);
    IIR_EARLY_EXIT((lhsStencil->getStencilAttributes() == rhsStencil->getStencilAttributes()));
    IIR_EARLY_EXIT((lhsStencil->getStencilID() == rhsStencil->getStencilID()));

    // checking each of the multistages
    for(int mssidx = 0, mssSize = lhsStencil->getChildren().size(); mssidx < mssSize; ++mssidx) {
      const auto& lhsMSS = lhsStencil->getChild(mssidx);
      const auto& rhsMSS = rhsStencil->getChild(mssidx);
      IIR_EARLY_EXIT((lhsMSS->getLoopOrder() == rhsMSS->getLoopOrder()));
      IIR_EARLY_EXIT((lhsMSS->getID() == rhsMSS->getID()));
      IIR_EARLY_EXIT((lhsMSS->getCaches().size() == rhsMSS->getCaches().size()));
      for(const auto& lhsPair : lhsMSS->getCaches()) {
        IIR_EARLY_EXIT(rhsMSS->getCaches().count(lhsPair.first));
        auto rhsValue = rhsMSS->getCaches().at(lhsPair.first);
        IIR_EARLY_EXIT((rhsValue == lhsPair.second));
      }
      // checking each of the stages
      for(int stageidx = 0, stageSize = lhsMSS->getChildren().size(); stageidx < stageSize;
          ++stageidx) {
        const auto& lhsStage = lhsMSS->getChild(stageidx);
        const auto& rhsStage = rhsMSS->getChild(stageidx);
        IIR_EARLY_EXIT((lhsStage->getStageID() == rhsStage->getStageID()));

        // checking each of the doMethods
        for(int doMethodidx = 0, doMethodSize = lhsStage->getChildren().size();
            doMethodidx < doMethodSize; ++doMethodidx) {
          const auto& lhsDoMethod = lhsStage->getChild(doMethodidx);
          const auto& rhsDoMethod = rhsStage->getChild(doMethodidx);
          IIR_EARLY_EXIT((lhsDoMethod->getID() == rhsDoMethod->getID()));
          IIR_EARLY_EXIT((lhsDoMethod->getInterval() == rhsDoMethod->getInterval()));

          // checking each of the StmtAccesspairs
          for(int stmtidx = 0, stmtSize = lhsDoMethod->getChildren().size(); stmtidx < stmtSize;
              ++stageidx) {
            const auto& lhsStmt = lhsDoMethod->getChild(stmtidx);
            const auto& rhsStmt = rhsDoMethod->getChild(stmtidx);
            // check the statement
            IIR_EARLY_EXIT(
                (lhsStmt->getStatement()->ASTStmt->equals(rhsStmt->getStatement()->ASTStmt.get())));

            // check the accesses
            IIR_EARLY_EXIT((lhsStmt->getCallerAccesses() == rhsStmt->getCallerAccesses()));
            if(lhsStmt->getCallerAccesses()) {
              for(const auto& lhsPair : rhsStmt->getCallerAccesses()->getReadAccesses()) {
                IIR_EARLY_EXIT(
                    rhsStmt->getCallerAccesses()->getReadAccesses().count(lhsPair.first));
                auto rhsValue = rhsStmt->getCallerAccesses()->getReadAccesses().at(lhsPair.first);
                IIR_EARLY_EXIT((rhsValue == lhsPair.second));
              }
              for(const auto& lhsPair : rhsStmt->getCallerAccesses()->getWriteAccesses()) {
                IIR_EARLY_EXIT(
                    rhsStmt->getCallerAccesses()->getWriteAccesses().count(lhsPair.first));
                auto rhsValue = rhsStmt->getCallerAccesses()->getWriteAccesses().at(lhsPair.first);
                IIR_EARLY_EXIT((rhsValue == lhsPair.second));
              }
            }
          }
        }
      }
    }
  }
  const auto& lhsControlFlowStmts = lhs->getControlFlowDescriptor().getStatements();
  const auto& rhsControlFlowStmts = rhs->getControlFlowDescriptor().getStatements();

  IIR_EARLY_EXIT((lhsControlFlowStmts.size() == rhsControlFlowStmts.size()));
  for(int i = 0, size = lhsControlFlowStmts.size(); i < size; ++i) {
    if(!lhsControlFlowStmts[i]->ASTStmt->equals(rhsControlFlowStmts[i]->ASTStmt.get()))
      return false;
    if(lhsControlFlowStmts[i]->StackTrace) {
      if(rhsControlFlowStmts[i]->StackTrace) {
        for(int j = 0, jsize = lhsControlFlowStmts[i]->StackTrace->size(); j < jsize; ++j) {
          if(!(lhsControlFlowStmts[i]->StackTrace->at(j) ==
               rhsControlFlowStmts[i]->StackTrace->at(j))) {
            return false;
          }
        }
      }
      return false;
    }
  }

  return true;
}
bool compareMetaData(iir::StencilMetaInformation& lhs, iir::StencilMetaInformation& rhs) {
  IIR_EARLY_EXIT((lhs.getExprIDToAccessIDMap() == rhs.getExprIDToAccessIDMap()));
  IIR_EARLY_EXIT((lhs.getStmtIDToAccessIDMap() == rhs.getStmtIDToAccessIDMap()));
  IIR_EARLY_EXIT((lhs.getAccessesOfType<iir::FieldAccessType::FAT_Literal>() ==
                  rhs.getAccessesOfType<iir::FieldAccessType::FAT_Literal>()));
  IIR_EARLY_EXIT((lhs.getAccessesOfType<iir::FieldAccessType::FAT_Field>() ==
                  rhs.getAccessesOfType<iir::FieldAccessType::FAT_Field>()));
  IIR_EARLY_EXIT((lhs.getAccessesOfType<iir::FieldAccessType::FAT_APIField>() ==
                  rhs.getAccessesOfType<iir::FieldAccessType::FAT_APIField>()));
  IIR_EARLY_EXIT((lhs.getAccessesOfType<iir::FieldAccessType::FAT_StencilTemporary>() ==
                  rhs.getAccessesOfType<iir::FieldAccessType::FAT_StencilTemporary>()));
  IIR_EARLY_EXIT((lhs.getAccessesOfType<iir::FieldAccessType::FAT_GlobalVariable>() ==
                  rhs.getAccessesOfType<iir::FieldAccessType::FAT_GlobalVariable>()));

  // we compare the content of the maps since the shared-ptr's are not the same
  IIR_EARLY_EXIT((lhs.getFieldNameToBCMap().size() == rhs.getFieldNameToBCMap().size()));
  for(const auto& lhsPair : lhs.getFieldNameToBCMap()) {
    IIR_EARLY_EXIT(rhs.getFieldNameToBCMap().count(lhsPair.first));
    auto rhsValue = rhs.getFieldNameToBCMap().at(lhsPair.first);
    IIR_EARLY_EXIT(rhsValue->equals(lhsPair.second.get()));
  }
  IIR_EARLY_EXIT((lhs.getFieldIDToDimsMap() == rhs.getFieldIDToDimsMap()));
  IIR_EARLY_EXIT((lhs.getStencilLocation() == rhs.getStencilLocation()));
  IIR_EARLY_EXIT((lhs.getStencilName() == rhs.getStencilName()));
  IIR_EARLY_EXIT((lhs.getFileName() == rhs.getFileName()));

  // we compare the content of the maps since the shared-ptr's are not the same
  IIR_EARLY_EXIT((lhs.getStencilIDToStencilCallMap().getDirectMap().size() ==
                  rhs.getStencilIDToStencilCallMap().getDirectMap().size()));
  for(const auto& lhsPair : lhs.getStencilIDToStencilCallMap().getDirectMap()) {
    IIR_EARLY_EXIT(rhs.getStencilIDToStencilCallMap().getDirectMap().count(lhsPair.first));
    auto rhsValue = rhs.getStencilIDToStencilCallMap().getDirectMap().at(lhsPair.first);
    IIR_EARLY_EXIT(rhsValue->equals(lhsPair.second.get()));
  }
  return true;
}

bool compareStencilInstantiations(const std::shared_ptr<iir::StencilInstantiation>& lhs,
                                  const std::shared_ptr<iir::StencilInstantiation>& rhs) {
  IIR_EARLY_EXIT(compareIIRs(lhs->getIIR().get(), rhs->getIIR().get()));
  IIR_EARLY_EXIT(compareMetaData(lhs->getMetaData(), rhs->getMetaData()));
  return true;
}

class createEmptyOptimizerContext : public ::testing::Test {
protected:
  virtual void SetUp() {
    dawn::DiagnosticsEngine diag;
    dawn::Options options;
    std::shared_ptr<SIR> sir = std::make_shared<SIR>();
    context_ = new OptimizerContext(diag, options, sir);
  }
  virtual void TearDown() override {}
  OptimizerContext* context_;
};

class IIRSerializerTest : public createEmptyOptimizerContext {
protected:
  virtual void SetUp() override {
    createEmptyOptimizerContext::SetUp();
    referenceInstantiaton = std::make_shared<iir::StencilInstantiation>(context_);
  }
  virtual void TearDown() override { referenceInstantiaton.reset(); }

  std::shared_ptr<iir::StencilInstantiation> serializeAndDeserializeRef() {
    return std::move(IIRSerializer::deserializeFromString(
        IIRSerializer::serializeToString(referenceInstantiaton), context_));
  }

  std::shared_ptr<iir::StencilInstantiation> referenceInstantiaton;
};

TEST_F(IIRSerializerTest, EmptySetup) {
  auto desired = serializeAndDeserializeRef();
  IIR_EXPECT_EQ(desired, referenceInstantiaton);
  desired->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_InterStencilTemporary, 10,
                                            "name");
  IIR_EXPECT_NE(desired, referenceInstantiaton);
}
TEST_F(IIRSerializerTest, SimpleDataStructures) {
  //===------------------------------------------------------------------------------------------===
  // Checking inserts into the various maps
  //===------------------------------------------------------------------------------------------===
  referenceInstantiaton->getMetaData().setAccessIDNamePair(1, "test");
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  referenceInstantiaton->getMetaData().insertExprToAccessID(std::make_shared<NOPExpr>(), 5);
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  referenceInstantiaton->getMetaData().insertStmtToAccessID(
      std::make_shared<ExprStmt>(std::make_shared<NOPExpr>()), 10);
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  referenceInstantiaton->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_Literal, 5,
                                                          "test");
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  referenceInstantiaton->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_Field, 712,
                                                          "field0");
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  referenceInstantiaton->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_APIField, 10,
                                                          "field1");
  referenceInstantiaton->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_APIField, 12,
                                                          "field2");
  auto deserializedStencilInstantiaion = serializeAndDeserializeRef();
  IIR_EXPECT_EQ(deserializedStencilInstantiaion, referenceInstantiaton);

  // check that ordering is preserved
  referenceInstantiaton->getMetaData().removeAccessID(12);
  referenceInstantiaton->getMetaData().removeAccessID(10);

  referenceInstantiaton->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_APIField, 12,
                                                          "field1");
  referenceInstantiaton->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_APIField, 10,
                                                          "field2");

  IIR_EXPECT_NE(deserializedStencilInstantiaion, referenceInstantiaton);

  referenceInstantiaton->getMetaData().insertAccessOfType(
      iir::FieldAccessType::FAT_StencilTemporary, 712, "field3");
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  // TODO this should not be legal, since 712 was already inserted
  referenceInstantiaton->getMetaData().insertAccessOfType(iir::FieldAccessType::FAT_GlobalVariable,
                                                          712, "field4");
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  referenceInstantiaton->getMetaData().insertFieldVersionIDPair(5, 6);
  referenceInstantiaton->getMetaData().insertFieldVersionIDPair(5, 7);
  referenceInstantiaton->getMetaData().insertFieldVersionIDPair(5, 8);
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  referenceInstantiaton->getMetaData().setFileName("fileName");
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);
  referenceInstantiaton->getMetaData().setStencilname("stencilName");
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);
  referenceInstantiaton->getMetaData().setStencilLocation(SourceLocation{1, 2});
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);
}

TEST_F(IIRSerializerTest, ComplexStrucutes) {
  auto statement = std::make_shared<Statement>(
      std::make_shared<StencilCallDeclStmt>(std::make_shared<sir::StencilCall>("me")), nullptr);
  statement->ASTStmt->getSourceLocation().Line = 10;
  statement->ASTStmt->getSourceLocation().Column = 12;
  referenceInstantiaton->getIIR()->getControlFlowDescriptor().insertStmt(statement);
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  auto stmt = std::make_shared<StencilCallDeclStmt>(std::make_shared<sir::StencilCall>("test"));
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  auto bcstmt = std::make_shared<BoundaryConditionDeclStmt>("callee");
  bcstmt->getFields().push_back(std::make_shared<sir::Field>("field1"));
  bcstmt->getFields().push_back(std::make_shared<sir::Field>("field2"));
  referenceInstantiaton->getMetaData().insertFieldBC("bc", bcstmt);
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);
}

TEST_F(IIRSerializerTest, IIRTests) {
  sir::Attr attributes;
  attributes.set(sir::Attr::AK_MergeStages);
  referenceInstantiaton->getIIR()->insertChild(
      make_unique<iir::Stencil>(referenceInstantiaton->getMetaData(), attributes, 10),
      referenceInstantiaton->getIIR());
  const auto& IIRStencil = referenceInstantiaton->getIIR()->getChild(0);
  auto deserialized = serializeAndDeserializeRef();
  IIR_EXPECT_EQ(deserialized, referenceInstantiaton);
  IIRStencil->getStencilAttributes().set(sir::Attr::AK_NoCodeGen);
  IIR_EXPECT_NE(deserialized, referenceInstantiaton);

  (IIRStencil)
      ->insertChild(make_unique<iir::MultiStage>(referenceInstantiaton->getMetaData(),
                                                 iir::LoopOrderKind::LK_Backward));
  const auto& IIRMSS = (IIRStencil)->getChild(0);
  IIRMSS->getCaches().emplace(
      10, iir::Cache(iir::Cache::IJ, iir::Cache::fill, 10, boost::none, boost::none, boost::none));
  deserialized = serializeAndDeserializeRef();
  IIR_EXPECT_EQ(deserialized, referenceInstantiaton);
  IIRMSS->setLoopOrder(iir::LoopOrderKind::LK_Forward);
  IIR_EXPECT_NE(deserialized, referenceInstantiaton);

  IIRMSS->insertChild(make_unique<iir::Stage>(referenceInstantiaton->getMetaData(), 12));
  const auto& IIRStage = IIRMSS->getChild(0);
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  (IIRStage)->insertChild(
      make_unique<iir::DoMethod>(iir::Interval(1, 5, 0, 1), referenceInstantiaton->getMetaData()));
  IIR_EXPECT_EQ(serializeAndDeserializeRef(), referenceInstantiaton);

  auto& IIRDoMethod = (IIRStage)->getChild(0);
  auto expr = std::make_shared<VarAccessExpr>("name");
  auto stmt = std::make_shared<ExprStmt>(expr);
  stmt->setID(22);
  auto statement = std::make_shared<Statement>(stmt, nullptr);
  auto stmtAccessPair = make_unique<iir::StatementAccessesPair>(statement);
  std::shared_ptr<iir::Accesses> callerAccesses = std::make_shared<iir::Accesses>();
  stmtAccessPair->setCallerAccesses(callerAccesses);

  (IIRDoMethod)->insertChild(std::move(stmtAccessPair));
}

} // anonymous namespace
