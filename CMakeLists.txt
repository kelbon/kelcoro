cmake_minimum_required(VERSION 3.21)
project(kelcoro)

option(KELCORO_ENABLE_TESTS "Enable tests or not" OFF)

file(GLOB modules "${CMAKE_CURRENT_SOURCE_DIR}/include/*.ixx" CMAKE_CONFIGURE_DEPENDS)
file(GLOB utility "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" CMAKE_CONFIGURE_DEPENDS)
if(KELCORO_ENABLE_TESTS)
    file(GLOB tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp CMAKE_CONFIGURE_DEPENDS)
endif() # else ${tests} is empty

add_executable(kelcoro ${tests} ${modules} ${utility})
target_include_directories(kelcoro PUBLIC ../include)
set_property(TARGET kelcoro PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(TARGET kelcoro PROPERTY CXX_STANDARD 23) # latest
source_group(Modules FILES ${modules})
if (KELCORO_ENABLE_TESTS)
    source_group(Tests FILES ${tests})
	enable_testing()
	add_subdirectory(tests)
endif()
source_group(Utility FILES ${utility})
