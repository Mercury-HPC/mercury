#------------------------------------------------------------------------------
# Include source and build directories
#------------------------------------------------------------------------------
set(MERCURY_BUILD_INCLUDE_DEPENDENCIES
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

#-----------------------------------------------------------------------------
# Create executable
#-----------------------------------------------------------------------------
add_executable(hg_rpcgen rpc_hg_cout.c rpc_hg_hout.c rpc_hg_main.c
               rpc_parse.c rpc_scan.c rpc_util.c)
mercury_set_exe_options(hg_rpcgen MERCURY)
if(MERCURY_ENABLE_COVERAGE)
  set_coverage_flags(hg_rpcgen)
endif()

#-----------------------------------------------------------------------------
# Add Target(s) to CMake Install
#-----------------------------------------------------------------------------
install(
  TARGETS
    hg_rpcgen
  RUNTIME DESTINATION ${MERCURY_INSTALL_BIN_DIR}
)
