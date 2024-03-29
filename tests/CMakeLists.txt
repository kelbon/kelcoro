cmake_minimum_required(VERSION 3.05)

add_executable(kelcorotest ${CMAKE_CURRENT_SOURCE_DIR}/test_coroutines.cpp)
target_link_libraries(kelcorotest PUBLIC kelcorolib)
set_target_properties(kelcorotest PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20
)

# sanitizers do not work on clang with coroutines... Rly...
if(UNIX)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(kelcorotest PUBLIC "-fsanitize=undefined" "-fsanitize=address")
	target_link_options(kelcorotest PUBLIC "-fsanitize=undefined" "-fsanitize=address")
endif()
endif()
add_executable(generator_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests_generator.cpp)

target_link_libraries(generator_tests PUBLIC kelcorolib)
set_target_properties(generator_tests PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20
)
if (UNIX)
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(generator_tests PUBLIC "-fsanitize=undefined" "-fsanitize=address")
	target_link_options(generator_tests PUBLIC "-fsanitize=undefined" "-fsanitize=address")
endif()
endif()

add_test(NAME generator_tests COMMAND generator_tests)

add_test(NAME test_kelcorotest COMMAND kelcorotest)

add_executable(thread_pool_tests ${CMAKE_CURRENT_SOURCE_DIR}/test_thread_pool.cpp)

target_link_libraries(thread_pool_tests PUBLIC kelcorolib)

add_test(NAME test_thread_pool COMMAND thread_pool_tests)
