file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "*.cpp" "*.hpp")
source_group(TREE "${CMAKE_CURRENT_LIST_DIR}" FILES ${SRC})
add_library(gimuserver STATIC ${SRC})

# The generated header (packets/all.hpp) is produced by pkgen_generate,
# declared at the project root. Everything in gimuserver transitively
# depends on it via App.hpp, so pin the build-order edge here.
add_dependencies(gimuserver pkgen_generate)

target_link_libraries(gimuserver PUBLIC
    # third-party libraries
    Drogon::Drogon
    cryptopp::cryptopp
    unofficial::sqlite3::sqlite3
    glaze::glaze
    # project
    pkgen_cpp
)

target_include_directories(gimuserver
    PUBLIC
        ../
    PRIVATE
        .
)

target_precompile_headers(gimuserver PRIVATE App.hpp)

# forgeStatus.php names the commit the server was built from (an env
# BF_SERVER_COMMIT wins at run time).  Only forge/BuildInfo.cpp gets the
# definition, without the PCH, so a new commit recompiles that one file.
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE GIMU_SERVER_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GIMU_SERVER_COMMIT_RESULT
    ERROR_QUIET)
if (NOT GIMU_SERVER_COMMIT_RESULT EQUAL 0 OR GIMU_SERVER_COMMIT STREQUAL "")
    set(GIMU_SERVER_COMMIT "unknown")
endif()
set_source_files_properties(forge/BuildInfo.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "GIMU_SERVER_COMMIT=\"${GIMU_SERVER_COMMIT}\"")

# The MSVC/Ninja PCH dependency scan can retain the old packet layout after
# pkgen_generate runs. Keep explicit file dependencies in addition to the
# target ordering above, including translation units that skip the PCH.
set_property(SOURCE ${SRC}
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/gimuserver.dir/cmake_pch.cxx"
    APPEND PROPERTY OBJECT_DEPENDS "${PKGEN_OUT_HEADER};${PKGEN_ARCHIVE_OUT_HEADER}")

if (MSVC)
    target_compile_options(gimuserver PUBLIC "/constexpr:steps8388608")
    # ServerCache.cpp instantiates one glaze LoadJson<T> per cached MST
    # table; past ~50 tables the object file exceeds the default COFF
    # section limit (C1128).
    target_compile_options(gimuserver PRIVATE "/bigobj")
endif()

# MSVC 19.51 emits C4737 for these coroutine-heavy handlers under /O2,
# even without LTCG. Keep the workaround local; all other Release objects
# remain optimized. Recheck when upgrading the compiler.
if (MSVC)
    set_property(SOURCE gme/handlers/Mission.cpp gme/handlers/Present.cpp gme/handlers/UserInfo.cpp
        APPEND PROPERTY COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:/Od>")
    # MSVC otherwise restores /O2 from the shared PCH (C4653), ignoring /Od.
    set_source_files_properties(gme/handlers/Mission.cpp gme/handlers/Present.cpp gme/handlers/UserInfo.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
