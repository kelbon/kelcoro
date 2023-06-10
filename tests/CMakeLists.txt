cmake_minimum_required(VERSION 3.05)

add_executable(kelcorotest ${CMAKE_CURRENT_SOURCE_DIR}/test_coroutines.cpp)
target_link_libraries(kelcorotest PUBLIC kelcorolib)
set_target_properties(kelcorotest PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20
)

add_test(NAME test_kelcorotest COMMAND kelcorotest)

