cmake_minimum_required(VERSION 3.05)
project(kelcoro LANGUAGES CXX)

option(KELCORO_ENABLE_TESTING "enables testing" OFF)

### kelcorolib library ###

file(GLOB KELCORO_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" CMAKE_CONFIGURE_DEPENDS)
add_library(kelcorolib INTERFACE ${KELCORO_HEADERS})

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

### VISUAL ###

source_group(Include FILES ${modules})



