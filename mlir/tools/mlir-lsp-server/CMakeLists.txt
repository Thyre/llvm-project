set(LLVM_OPTIONAL_SOURCES
  null.cpp
)

get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
set(LLVM_LINK_COMPONENTS
  Core
  Support
  AsmParser
  )

if(MLIR_INCLUDE_TESTS)
  set(test_libs
    MLIRAffineTransformsTestPasses
    MLIRShapeTestPasses
    MLIRSPIRVTestPasses
    MLIRTestAnalysis
    MLIRTestDialect
    MLIRTestDynDialect
    MLIRTestIR
    MLIRTestPass
    MLIRTestReducer
    )
  set(test_libs
    ${test_libs}
    MLIRTestTransformDialect
    MLIRTestTransforms)

  if (MLIR_ENABLE_PDL_IN_PATTERNMATCH)
    set(test_libs
      ${test_libs}
      MLIRTestRewrite)
  endif()
endif()

set(LIBS
  ${conversion_libs}
  ${dialect_libs}
  ${extension_libs}

  MLIRAffineAnalysis
  MLIRAnalysis
  MLIRDialect
  MLIRFuncAllExtensions
  MLIRLspServerLib
  MLIRParser
  MLIRPass
  MLIRTensorAllExtensions
  MLIRTransforms
  MLIRTransformUtils
  MLIRSupport
  MLIRIR
  )

add_mlir_tool(mlir-lsp-server
  mlir-lsp-server.cpp
  )
mlir_target_link_libraries(mlir-lsp-server PRIVATE ${LIBS})
target_link_libraries(mlir-lsp-server PRIVATE ${test_libs})
llvm_update_compile_flags(mlir-lsp-server)

mlir_check_all_link_libraries(mlir-lsp-server)
