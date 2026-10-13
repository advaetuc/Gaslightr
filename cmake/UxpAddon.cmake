option(GASLIGHTR_BUILD_UXP_ADDON "Build Adobe's native UXP addon" OFF)
if(GASLIGHTR_BUILD_UXP_ADDON)
    set(_adobe "${PROJECT_SOURCE_DIR}/vendor/adobe/uxp")
    set(_adobe_sources
        "${_adobe}/utilities/UxpAddon.cpp"
        "${_adobe}/utilities/UxpTask.cpp"
        "${_adobe}/utilities/UxpValue.cpp")
    foreach(_file IN LISTS _adobe_sources)
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "Missing ${_file}. Run scripts/Stage-AdobeSdk.ps1 first.")
        endif()
    endforeach()
    # Isolate SDK warnings from our /WX policy; PCH is disabled for EVERY unit.
    add_library(gaslightr_adobe_sdk STATIC ${_adobe_sources})
    set_target_properties(gaslightr_adobe_sdk PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        DISABLE_PRECOMPILE_HEADERS ON)
    set_source_files_properties(${_adobe_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(gaslightr_adobe_sdk SYSTEM PUBLIC
        "${_adobe}/api" "${_adobe}/utilities")
    target_compile_definitions(gaslightr_adobe_sdk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(gaslightr_adobe_sdk PRIVATE /Y- /Z7 /EHsc /utf-8 /W3)

    add_library(gaslightr_uxpaddon SHARED "${PROJECT_SOURCE_DIR}/src/native/UxpAddonAdapter.cpp")
    gaslightr_compile(gaslightr_uxpaddon)
    target_compile_options(gaslightr_uxpaddon PRIVATE /Y- /Z7)
    target_link_options(gaslightr_uxpaddon PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
    target_link_libraries(gaslightr_uxpaddon PRIVATE GaslightrCore gaslightr_adobe_sdk d3d12.lib dxgi.lib)
    set_target_properties(gaslightr_uxpaddon PROPERTIES
        PREFIX "" OUTPUT_NAME "gaslightr" SUFFIX ".uxpaddon"
        PDB_NAME "gaslightr"
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        DISABLE_PRECOMPILE_HEADERS ON
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/native/$<CONFIG>")
    # PDBs for core breakpoints in Release without changing Release's /MT runtime.
    target_compile_options(GaslightrCore PRIVATE /Z7)
    set(_stage "${PROJECT_SOURCE_DIR}/uxp-frontend/win/x64")
    add_custom_command(TARGET gaslightr_uxpaddon POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:gaslightr_uxpaddon>" "${_stage}/gaslightr.uxpaddon"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_PDB_FILE:gaslightr_uxpaddon>" "${_stage}/gaslightr.pdb"
        COMMENT "Staging native addon and matching symbols into the UXP bundle"
        VERBATIM)
endif()
