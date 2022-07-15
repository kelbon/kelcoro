cmake_minimum_required(VERSION 3.21)
project(kelcoro LANGUAGES CXX)

include(CTest)

file(GLOB headers "${CMAKE_CURRENT_SOURCE_DIR}/headers/*.hpp" CMAKE_CONFIGURE_DEPENDS)
add_library(kelcorolib INTERFACE ${headers})

target_include_directories(kelcorolib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/headers)

file(GLOB modules "${CMAKE_CURRENT_SOURCE_DIR}/include/*.ixx" CMAKE_CONFIGURE_DEPENDS)
file(GLOB utility "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp" CMAKE_CONFIGURE_DEPENDS)
if(BUILD_TESTING)
    file(GLOB tests_ "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp CMAKE_CONFIGURE_DEPENDS)
endif() # else ${tests_} is empty

add_executable(kelcoro ${tests_} ${modules} ${utility})
target_include_directories(kelcoro PUBLIC ../include)

set_target_properties(kelcorolib kelcoro PROPERTIES
LINKER_LANGUAGE CXX
CMAKE_CXX_STANDARD_REQUIRED ON
)
target_compile_features(kelcoro PUBLIC cxx_std_23)
target_compile_features(kelcorolib INTERFACE cxx_std_23)

source_group(Modules FILES ${modules})
if (BUILD_TESTING)
    source_group(Tests FILES ${tests_})
    add_subdirectory(tests)
endif()
source_group(Utility FILES ${utility})
