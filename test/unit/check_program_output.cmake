# SPDX-License-Identifier: Apache-2.0
#
# Runs PROGRAM with the arguments of ARGS, a list, then fails unless it exited with status 0, its
# output, standard error included, matches EXPECT, and it does not match FORBID. A test of
# exit-time code needs all three: a crash after the expected lines still fails it.
execute_process(COMMAND "${PROGRAM}" ${ARGS}
  RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
set(output "${out}${err}")
message("${output}")
if(NOT status STREQUAL "0")
  message(FATAL_ERROR "exit status: ${status}")
endif()
if(DEFINED EXPECT AND NOT output MATCHES "${EXPECT}")
  message(FATAL_ERROR "no match for: ${EXPECT}")
endif()
if(DEFINED FORBID AND output MATCHES "${FORBID}")
  message(FATAL_ERROR "match for: ${FORBID}")
endif()
