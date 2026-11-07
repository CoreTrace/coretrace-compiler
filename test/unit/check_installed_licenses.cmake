# SPDX-License-Identifier: Apache-2.0
#
# Installs the `licenses` component of the build tree BUILD_DIR into PREFIX, in the configuration
# CONFIG when a multi-config generator built it, then fails unless LICENSE and NOTICE of
# SOURCE_DIR lie unchanged in PREFIX/share/doc/coretrace-compiler.
file(REMOVE_RECURSE "${PREFIX}")
set(config_args)
if(CONFIG)
  set(config_args --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --component licenses
    --prefix "${PREFIX}" ${config_args}
  RESULT_VARIABLE status)
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "cmake --install exited with: ${status}")
endif()
foreach(file LICENSE NOTICE)
  set(installed "${PREFIX}/share/doc/coretrace-compiler/${file}")
  if(NOT EXISTS "${installed}")
    message(FATAL_ERROR "not installed: share/doc/coretrace-compiler/${file}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${SOURCE_DIR}/${file}" "${installed}"
    RESULT_VARIABLE differs)
  if(differs)
    message(FATAL_ERROR "differs from ${file} of the sources: share/doc/coretrace-compiler/${file}")
  endif()
endforeach()
