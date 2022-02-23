cmake_minimum_required(VERSION 3.21)
project(kelcoro)

option(ENABLE_TESTS "Enable tests or not" OFF)

file(GLOB modules "${CMAKE_CURRENT_SOURCE_DIR}/include/*.ixx" CMAKE_CONFIGURE_DEPENDS)
if(ENABLE_TESTS)
    file(GLOB tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp CMAKE_CONFIGURE_DEPENDS)
endif() # else ${tests} is empty

add_executable(kelcoro ${tests} ${modules})
target_include_directories(kelcoro PUBLIC ../include)

if (MSVC)
    target_compile_options(kelcoro PUBLIC /std:c++latest)
else()
    message("\nNow (17.11.2021) its works only for msvc with /std:c++latest (NOT C++20 flag), Waiting for C++20 support from other compilers\n")
endif()
source_group(Modules FILES ${modules})
if (ENABLE_TESTS)
    source_group(Tests FILES ${tests})
endif()
