##===------------------------------------------------------------------------------*- CMake -*-===##
##                          _
##                         | |
##                       __| | __ ___      ___ ___
##                      / _` |/ _` \ \ /\ / / '_  |
##                     | (_| | (_| |\ V  V /| | | |
##                      \__,_|\__,_| \_/\_/ |_| |_| - Compiler Toolchain
##
##
##  This file is distributed under the MIT License (MIT).
##  See LICENSE.txt for details.
##
##===------------------------------------------------------------------------------------------===##

yoda_add_library(
  NAME DawnSerializer
  SOURCES ASTSerializer.h
          ASTSerializer.cpp
          IIRSerializer.h
          IIRSerializer.cpp
          SIRSerializer.h
          SIRSerializer.cpp
  OBJECT
)

yoda_combine_libraries(
  NAME DawnSerializer
  OBJECTS DawnSerializerObjects
  DEPENDS DawnIIRStatic DawnOptimizerStatic
  INSTALL_DESTINATION ${DAWN_INSTALL_LIB_DIR}
  VERSION ${DAWN_VERSION}
  EXPORT_GROUP DawnTargets
)

add_dependencies(DawnSerializerObjects IIR_Proto_Generated)
target_include_directories(DawnSerializerObjects PUBLIC $<TARGET_PROPERTY:DawnIIRStatic,INCLUDE_DIRECTORIES>)

target_include_directories(DawnSerializerObjects PUBLIC /mnt/iir-gtclang/prefix/include)

set(MLIR_LINK_LIBRARIES
        MLIRStandardOps
        MLIRIR
        MLIRParser
        MLIRTypeUtilities
        MLIRTransformUtils
        MLIRDialect
        MLIRMlirOptLib
        MLIRStencil
        MLIRPass
        MLIRAnalysis
        MLIRSupport
        MLIRTransforms
        MLIRStandardToLLVM
        LLVMCore
        LLVMSupport
        z rt dl tinfo pthread m xml2)

target_link_directories(DawnSerializerShared PUBLIC /mnt/iir-gtclang/prefix/lib)
#set_target_properties(DawnSerializerShared PROPERTIES LINK_FLAGS "-Wl,--whole-archive,-lMLIRStandardOps,-lMLIRIR,-lMLIRParser,-lMLIRTypeUtilities,-lMLIRTransformUtils,-lMLIRDialect,-lMLIRMlirOptLib,-lMLIRStencil,-lMLIRPass,-lMLIRAnalysis,-lMLIRSupport,-lMLIRTransforms,-lMLIRStandardToLLVM,--no-whole-archive")
target_link_libraries(DawnSerializerShared PUBLIC ${MLIR_LINK_LIBRARIES})

target_link_directories(DawnSerializerStatic PUBLIC /mnt/iir-gtclang/prefix/lib)
#set_target_properties(DawnSerializerStatic PROPERTIES LINK_FLAGS "-Wl,--whole-archive,-lMLIRStandardOps,-lMLIRIR,-lMLIRParser,-lMLIRTypeUtilities,-lMLIRTransformUtils,-lMLIRDialect,-lMLIRMlirOptLib,-lMLIRStencil,-lMLIRPass,-lMLIRAnalysis,-lMLIRSupport,-lMLIRTransforms,-lMLIRStandardToLLVM,--no-whole-archive")
target_link_libraries(DawnSerializerStatic PUBLIC ${MLIR_LINK_LIBRARIES})

#target_include_directories(DawnSerializerObjects PUBLIC ${sir_proto_include_dirs})
install(EXPORT DawnTargets NAMESPACE Dawn:: DESTINATION ${DAWN_INSTALL_CMAKE_DIR})

