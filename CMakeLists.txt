cmake_minimum_required(VERSION 3.20)

project(stellac CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_program(LLVM_CONFIG_EXE llvm-config REQUIRED)
set(LLVM_CONFIG ${LLVM_CONFIG_EXE} CACHE STRING "Path to llvm-config")

# LLVM flags
execute_process(
    COMMAND ${LLVM_CONFIG} --cxxflags
    OUTPUT_VARIABLE LLVM_CXXFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${LLVM_CONFIG} --ldflags
    OUTPUT_VARIABLE LLVM_LDFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${LLVM_CONFIG} --libs core irreader support passes target nativecodegen
    OUTPUT_VARIABLE LLVM_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LLVM_LIBS_RESULT)
execute_process(
    COMMAND ${LLVM_CONFIG} --system-libs
    OUTPUT_VARIABLE LLVM_SYSLIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LLVM_SYSLIBS_RESULT)

if(NOT LLVM_LIBS_RESULT EQUAL 0 OR NOT LLVM_SYSLIBS_RESULT EQUAL 0)
    message(FATAL_ERROR "llvm-config failed while querying LLVM libraries")
endif()

separate_arguments(LLVM_CXXFLAGS UNIX_COMMAND "${LLVM_CXXFLAGS}")
separate_arguments(LLVM_LDFLAGS  UNIX_COMMAND "${LLVM_LDFLAGS}")
separate_arguments(LLVM_LIBS    UNIX_COMMAND "${LLVM_LIBS}")
separate_arguments(LLVM_SYSLIBS UNIX_COMMAND "${LLVM_SYSLIBS}")

file(GLOB_RECURSE STELLA_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(stellac ${STELLA_SRCS})
target_include_directories(stellac PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(stellac PRIVATE ${LLVM_CXXFLAGS})
if(MSVC)
    target_compile_options(stellac PRIVATE /W4)
else()
    target_compile_options(stellac PRIVATE -Wall -Wextra)
endif()
target_link_options(stellac PRIVATE ${LLVM_LDFLAGS})
target_link_libraries(stellac PRIVATE ${LLVM_LIBS} ${LLVM_SYSLIBS})

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(stellac PRIVATE -O3)
endif()

install(TARGETS stellac RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    find_program(STELLA_TEST_CC NAMES clang cc REQUIRED)
    find_program(STELLA_TEST_NM NAMES llvm-nm nm REQUIRED)
    find_program(STELLA_TEST_LLC NAMES llc REQUIRED)
    add_test(
        NAME private_global_linkage
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/private_global_linkage.sh"
            "$<TARGET_FILE:stellac>"
            "${STELLA_TEST_CC}"
            "${STELLA_TEST_NM}"
            "${CMAKE_CURRENT_BINARY_DIR}/tests/private_global_linkage"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/private_global_linkage"
    )
    add_test(
        NAME language_features
        COMMAND sh
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/language_features.sh"
            "$<TARGET_FILE:stellac>"
            "${STELLA_TEST_CC}"
            "${STELLA_TEST_LLC}"
            "${CMAKE_CURRENT_BINARY_DIR}/tests/language_features"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/language_features"
    )
    add_test(
        NAME diagnostics
        COMMAND sh
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/diagnostics.sh"
            "$<TARGET_FILE:stellac>"
            "${CMAKE_CURRENT_BINARY_DIR}/tests/diagnostics"
    )
	add_test(
		NAME variadics
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/variadics.sh"
			"$<TARGET_FILE:stellac>" "${STELLA_TEST_CC}"
			"${CMAKE_CURRENT_BINARY_DIR}/tests/variadics"
			"${CMAKE_CURRENT_SOURCE_DIR}/tests/language_features"
	)
	add_test(
		NAME generic_imports
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_imports.sh"
			"$<TARGET_FILE:stellac>" "${STELLA_TEST_CC}"
			"${CMAKE_CURRENT_BINARY_DIR}/tests/generic_imports"
			"${CMAKE_CURRENT_SOURCE_DIR}/tests/language_features"
	)
	add_test(
		NAME slices
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/slices.sh"
			"$<TARGET_FILE:stellac>" "${STELLA_TEST_CC}"
			"${CMAKE_CURRENT_BINARY_DIR}/tests/slices"
	)
	add_test(
		NAME abi_and_repr
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/abi_and_repr.sh"
			"$<TARGET_FILE:stellac>" "${STELLA_TEST_CC}"
			"${CMAKE_CURRENT_BINARY_DIR}/tests/abi_and_repr"
	)
	add_test(
		NAME target_cpu_attrs
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/target_cpu_attrs.sh"
			"$<TARGET_FILE:stellac>" "${STELLA_TEST_LLC}"
			"${CMAKE_CURRENT_BINARY_DIR}/tests/target_cpu_attrs"
	)
	add_test(
		NAME tagged_enums_and_match
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/tagged_enums_and_match.sh"
			"$<TARGET_FILE:stellac>" "${STELLA_TEST_CC}"
			"${CMAKE_CURRENT_BINARY_DIR}/tests/tagged_enums_and_match"
	)
	add_test(
		NAME src_embed
		COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/src_embed.sh"
			"$<TARGET_FILE:stellac>" "${STELLA_TEST_CC}"
			"${CMAKE_CURRENT_BINARY_DIR}/tests/src_embed"
	)
endif()
