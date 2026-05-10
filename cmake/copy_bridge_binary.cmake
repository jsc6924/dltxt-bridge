file(MAKE_DIRECTORY "${dst}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src}" "${dst}"
  RESULT_VARIABLE copy_result
  ERROR_VARIABLE copy_error
)

if(NOT copy_result EQUAL 0)
  string(STRIP "${copy_error}" copy_error)
  message(WARNING "Failed to copy bridge binary to extension bin directory: ${copy_error}")
endif()