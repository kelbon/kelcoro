cmake_minimum_required(VERSION 3.05)

set(KELCORO_PARTS coroutines generator thread_pool stack_resource gate)
foreach(PART ${KELCORO_PARTS})
  add_executable(${PART} "${CMAKE_CURRENT_SOURCE_DIR}/test_${PART}.cpp")
  target_link_libraries(${PART} PUBLIC kelcorolib)
  set_target_properties(${PART} PROPERTIES
	  CMAKE_CXX_EXTENSIONS OFF
	  LINKER_LANGUAGE CXX
	  CMAKE_CXX_STANDARD_REQUIRED ON
	  CXX_STANDARD 20
  )

  add_test(NAME test_${PART} COMMAND ${PART})
endforeach()
