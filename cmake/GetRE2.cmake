#=============================================================================
# Copyright 2017-2025, Manticore Software LTD (https://manticoresearch.com)
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================
# This file retrieves RE2 library sources and applies patches if needed.
# It first tries to find RE2 installed on the system.
# If not found, it looks in ${LIBS_BUNDLE} for re2-${RE2_BRANCH}.zip,
# and finally tries to download it from GitHub.

set(RE2_REPO "https://github.com/manticoresoftware/re2")
set(RE2_BRANCH "2015-06-01") # specific tag for reproducible builds
set(RE2_SRC_MD5 "023053ef20051a0fc5911a76869be59f")

set(RE2_GITHUB "${RE2_REPO}/archive/${RE2_BRANCH}.zip")
set(RE2_BUNDLE "${LIBS_BUNDLE}/re2-${RE2_BRANCH}.zip")

cmake_minimum_required(VERSION 3.17 FATAL_ERROR)
include(update_bundle)

# If allowed to use system library, try to use it
if(NOT WITH_RE2_FORCE_STATIC)
	find_package(re2 MODULE QUIET)
	return_if_target_found(re2::re2 "as default (sys or other) lib")
endif()

# Try to find a prebuilt RE2
find_package(re2 QUIET CONFIG)
return_if_target_found(re2::re2 "found ready (no need to build)")

function(PATCH_GIT RESULT RE2_SRC)
	find_package(Git QUIET)
	if(NOT GIT_EXECUTABLE)
		message(STATUS "Git not found. Skipping patch_git.")
		set(${RESULT} FALSE PARENT_SCOPE)
		return()
	endif()

	set(PATCH_FILE "${RE2_SRC}/libre2.patch")
	message(STATUS "Trying to apply Git patch...")
	message(STATUS "Patch file: ${PATCH_FILE}")
	
	file(READ "${PATCH_FILE}" PATCH_CONTENT)
	message(STATUS "Patch content:\n${PATCH_CONTENT}")

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --verbose libre2.patch
		WORKING_DIRECTORY "${RE2_SRC}"
		RESULT_VARIABLE PATCH_RESULT
		OUTPUT_VARIABLE PATCH_STDOUT
		ERROR_VARIABLE PATCH_STDERR
	)

	if(PATCH_RESULT EQUAL 0)
		message(STATUS "Patch applied successfully with Git.")
		set(${RESULT} TRUE PARENT_SCOPE)
	else()
		message(WARNING "Git patch failed with error code: ${PATCH_RESULT}")
		message(WARNING "Git patch stderr: ${PATCH_STDERR}")
		message(WARNING "Git patch stdout: ${PATCH_STDOUT}")
		set(${RESULT} FALSE PARENT_SCOPE)
	endif()
endfunction()

function(PATCH_PATCH RESULT RE2_SRC)
	find_program(PATCH_PROG patch)
	if(NOT PATCH_PROG)
		message(STATUS "System patch tool not found. Skipping patch_patch.")
		set(${RESULT} FALSE PARENT_SCOPE)
		return()
	endif()

	set(PATCH_FILE "${RE2_SRC}/libre2.patch")
	message(STATUS "Trying to apply patch with system 'patch' tool...")
	message(STATUS "Patch file: ${PATCH_FILE}")

	file(READ "${PATCH_FILE}" PATCH_CONTENT)
	message(STATUS "Patch content:\n${PATCH_CONTENT}")

	execute_process(
		COMMAND "${PATCH_PROG}" --verbose -p1 --binary -i libre2.patch
		WORKING_DIRECTORY "${RE2_SRC}"
		RESULT_VARIABLE PATCH_RESULT
		OUTPUT_VARIABLE PATCH_STDOUT
		ERROR_VARIABLE PATCH_STDERR
	)

	if(PATCH_RESULT EQUAL 0)
		message(STATUS "Patch applied successfully with system patch.")
		set(${RESULT} TRUE PARENT_SCOPE)
	else()
		message(WARNING "System patch failed with error code: ${PATCH_RESULT}")
		message(WARNING "System patch stderr: ${PATCH_STDERR}")
		message(WARNING "System patch stdout: ${PATCH_STDOUT}")
		message(WARNING "Tip: This may happen if RE2 version does not match the patch.")
		set(${RESULT} FALSE PARENT_SCOPE)
	endif()
endfunction()

# Finalize RE2 sources (apply patch, add CMake config)
function(PREPARE_RE2 RE2_SRC)
	if(EXISTS "${RE2_SRC}/is_patched.txt")
		message(STATUS "RE2 already patched. Skipping.")
		return()
	endif()

	message(STATUS "Copying patch to RE2 source dir...")
	file(COPY "${MANTICORE_SOURCE_DIR}/libre2/libre2.patch" DESTINATION "${RE2_SRC}")

	patch_git(PATCHED ${RE2_SRC})
	if(NOT PATCHED)
		patch_patch(PATCHED ${RE2_SRC})
	endif()

	if(NOT PATCHED)
		message(FATAL_ERROR "Couldn't patch RE2 distro. Patch failed with both Git and system 'patch'. Please verify that the patch matches the RE2 version (${RE2_BRANCH}) and that the source files are unmodified.")
	endif()

	file(WRITE "${RE2_SRC}/is_patched.txt" "ok")
	execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MANTICORE_SOURCE_DIR}/libre2/CMakeLists.txt" "${RE2_SRC}/CMakeLists.txt")
endfunction()

# Prepare sources
select_nearest_url(RE2_PLACE re2 ${RE2_BUNDLE} ${RE2_GITHUB})
fetch_and_check(re2 ${RE2_PLACE} ${RE2_SRC_MD5} RE2_SRC)
prepare_re2(${RE2_SRC})

# Build
get_build(RE2_BUILD re2)
external_build(re2 RE2_SRC RE2_BUILD)

# Now it should find the built library
find_package(re2 REQUIRED CONFIG)
return_if_target_found(re2::re2 "was built")
