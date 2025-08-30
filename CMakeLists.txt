cmake_minimum_required(VERSION 3.24)
project(kelcoro LANGUAGES CXX)

option(KELCORO_ENABLE_TESTING "enables testing" OFF)
option(KELCORO_ENABLE_ASAN "build ASAN (for CI)" OFF)
option(KELCORO_ENABLE_UBSAN "build UNSAN (for CI)" OFF)

if (KELCORO_ENABLE_ASAN AND KELCORO_ENABLE_UBSAN)
   message(FATAL_ERROR "ASAN are incompatible with UBSAN (error: Cannot represent a difference across sections)")
endif()

if (KELCORO_ENABLE_ASAN)
  # fno omit frame pointer to improve trace from sanitizers
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

if (KELCORO_ENABLE_UBSAN)
  # fno omit frame pointer to improve trace from sanitizers
  add_compile_options(-fno-omit-frame-pointer -fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

### kelcorolib library ###

add_library(kelcorolib INTERFACE)

set_target_properties(kelcorolib PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20
)
target_include_directories(kelcorolib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

### TESTS ###

if(KELCORO_ENABLE_TESTING)
	include(CTest)
	add_subdirectory(tests)
endif()
