cmake_minimum_required(VERSION 3.25)

project(NeuroShade VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

option(NS_BUILD_TESTBED "Build the deterministic Vulkan testbed" ON)
option(NS_BUILD_LAYER "Build the explicit Vulkan layer" ON)
option(NS_BUILD_HIP_INTEROP "Build the Vulkan/HIP interop runtime and qualification test" ON)
option(NS_BUILD_NEURAL "Build the MIGraphX spatial neural runtime when available" ON)
option(NS_BUILD_DLSSNR_TOOLS "Build native DLSSNR weight inspection (not an inference backend)" ON)
option(NS_BUILD_DLSSNR_HIP "Build the optional native DLSSNR HIP host64 runtime" OFF)
option(NS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(NS_HIP_ARCHITECTURES "gfx1200" CACHE STRING "AMD GPU architectures for bundled HIP kernels")
set(NS_MIGRAPHX_ROOT "" CACHE PATH "Optional MIGraphX installation prefix")
set(NS_VALIDATION_LAYER_LIBRARY "" CACHE FILEPATH
    "Optional libVkLayer_khronos_validation.so used when no system manifest is installed")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NS_BUILD_DLSSNR_HIP)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "DLSSNR HIP runs in host64; keep this option OFF for x86 layers")
    endif()
    find_path(NS_DLSSNR_HIP_INCLUDE hip/hip_runtime_api.h HINTS /opt/rocm/include REQUIRED)
    find_library(NS_DLSSNR_HIP_LIBRARY amdhip64 HINTS /opt/rocm/lib REQUIRED)
    if(NOT DEFINED CMAKE_HIP_COMPILER AND EXISTS "/opt/rocm/lib/llvm/bin/clang++")
        set(CMAKE_HIP_COMPILER "/opt/rocm/lib/llvm/bin/clang++" CACHE FILEPATH "HIP compiler")
    endif()
    set(CMAKE_HIP_ARCHITECTURES "${NS_HIP_ARCHITECTURES}" CACHE STRING "HIP architectures" FORCE)
    enable_language(HIP)
    add_library(neuroshade_dlssnr SHARED src/neural/dlssnr/native_runtime.cpp src/neural/dlssnr/frame_io.hip)
    set_target_properties(neuroshade_dlssnr PROPERTIES HIP_ARCHITECTURES "${NS_HIP_ARCHITECTURES}")
    target_compile_features(neuroshade_dlssnr PRIVATE cxx_std_20)
    target_compile_definitions(neuroshade_dlssnr PRIVATE __HIP_PLATFORM_AMD__=1)
    target_include_directories(neuroshade_dlssnr SYSTEM PRIVATE "${NS_DLSSNR_HIP_INCLUDE}")
    target_link_libraries(neuroshade_dlssnr PRIVATE "${NS_DLSSNR_HIP_LIBRARY}")
    add_executable(ns-dlssnr-profile tools/ns-dlssnr-profile/main.cpp)
    target_compile_features(ns-dlssnr-profile PRIVATE cxx_std_20)
    target_link_libraries(ns-dlssnr-profile PRIVATE neuroshade_dlssnr)
    find_package(Vulkan 1.2 REQUIRED)
    add_executable(ns-dlssnr-interop tools/ns-dlssnr-interop/main.cpp src/vulkan/interop_buffers/interop_pool.cpp)
    target_compile_features(ns-dlssnr-interop PRIVATE cxx_std_20)
    target_include_directories(ns-dlssnr-interop PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(ns-dlssnr-interop PRIVATE neuroshade_dlssnr Vulkan::Vulkan)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NS_JSONC REQUIRED IMPORTED_TARGET json-c)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    add_executable(ns-dlssnr-host tools/ns-dlssnr-host/main.cpp)
    target_compile_features(ns-dlssnr-host PRIVATE cxx_std_20)
    target_link_libraries(ns-dlssnr-host PRIVATE neuroshade_dlssnr PkgConfig::NS_JSONC OpenSSL::Crypto)
    target_compile_options(ns-dlssnr-host PRIVATE -Wall -Wextra -Wpedantic)
    if(NS_WARNINGS_AS_ERRORS)
        target_compile_options(ns-dlssnr-host PRIVATE -Werror)
    endif()
    file(RELATIVE_PATH NS_DLSSNR_PROFILE_LIB_RELATIVE "/${CMAKE_INSTALL_LIBEXECDIR}/neuroshade" "/${CMAKE_INSTALL_DATADIR}/neuroshade/runtime/lib")
    set_target_properties(ns-dlssnr-profile PROPERTIES INSTALL_RPATH "$ORIGIN/${NS_DLSSNR_PROFILE_LIB_RELATIVE}")
    set_target_properties(ns-dlssnr-host PROPERTIES INSTALL_RPATH "$ORIGIN/${NS_DLSSNR_PROFILE_LIB_RELATIVE}")
    install(TARGETS ns-dlssnr-host RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/neuroshade COMPONENT DLSSNRNative)
    install(TARGETS ns-dlssnr-profile RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/neuroshade)
    install(TARGETS neuroshade_dlssnr LIBRARY DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/runtime/lib COMPONENT DLSSNRNative)
endif()

if(NS_BUILD_DLSSNR_TOOLS)
    add_library(neuroshade_dlssnr_weights STATIC src/neural/dlssnr/weight_archive.cpp)
    target_compile_features(neuroshade_dlssnr_weights PUBLIC cxx_std_20)
    target_include_directories(neuroshade_dlssnr_weights PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(neuroshade_dlssnr_weights PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_executable(ns-dlssnr-inspect tools/ns-dlssnr-inspect/main.cpp)
    target_link_libraries(ns-dlssnr-inspect PRIVATE neuroshade_dlssnr_weights)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(neuroshade_dlssnr_weights PRIVATE -Wall -Wextra -Wpedantic)
        if(NS_WARNINGS_AS_ERRORS)
            target_compile_options(neuroshade_dlssnr_weights PRIVATE -Werror)
        endif()
    endif()
    if(BUILD_TESTING)
        add_executable(ns-dlssnr-weights-test tests/unit/dlssnr_weights_tests.cpp)
        target_link_libraries(ns-dlssnr-weights-test PRIVATE neuroshade_dlssnr_weights)
        add_test(NAME unit.dlssnr_weights COMMAND ns-dlssnr-weights-test)
    endif()
    install(TARGETS ns-dlssnr-inspect RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/neuroshade)
endif()

file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
configure_file(tools/neuroshade-cli/neuroshade.in
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/neuroshade" @ONLY
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
configure_file(tools/neuroshade-run/neuroshade-setup
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/neuroshade-setup" COPYONLY)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4)
endif()
if(GTK4_FOUND)
    add_executable(neuroshade-desktop frontend/desktop/main.cpp)
    target_compile_features(neuroshade-desktop PRIVATE cxx_std_20)
    target_link_libraries(neuroshade-desktop PRIVATE PkgConfig::GTK4)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(neuroshade-desktop PRIVATE -Wall -Wextra -Wpedantic)
        if(NS_WARNINGS_AS_ERRORS)
            target_compile_options(neuroshade-desktop PRIVATE -Werror)
        endif()
    endif()
else()
    message(STATUS "GTK4 not found: using the Zenity desktop frontend fallback")
    configure_file(frontend/desktop/neuroshade-desktop.in
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/neuroshade-desktop" @ONLY
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
configure_file(frontend/desktop/org.neuroshade.NeuroShade.desktop.in
    "${CMAKE_BINARY_DIR}/org.neuroshade.NeuroShade.desktop" @ONLY)
configure_file(cmake/record_plugin_hashes.cmake.in
    "${CMAKE_BINARY_DIR}/record_plugin_hashes.cmake" @ONLY)

add_library(neuroshade_common STATIC
    src/logging/log.cpp
    src/framegraph/compiler/compiler.cpp
    src/plugins/shader/manifest.cpp
    src/profile/profile.cpp
    src/layer/resource_tracker/resource_tracker.cpp
    src/layer/resource_tracker/analyzer.cpp
    src/layer/resource_tracker/bindings.cpp
    src/layer/resource_tracker/preview.cpp
    src/neural/model/package.cpp
    src/overlay/overlay_state.cpp
    src/runtime/pipeline_plan.cpp
)
add_library(NeuroShade::common ALIAS neuroshade_common)
set_target_properties(neuroshade_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_features(neuroshade_common PUBLIC cxx_std_20)
target_include_directories(neuroshade_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/neuroshade>
)

add_executable(ns-package-inspect tools/neuroshade-cli/package_inspect.cpp)
target_compile_features(ns-package-inspect PRIVATE cxx_std_20)
target_link_libraries(ns-package-inspect PRIVATE NeuroShade::common)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(neuroshade_common PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ns-package-inspect PRIVATE -Wall -Wextra -Wpedantic)
    if(NS_WARNINGS_AS_ERRORS)
        target_compile_options(neuroshade_common PRIVATE -Werror)
        target_compile_options(ns-package-inspect PRIVATE -Werror)
    endif()
endif()

set(NS_MIGRAPHX_AVAILABLE OFF)
if(NS_BUILD_NEURAL)
    if(NOT NS_MIGRAPHX_ROOT AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/migraphx-root/opt/rocm/lib/cmake/migraphx")
        set(NS_MIGRAPHX_ROOT
            "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/migraphx-root/opt/rocm")
    endif()
    find_package(hiprtc QUIET CONFIG PATHS /opt/rocm PATH_SUFFIXES lib/cmake/hiprtc)
    find_package(migraphx QUIET CONFIG
        PATHS "${NS_MIGRAPHX_ROOT}" /opt/rocm
        PATH_SUFFIXES lib/cmake/migraphx)
    if(migraphx_FOUND)
        set(NS_MIGRAPHX_AVAILABLE ON)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(NS_BUNDLED_MODEL_DIR
            "${CMAKE_BINARY_DIR}/share/neuroshade/models/spatial_gain.nsmodel")
        set(NS_BUNDLED_MODEL_FILES
            "${NS_BUNDLED_MODEL_DIR}/manifest.json"
            "${NS_BUNDLED_MODEL_DIR}/model.onnx"
            "${NS_BUNDLED_MODEL_DIR}/signature.json"
            "${NS_BUNDLED_MODEL_DIR}/metadata.json"
            "${NS_BUNDLED_MODEL_DIR}/preview.webp")
        add_custom_command(
            OUTPUT ${NS_BUNDLED_MODEL_FILES}
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/generate_spatial_model.py"
                    "${NS_BUNDLED_MODEL_DIR}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/generate_spatial_model.py"
            VERBATIM)
        add_custom_target(neuroshade_models ALL
            DEPENDS ${NS_BUNDLED_MODEL_FILES})

        add_library(neuroshade_neural STATIC src/neural/runtime/spatial_runtime.cpp)
        add_library(NeuroShade::neural ALIAS neuroshade_neural)
        set_target_properties(neuroshade_neural PROPERTIES POSITION_INDEPENDENT_CODE ON)
        add_dependencies(neuroshade_neural neuroshade_models)
        target_compile_features(neuroshade_neural PUBLIC cxx_std_20)
        target_include_directories(neuroshade_neural PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_include_directories(neuroshade_neural SYSTEM PRIVATE
            "${NS_MIGRAPHX_ROOT}/include" "/opt/rocm/include")
        target_compile_definitions(neuroshade_neural PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(neuroshade_neural PRIVATE
            NeuroShade::common migraphx::c migraphx::onnx migraphx::gpu amdhip64)
        target_link_directories(neuroshade_neural PRIVATE /opt/rocm/lib)

        add_executable(ns-neural-test tools/ns-neural-test/main.cpp)
        add_dependencies(ns-neural-test neuroshade_models)
        target_compile_features(ns-neural-test PRIVATE cxx_std_20)
        target_include_directories(ns-neural-test SYSTEM PRIVATE "/opt/rocm/include")
        target_compile_definitions(ns-neural-test PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(ns-neural-test PRIVATE NeuroShade::neural amdhip64)
        target_link_directories(ns-neural-test PRIVATE /opt/rocm/lib)
        if(NS_MIGRAPHX_ROOT)
            set(NS_MIGRAPHX_LIBRARY_PATH
                "${NS_MIGRAPHX_ROOT}/lib:${NS_MIGRAPHX_ROOT}/lib/migraphx/lib")
            set_target_properties(ns-neural-test PROPERTIES
                BUILD_RPATH "${NS_MIGRAPHX_ROOT}/lib;${NS_MIGRAPHX_ROOT}/lib/migraphx/lib;/opt/rocm/lib"
                INSTALL_RPATH "/opt/rocm/lib;/opt/rocm/lib/migraphx/lib")
        else()
            set(NS_MIGRAPHX_LIBRARY_PATH "/opt/rocm/lib:/opt/rocm/lib/migraphx/lib")
        endif()
        set_property(TARGET ns-neural-test PROPERTY
            INSTALL_RPATH "/opt/rocm/lib;/opt/rocm/lib/migraphx/lib")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(neuroshade_neural PRIVATE
                -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Wno-attributes)
            target_compile_options(ns-neural-test PRIVATE
                -Wall -Wextra -Wpedantic -Wno-attributes)
            if(NS_WARNINGS_AS_ERRORS)
                target_compile_options(neuroshade_neural PRIVATE -Werror)
                target_compile_options(ns-neural-test PRIVATE -Werror)
            endif()
        endif()
        install(TARGETS neuroshade_neural ns-neural-test
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY "${NS_BUNDLED_MODEL_DIR}"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/models)

        # ---- M6: temporal runtime, bundled temporal model, profiler ----
        set(NS_BUNDLED_TEMPORAL_MODEL_DIR
            "${CMAKE_BINARY_DIR}/share/neuroshade/models/temporal_blend.nsmodel")
        set(NS_BUNDLED_TEMPORAL_MODEL_FILES
            "${NS_BUNDLED_TEMPORAL_MODEL_DIR}/manifest.json"
            "${NS_BUNDLED_TEMPORAL_MODEL_DIR}/model.onnx"
            "${NS_BUNDLED_TEMPORAL_MODEL_DIR}/signature.json"
            "${NS_BUNDLED_TEMPORAL_MODEL_DIR}/metadata.json"
            "${NS_BUNDLED_TEMPORAL_MODEL_DIR}/preview.webp")
        add_custom_command(
            OUTPUT ${NS_BUNDLED_TEMPORAL_MODEL_FILES}
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/generate_temporal_model.py"
                    "${NS_BUNDLED_TEMPORAL_MODEL_DIR}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/generate_temporal_model.py"
            VERBATIM)
        add_custom_target(neuroshade_temporal_model ALL
            DEPENDS ${NS_BUNDLED_TEMPORAL_MODEL_FILES})

        # Profiler lives in the temporal library because it depends on
        # HIP events for the GPU-backed timing path; future consumers can
        # promote it into neuroshade_common if a non-HIP build site needs
        # the CPU-only fallback.
        add_library(neuroshade_temporal STATIC
            src/neural/temporal/ring_buffer.cpp
            src/neural/temporal/temporal_runtime.cpp
            src/telemetry/profiler.cpp)
        add_library(NeuroShade::temporal ALIAS neuroshade_temporal)
        set_target_properties(neuroshade_temporal PROPERTIES POSITION_INDEPENDENT_CODE ON)
        add_dependencies(neuroshade_temporal neuroshade_temporal_model)
        target_compile_features(neuroshade_temporal PUBLIC cxx_std_20)
        target_include_directories(neuroshade_temporal PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_include_directories(neuroshade_temporal SYSTEM PRIVATE
            "${NS_MIGRAPHX_ROOT}/include" "/opt/rocm/include")
        target_compile_definitions(neuroshade_temporal PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(neuroshade_temporal PRIVATE
            NeuroShade::common migraphx::c migraphx::onnx migraphx::gpu amdhip64)
        target_link_directories(neuroshade_temporal PRIVATE /opt/rocm/lib)

        add_executable(ns-temporal-test tools/ns-temporal-test/main.cpp)
        add_dependencies(ns-temporal-test neuroshade_temporal_model)
        target_compile_features(ns-temporal-test PRIVATE cxx_std_20)
        target_include_directories(ns-temporal-test SYSTEM PRIVATE "/opt/rocm/include")
        target_compile_definitions(ns-temporal-test PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(ns-temporal-test PRIVATE
            NeuroShade::temporal NeuroShade::common amdhip64)
        target_link_directories(ns-temporal-test PRIVATE /opt/rocm/lib)
        set_target_properties(ns-temporal-test PROPERTIES
            BUILD_RPATH "${NS_MIGRAPHX_LIBRARY_PATH};/opt/rocm/lib"
            INSTALL_RPATH "/opt/rocm/lib;/opt/rocm/lib/migraphx/lib")

        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(neuroshade_temporal PRIVATE
                -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Wno-attributes)
            target_compile_options(ns-temporal-test PRIVATE
                -Wall -Wextra -Wpedantic -Wno-attributes)
            if(NS_WARNINGS_AS_ERRORS)
                target_compile_options(neuroshade_temporal PRIVATE -Werror)
                target_compile_options(ns-temporal-test PRIVATE -Werror)
            endif()
        endif()

        install(TARGETS neuroshade_temporal ns-temporal-test
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY "${NS_BUNDLED_TEMPORAL_MODEL_DIR}"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/models)

        # ---- M7: .pth importer, reference model, adapter ----
        set(NS_IMPORTER_PYTHON_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/importer/python")
        set(NS_REFERENCE_ADAPTER
            "${CMAKE_CURRENT_SOURCE_DIR}/importer/adapters/spatial_gain.py")
        set(NS_REFERENCE_PTH "${CMAKE_BINARY_DIR}/share/neuroshade/importer/reference_spatial_gain.pth")
        add_custom_command(
            OUTPUT "${NS_REFERENCE_PTH}"
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/build_reference_model.py"
                    "${NS_REFERENCE_PTH}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/build_reference_model.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/adapters/spatial_gain.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/__init__.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/adapter.py"
            VERBATIM)
        add_custom_target(neuroshade_reference_pth ALL
            DEPENDS "${NS_REFERENCE_PTH}")

        set(NS_REFERENCE_MODEL_DIR
            "${CMAKE_BINARY_DIR}/share/neuroshade/importer/reference_spatial_gain.nsmodel")
        add_custom_command(
            OUTPUT "${NS_REFERENCE_MODEL_DIR}/manifest.json"
            COMMAND "${CMAKE_COMMAND}" -E env
                    "NEUROSHADE_NO_INSTALL=1"
                    "SOURCE_DATE_EPOCH=1700000000"
                    "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/importer.py" import
                    "${NS_REFERENCE_PTH}"
                    --adapter "${CMAKE_CURRENT_SOURCE_DIR}/importer/adapters/spatial_gain.py"
                    --output "${NS_REFERENCE_MODEL_DIR}"
            DEPENDS
                "${NS_REFERENCE_PTH}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/importer.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/__init__.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/adapter.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/cli.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/core.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/package.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/preflight.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/importer/python/neuroshade_importer/verifier.py"
            VERBATIM)
        add_custom_target(neuroshade_reference_model ALL
            DEPENDS "${NS_REFERENCE_MODEL_DIR}/manifest.json")
        add_dependencies(neuroshade_reference_model neuroshade_reference_pth)
        # ---- end M7 ----

        # ---- M8: motion + 2x temporal-SR bundled model + test ----
        set(NS_BUNDLED_TEMPORAL_SR_MODEL_DIR
            "${CMAKE_BINARY_DIR}/share/neuroshade/models/temporal_sr_2x.nsmodel")
        set(NS_BUNDLED_TEMPORAL_SR_MODEL_FILES
            "${NS_BUNDLED_TEMPORAL_SR_MODEL_DIR}/manifest.json"
            "${NS_BUNDLED_TEMPORAL_SR_MODEL_DIR}/model.onnx"
            "${NS_BUNDLED_TEMPORAL_SR_MODEL_DIR}/signature.json"
            "${NS_BUNDLED_TEMPORAL_SR_MODEL_DIR}/metadata.json"
            "${NS_BUNDLED_TEMPORAL_SR_MODEL_DIR}/preview.webp")
        add_custom_command(
            OUTPUT ${NS_BUNDLED_TEMPORAL_SR_MODEL_FILES}
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/generate_temporal_sr_model.py"
                    "${NS_BUNDLED_TEMPORAL_SR_MODEL_DIR}"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/nsmodel/generate_temporal_sr_model.py"
            VERBATIM)
        add_custom_target(neuroshade_temporal_sr_model ALL
            DEPENDS ${NS_BUNDLED_TEMPORAL_SR_MODEL_FILES})
        add_dependencies(neuroshade_temporal_sr_model neuroshade_temporal_model)

        add_executable(ns-temporal-sr-test tools/ns-temporal-sr-test/main.cpp)
        add_dependencies(ns-temporal-sr-test neuroshade_temporal_sr_model)
        target_compile_features(ns-temporal-sr-test PRIVATE cxx_std_20)
        target_include_directories(ns-temporal-sr-test SYSTEM PRIVATE "/opt/rocm/include")
        target_compile_definitions(ns-temporal-sr-test PRIVATE __HIP_PLATFORM_AMD__=1)
        target_link_libraries(ns-temporal-sr-test PRIVATE
            NeuroShade::temporal NeuroShade::common amdhip64)
        target_link_directories(ns-temporal-sr-test PRIVATE /opt/rocm/lib)
        set_target_properties(ns-temporal-sr-test PROPERTIES
            BUILD_RPATH "${NS_MIGRAPHX_LIBRARY_PATH};/opt/rocm/lib"
            INSTALL_RPATH "/opt/rocm/lib;/opt/rocm/lib/migraphx/lib")

        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(ns-temporal-sr-test PRIVATE
                -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Wno-attributes)
            if(NS_WARNINGS_AS_ERRORS)
                target_compile_options(ns-temporal-sr-test PRIVATE -Werror)
            endif()
        endif()

        install(TARGETS ns-temporal-sr-test
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY "${NS_BUNDLED_TEMPORAL_SR_MODEL_DIR}"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/models)
        install(FILES src/profile/examples/temporal_sr_2x.json
            DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/profiles/examples)
        # ---- end M8 ----
        # ---- end M6 ----
    else()
        message(STATUS "MIGraphX not found; M5 runtime target is disabled")
    endif()
endif()

if(NS_BUILD_HIP_INTEROP)
    find_package(Vulkan 1.2 REQUIRED)
    include(CheckLanguage)
    if(NOT DEFINED CMAKE_HIP_COMPILER AND EXISTS "/opt/rocm/lib/llvm/bin/clang++")
        set(CMAKE_HIP_COMPILER "/opt/rocm/lib/llvm/bin/clang++" CACHE FILEPATH "HIP compiler")
    endif()
    set(CMAKE_HIP_ARCHITECTURES "${NS_HIP_ARCHITECTURES}" CACHE STRING "HIP architectures" FORCE)
    check_language(HIP)
    if(NOT CMAKE_HIP_COMPILER)
        message(FATAL_ERROR "NS_BUILD_HIP_INTEROP requires a ROCm HIP compiler")
    endif()
    enable_language(HIP)

    add_executable(ns-interop-test
        tools/ns-interop-test/main.cpp
        src/vulkan/interop_buffers/interop_pool.cpp
        src/hip/interop/interop_runtime.cpp
        src/hip/kernels/color_effect.hip)
    target_compile_features(ns-interop-test PRIVATE cxx_std_20)
    target_include_directories(ns-interop-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(ns-interop-test SYSTEM PRIVATE "/opt/rocm/include")
    target_compile_definitions(ns-interop-test PRIVATE __HIP_PLATFORM_AMD__=1)
    target_link_libraries(ns-interop-test PRIVATE Vulkan::Vulkan amdhip64)
    target_link_directories(ns-interop-test PRIVATE /opt/rocm/lib)
    set_target_properties(ns-interop-test PROPERTIES
        HIP_ARCHITECTURES "${NS_HIP_ARCHITECTURES}"
        LINKER_LANGUAGE HIP)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(ns-interop-test PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wno-missing-field-initializers;-Wno-attributes>
            $<$<COMPILE_LANGUAGE:HIP>:-Wall;-Wextra;-Wpedantic>)
        if(NS_WARNINGS_AS_ERRORS)
            target_compile_options(ns-interop-test PRIVATE -Werror)
        endif()
    endif()
    install(TARGETS ns-interop-test RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(NS_BUILD_TESTBED)
    find_package(Vulkan 1.2 REQUIRED)
    if(NOT Vulkan_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc is required to build bundled SPIR-V shaders")
    endif()

    set(NS_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/share/neuroshade/shaders")
    file(MAKE_DIRECTORY "${NS_SHADER_OUTPUT_DIR}")
set(NS_BUNDLED_SHADERS copy sharpen color_adjust overlay)
    set(NS_SHADER_BINARIES)
    foreach(shader IN LISTS NS_BUNDLED_SHADERS)
        set(shader_source "${CMAKE_CURRENT_SOURCE_DIR}/plugins/bundled/${shader}/${shader}.comp")
        set(shader_binary "${NS_SHADER_OUTPUT_DIR}/${shader}.spv")
        add_custom_command(
            OUTPUT "${shader_binary}"
            COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -O
                    "${shader_source}" -o "${shader_binary}"
            DEPENDS "${shader_source}"
            VERBATIM)
        list(APPEND NS_SHADER_BINARIES "${shader_binary}")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/bundled/${shader}/manifest.toml")
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/neuroshade/plugins/${shader}")
            configure_file(
                "${CMAKE_CURRENT_SOURCE_DIR}/plugins/bundled/${shader}/manifest.toml"
                "${CMAKE_BINARY_DIR}/share/neuroshade/plugins/${shader}/manifest.toml"
                COPYONLY)
        endif()
    endforeach()
    add_custom_target(neuroshade_shaders ALL DEPENDS ${NS_SHADER_BINARIES})

    add_executable(ns-testbed
        tools/ns-testbed/main.cpp
        src/vulkan/presentation/shader_executor.cpp)
    add_dependencies(ns-testbed neuroshade_shaders)
    target_compile_features(ns-testbed PRIVATE cxx_std_20)
    target_link_libraries(ns-testbed PRIVATE NeuroShade::common Vulkan::Vulkan)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Vulkan structures are intentionally aggregate-initialized with sType;
        # remaining members are zeroed and populated explicitly below.
        target_compile_options(ns-testbed PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
        if(NS_WARNINGS_AS_ERRORS)
            target_compile_options(ns-testbed PRIVATE -Werror)
        endif()
    endif()

    install(TARGETS ns-testbed RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${NS_SHADER_BINARIES} DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/shaders)
    install(DIRECTORY plugins/bundled/ DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/plugins
        FILES_MATCHING PATTERN "manifest.toml")
endif()

if(NS_BUILD_LAYER)
    find_package(Vulkan 1.2 REQUIRED)

    add_library(neuroshade_layer SHARED
        src/layer/entrypoints/layer.cpp
        src/layer/presentation/present_processor.cpp
        src/overlay/home_key_input.cpp
        src/overlay/runtime_ui.cpp
        src/neural/runtime/host_client.cpp)
    if(TARGET neuroshade_shaders)
        add_dependencies(neuroshade_layer neuroshade_shaders)
    endif()
    target_compile_features(neuroshade_layer PRIVATE cxx_std_20)
    target_include_directories(neuroshade_layer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(neuroshade_layer PRIVATE NeuroShade::common Vulkan::Headers)
    if(TARGET neuroshade_neural)
        target_compile_definitions(neuroshade_layer PRIVATE NEUROSHADE_LAYER_NEURAL=1)
        target_link_libraries(neuroshade_layer PRIVATE NeuroShade::neural)
        target_link_directories(neuroshade_layer PRIVATE /opt/rocm/lib)
        set_property(TARGET neuroshade_layer APPEND PROPERTY BUILD_RPATH
            "${NS_MIGRAPHX_LIBRARY_PATH};/opt/rocm/lib")
        set_property(TARGET neuroshade_layer APPEND PROPERTY INSTALL_RPATH
            "/opt/rocm/lib;/opt/rocm/lib/migraphx/lib")
    endif()
    if(TARGET neuroshade_temporal)
        target_compile_definitions(neuroshade_layer PRIVATE NEUROSHADE_LAYER_TEMPORAL=1)
        target_link_libraries(neuroshade_layer PRIVATE NeuroShade::temporal)
    endif()
    find_package(X11 QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(XCB_HOTKEY QUIET IMPORTED_TARGET xcb xcb-keysyms)
    endif()
    if(XCB_HOTKEY_FOUND)
        target_compile_definitions(neuroshade_layer PRIVATE NEUROSHADE_HAVE_XCB_HOTKEY=1)
        target_link_libraries(neuroshade_layer PRIVATE PkgConfig::XCB_HOTKEY)
    endif()
    set_target_properties(neuroshade_layer PROPERTIES
        OUTPUT_NAME VkLayer_neuroshade
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/neuroshade"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(neuroshade_layer PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
        # Layer entry points intentionally share Vulkan loader symbol names.
        # Bind references inside the layer to this DSO to prevent ELF symbol
        # preemption from redirecting them back into libvulkan and recursing.
        target_link_options(neuroshade_layer PRIVATE -Wl,-Bsymbolic-functions)
        if(NS_WARNINGS_AS_ERRORS)
            target_compile_options(neuroshade_layer PRIVATE -Werror)
        endif()
    endif()

    set(NS_LAYER_MANIFEST_DIR "${CMAKE_BINARY_DIR}/share/vulkan/explicit_layer.d")
    file(MAKE_DIRECTORY "${NS_LAYER_MANIFEST_DIR}")
    configure_file(
        packaging/vulkan-layer/VkLayer_neuroshade.json.in
        "${NS_LAYER_MANIFEST_DIR}/VkLayer_neuroshade.json"
        @ONLY
    )
    configure_file(
        tools/neuroshade-run/neuroshade-run.in
        "${CMAKE_BINARY_DIR}/bin/neuroshade-run"
        @ONLY
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
    configure_file(
        tests/integration/wrapper_test.cmake.in
        "${CMAKE_BINARY_DIR}/wrapper_test.cmake"
        @ONLY
    )
    configure_file(
        tests/vulkan/shader_pipeline_test.cmake.in
        "${CMAKE_BINARY_DIR}/shader_pipeline_test.cmake"
        @ONLY
    )

    install(TARGETS neuroshade_layer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/neuroshade)
    install(FILES "${NS_LAYER_MANIFEST_DIR}/VkLayer_neuroshade.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/explicit_layer.d)
    install(PROGRAMS "${CMAKE_BINARY_DIR}/bin/neuroshade-run" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(BUILD_TESTING)
    if(X11_FOUND AND TARGET X11::Xtst)
        add_executable(ns-send-home tests/vulkan/send_home.cpp)
        target_compile_features(ns-send-home PRIVATE cxx_std_20)
        target_link_libraries(ns-send-home PRIVATE X11::X11 X11::Xtst)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(ns-send-home PRIVATE -Wall -Wextra -Wpedantic)
            if(NS_WARNINGS_AS_ERRORS)
                target_compile_options(ns-send-home PRIVATE -Werror)
            endif()
        endif()
    endif()

    add_executable(ns-common-tests tests/unit/log_tests.cpp)
    target_compile_features(ns-common-tests PRIVATE cxx_std_20)
    target_link_libraries(ns-common-tests PRIVATE NeuroShade::common)
    add_test(NAME unit.common COMMAND ns-common-tests)

    add_executable(ns-framegraph-tests tests/unit/framegraph_tests.cpp)
    target_compile_features(ns-framegraph-tests PRIVATE cxx_std_20)
    target_link_libraries(ns-framegraph-tests PRIVATE NeuroShade::common)
    add_test(NAME unit.framegraph COMMAND ns-framegraph-tests)

    add_executable(ns-pipeline-plan-tests tests/unit/pipeline_plan_tests.cpp)
    target_compile_features(ns-pipeline-plan-tests PRIVATE cxx_std_20)
    target_link_libraries(ns-pipeline-plan-tests PRIVATE NeuroShade::common)
    if(TARGET neuroshade_temporal_sr_model)
        add_dependencies(ns-pipeline-plan-tests neuroshade_temporal_sr_model)
        target_compile_definitions(ns-pipeline-plan-tests PRIVATE NS_TEST_HAS_TEMPORAL_MODEL=1)
    endif()
    add_test(NAME unit.pipeline_plan COMMAND ns-pipeline-plan-tests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/m2-profile.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/profile/examples/temporal_sr_2x.json"
        "${CMAKE_BINARY_DIR}/share/neuroshade/plugins")
    set_tests_properties(unit.pipeline_plan PROPERTIES
        PASS_REGULAR_EXPRESSION "pipeline_plan=pass")

    add_executable(ns-resource-tests tests/unit/resource_tests.cpp)
    target_compile_features(ns-resource-tests PRIVATE cxx_std_20)
    target_link_libraries(ns-resource-tests PRIVATE NeuroShade::common Vulkan::Headers)
    add_test(NAME unit.resources COMMAND ns-resource-tests)

    add_executable(ns-profile-tests tests/unit/profile_tests.cpp)
    target_compile_features(ns-profile-tests PRIVATE cxx_std_20)
    target_link_libraries(ns-profile-tests PRIVATE NeuroShade::common)
    add_test(NAME profile.schema_v2_roundtrip
        COMMAND ns-profile-tests
            "${CMAKE_CURRENT_SOURCE_DIR}/src/profile/examples/temporal_sr_2x.json"
            "${CMAKE_BINARY_DIR}/profile-schema-v2-roundtrip.json")
    set_tests_properties(profile.schema_v2_roundtrip PROPERTIES
        PASS_REGULAR_EXPRESSION "schema_v2_roundtrip=pass")

    add_executable(ns-host-client-tests tests/unit/host_client_tests.cpp src/neural/runtime/host_client.cpp)
    target_link_libraries(ns-host-client-tests PRIVATE neuroshade_common)
    add_test(NAME unit.host_client COMMAND ns-host-client-tests)
    set_tests_properties(unit.host_client PROPERTIES TIMEOUT 10)
    add_executable(ns-runtime-ui-tests tests/unit/runtime_ui_tests.cpp src/overlay/runtime_ui.cpp)
    target_link_libraries(ns-runtime-ui-tests PRIVATE NeuroShade::common)
    add_test(NAME runtime-ui COMMAND ns-runtime-ui-tests)

    add_executable(ns-overlay-tests tests/unit/overlay_tests.cpp)
    target_compile_features(ns-overlay-tests PRIVATE cxx_std_20)
    target_link_libraries(ns-overlay-tests PRIVATE NeuroShade::common)
    add_test(NAME unit.overlay_state COMMAND ns-overlay-tests)
    set_tests_properties(unit.overlay_state PROPERTIES
        PASS_REGULAR_EXPRESSION "overlay_state=pass")

    configure_file(tests/integration/product_tools_test.cmake.in
        "${CMAKE_BINARY_DIR}/product_tools_test.cmake" @ONLY)
    add_test(NAME product.tools
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/product_tools_test.cmake")
    set_tests_properties(product.tools PROPERTIES TIMEOUT 60)

    configure_file(tests/integration/portable_package_test.cmake.in
        "${CMAKE_BINARY_DIR}/portable_package_test.cmake" @ONLY)
    add_test(NAME product.portable_package
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/portable_package_test.cmake")
    set_tests_properties(product.portable_package PROPERTIES TIMEOUT 120)

    if(NS_BUILD_LAYER)
        configure_file(tests/proton/harness_test.cmake.in
            "${CMAKE_BINARY_DIR}/proton_harness_test.cmake" @ONLY)
        add_test(NAME proton.harness_contract
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/proton_harness_test.cmake")
        set_tests_properties(proton.harness_contract PROPERTIES TIMEOUT 30)

        configure_file(tests/proton/contained_runner_test.cmake.in
            "${CMAKE_BINARY_DIR}/contained_runner_test.cmake" @ONLY)
        add_test(NAME proton.contained_runner
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/contained_runner_test.cmake")
        set_tests_properties(proton.contained_runner PROPERTIES
            TIMEOUT 30 SKIP_REGULAR_EXPRESSION "proton.contained_runner: SKIP")

        configure_file(tests/integration/session_log_test.cmake.in
            "${CMAKE_BINARY_DIR}/session_log_test.cmake" @ONLY)
        add_test(NAME product.session_log
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/session_log_test.cmake")
        set_tests_properties(product.session_log PROPERTIES TIMEOUT 30)
    endif()

    if(NS_BUILD_HIP_INTEROP)
        add_test(NAME hip.interop_zero_copy COMMAND ns-interop-test)
        set_tests_properties(hip.interop_zero_copy PROPERTIES
            TIMEOUT 60
            PASS_REGULAR_EXPRESSION "Interop: ZERO-COPY BUFFER;steady_state_cpu_frame_readback=no")
        add_test(NAME hip.interop_host_staging
            COMMAND ${CMAKE_COMMAND} -E env NEUROSHADE_FORCE_HOST_STAGING=1
                    $<TARGET_FILE:ns-interop-test>)
        set_tests_properties(hip.interop_host_staging PROPERTIES
            TIMEOUT 60
            PASS_REGULAR_EXPRESSION "Interop: HOST-STAGING FALLBACK;steady_state_cpu_frame_readback=yes")
        add_test(NAME hip.interop_self_test_fallback
            COMMAND ${CMAKE_COMMAND} -E env NEUROSHADE_TEST_FAIL_ZERO_COPY=1
                    $<TARGET_FILE:ns-interop-test>)
        set_tests_properties(hip.interop_self_test_fallback PROPERTIES
            TIMEOUT 60
            PASS_REGULAR_EXPRESSION "zero-copy self-test failed;Interop: HOST-STAGING FALLBACK")
    endif()

    if(NS_MIGRAPHX_AVAILABLE)
        configure_file(tests/integration/soak_harness_test.cmake.in
            "${CMAKE_BINARY_DIR}/soak_harness_test.cmake" @ONLY)
        add_test(NAME reliability.soak_harness
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/soak_harness_test.cmake")
        set_tests_properties(reliability.soak_harness PROPERTIES TIMEOUT 60)

        configure_file(tests/neural/spatial_runtime_test.cmake.in
            "${CMAKE_BINARY_DIR}/spatial_runtime_test.cmake" @ONLY)
        add_test(NAME neural.spatial_runtime
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/spatial_runtime_test.cmake")
        set_tests_properties(neural.spatial_runtime PROPERTIES TIMEOUT 120)

        configure_file(tests/neural/temporal_runtime_test.cmake.in
            "${CMAKE_BINARY_DIR}/temporal_runtime_test.cmake" @ONLY)
        add_test(NAME neural.temporal_runtime
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/temporal_runtime_test.cmake")
        set_tests_properties(neural.temporal_runtime PROPERTIES TIMEOUT 180)

        configure_file(tests/neural/temporal_sr_runtime_test.cmake.in
            "${CMAKE_BINARY_DIR}/temporal_sr_runtime_test.cmake" @ONLY)
        add_test(NAME neural.temporal_sr_2x
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/temporal_sr_runtime_test.cmake")
        set_tests_properties(neural.temporal_sr_2x PROPERTIES TIMEOUT 180)

        # M7 importer tests — drive tools/nsmodel/importer.py against the
        # reference .pth produced by neuroshade_reference_pth.
        set(NS_IMPORTER_TEST_ROOT "${CMAKE_BINARY_DIR}/importer-tests")
        file(REMOVE_RECURSE "${NS_IMPORTER_TEST_ROOT}")

        configure_file(tests/importer/adapter_protocol_test.cmake.in
            "${CMAKE_BINARY_DIR}/adapter_protocol_test.cmake" @ONLY)
        add_test(NAME importer.adapter_protocol
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/adapter_protocol_test.cmake")
        set_tests_properties(importer.adapter_protocol PROPERTIES TIMEOUT 60)

        configure_file(tests/importer/importer_roundtrip_test.cmake.in
            "${CMAKE_BINARY_DIR}/importer_roundtrip_test.cmake" @ONLY)
        add_test(NAME importer.roundtrip
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/importer_roundtrip_test.cmake")
        set_tests_properties(importer.roundtrip PROPERTIES TIMEOUT 120)

        configure_file(tests/importer/bad_architecture_test.cmake.in
            "${CMAKE_BINARY_DIR}/bad_architecture_test.cmake" @ONLY)
        add_test(NAME importer.bad_architecture
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/bad_architecture_test.cmake")
        set_tests_properties(importer.bad_architecture PROPERTIES TIMEOUT 60)

        configure_file(tests/importer/unsupported_op_test.cmake.in
            "${CMAKE_BINARY_DIR}/unsupported_op_test.cmake" @ONLY)
        add_test(NAME importer.unsupported_op
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/unsupported_op_test.cmake")
        set_tests_properties(importer.unsupported_op PROPERTIES TIMEOUT 60)

        # Once built, the reference .nsmodel is also installable.
        install(DIRECTORY "${NS_REFERENCE_MODEL_DIR}"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/models OPTIONAL)
    endif()

    if(NS_BUILD_TESTBED)
        add_test(NAME vulkan.deterministic_testbed COMMAND ns-testbed --frames 4)
        set_tests_properties(vulkan.deterministic_testbed PROPERTIES TIMEOUT 30)
        add_test(NAME vulkan.shader_pipeline
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/shader_pipeline_test.cmake")
        set_tests_properties(vulkan.shader_pipeline PROPERTIES TIMEOUT 30)

        if(NS_BUILD_LAYER)
            add_test(NAME vulkan.layer_passthrough
                COMMAND ${CMAKE_COMMAND} -E env
                    VK_LAYER_PATH=${NS_LAYER_MANIFEST_DIR}
                    VK_INSTANCE_LAYERS=VK_LAYER_NEUROSHADE
                    $<TARGET_FILE:ns-testbed> --frames 4)
            set_tests_properties(vulkan.layer_passthrough PROPERTIES
                TIMEOUT 30
                PASS_REGULAR_EXPRESSION "checksum=0x3d23155e380d6325")

            add_test(NAME launcher.wrapper_contract
                COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/wrapper_test.cmake")
            set_tests_properties(launcher.wrapper_contract PROPERTIES TIMEOUT 10)

            find_file(NS_VALIDATION_LAYER_MANIFEST
                NAMES VkLayer_khronos_validation.json
                PATHS
                    /usr/share/vulkan/explicit_layer.d
                    /usr/local/share/vulkan/explicit_layer.d
            )
            if(NOT NS_VALIDATION_LAYER_MANIFEST AND NS_VALIDATION_LAYER_LIBRARY)
                if(NOT EXISTS "${NS_VALIDATION_LAYER_LIBRARY}")
                    message(FATAL_ERROR "NS_VALIDATION_LAYER_LIBRARY does not exist: ${NS_VALIDATION_LAYER_LIBRARY}")
                endif()
                set(NS_LOCAL_VALIDATION_DIR "${CMAKE_BINARY_DIR}/validation-layer")
                file(MAKE_DIRECTORY "${NS_LOCAL_VALIDATION_DIR}")
                configure_file(
                    tests/vulkan/VkLayer_khronos_validation.json.in
                    "${NS_LOCAL_VALIDATION_DIR}/VkLayer_khronos_validation.json"
                    @ONLY)
                set(NS_VALIDATION_LAYER_MANIFEST
                    "${NS_LOCAL_VALIDATION_DIR}/VkLayer_khronos_validation.json")
            endif()
            if(NS_VALIDATION_LAYER_MANIFEST)
                get_filename_component(NS_VALIDATION_LAYER_DIR "${NS_VALIDATION_LAYER_MANIFEST}" DIRECTORY)
                add_test(NAME vulkan.layer_validation
                    COMMAND ${CMAKE_COMMAND} -E env
                        VK_LAYER_PATH=${NS_LAYER_MANIFEST_DIR}:${NS_VALIDATION_LAYER_DIR}
                        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation:VK_LAYER_NEUROSHADE
                        $<TARGET_FILE:ns-testbed> --frames 4)
                set_tests_properties(vulkan.layer_validation PROPERTIES
                    TIMEOUT 30
                    PASS_REGULAR_EXPRESSION "checksum=0x3d23155e380d6325"
                    FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")

                add_test(NAME vulkan.shader_validation
                    COMMAND ${CMAKE_COMMAND} -E env
                        VK_LAYER_PATH=${NS_VALIDATION_LAYER_DIR}
                        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
                        NEUROSHADE_TEST_EFFECTS=sharpen,color_adjust
                        $<TARGET_FILE:ns-testbed> --frames 4)
                set_tests_properties(vulkan.shader_validation PROPERTIES
                    TIMEOUT 30
                    PASS_REGULAR_EXPRESSION "effects=2"
                    FAIL_REGULAR_EXPRESSION "Validation Error;VUID-")
            endif()

            if(TARGET ns-send-home)
                configure_file(tests/vulkan/present_processing_test.sh.in
                    "${CMAKE_BINARY_DIR}/present_processing_test.sh" @ONLY
                    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
                add_test(NAME vulkan.present_processing
                    COMMAND "${CMAKE_BINARY_DIR}/present_processing_test.sh")
                set_tests_properties(vulkan.present_processing PROPERTIES
                    TIMEOUT 30 SKIP_RETURN_CODE 77
                    PASS_REGULAR_EXPRESSION "present_processing=pass")
            else()
                message(STATUS "XTest not found: skipping interactive Home-key present test")
            endif()
            if(TARGET neuroshade_neural)
                configure_file(tests/vulkan/neural_present_test.sh.in
                    "${CMAKE_BINARY_DIR}/neural_present_test.sh" @ONLY
                    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
                add_test(NAME vulkan.neural_present
                    COMMAND "${CMAKE_BINARY_DIR}/neural_present_test.sh")
                set_tests_properties(vulkan.neural_present PROPERTIES
                    TIMEOUT 45 SKIP_RETURN_CODE 77
                    PASS_REGULAR_EXPRESSION "neural_present=pass")
            endif()
            if(TARGET neuroshade_temporal)
                configure_file(tests/vulkan/temporal_present_test.sh.in
                    "${CMAKE_BINARY_DIR}/temporal_present_test.sh" @ONLY
                    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
                add_test(NAME vulkan.temporal_present
                    COMMAND "${CMAKE_BINARY_DIR}/temporal_present_test.sh")
                set_tests_properties(vulkan.temporal_present PROPERTIES
                    TIMEOUT 45 SKIP_RETURN_CODE 77
                    PASS_REGULAR_EXPRESSION "temporal_present=pass")
            endif()
        endif()
    endif()
endif()

install(TARGETS neuroshade_common
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS ns-package-inspect RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/neuroshade)
if(GTK4_FOUND)
    install(TARGETS neuroshade-desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES src/logging/log.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/neuroshade/logging)

install(PROGRAMS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/neuroshade"
    DESTINATION ${CMAKE_INSTALL_BINDIR})
if(NOT GTK4_FOUND)
    install(PROGRAMS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/neuroshade-desktop"
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES "${CMAKE_BINARY_DIR}/org.neuroshade.NeuroShade.desktop"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(PROGRAMS tools/neuroshade-run/neuroshade-setup
    DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY images/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/neuroshade/images)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/neuroshade/docs)
install(PROGRAMS packaging/linux/install.sh packaging/linux/uninstall.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/packaging)
install(PROGRAMS tests/proton/qualify-proton.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification)
install(PROGRAMS tests/proton/run-contained-wine.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification)
install(PROGRAMS tests/proton/run-bottles-smoke.sh tests/proton/build-windows-smokes.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification)
install(DIRECTORY tests/proton/windows/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification/windows)
install(FILES tests/proton/README.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification
    RENAME README-Proton.md)
install(PROGRAMS tests/soak/run-soak.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification)
install(PROGRAMS tests/soak/run-m9-soaks.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification)
install(PROGRAMS tests/soak/verify-soak-results.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/qualification)
install(PROGRAMS tools/nsmodel/importer.py
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/neuroshade
    RENAME nsmodel-importer.py)
install(DIRECTORY importer/python/neuroshade_importer
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/neuroshade/python
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE)
install(DIRECTORY importer/adapters/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/neuroshade/importer/adapters
    FILES_MATCHING PATTERN "*.py"
    PATTERN "__pycache__" EXCLUDE)
install(FILES README.md COMPATIBILITY.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/neuroshade)
install(SCRIPT "${CMAKE_BINARY_DIR}/record_plugin_hashes.cmake")

add_custom_target(neuroshade-portable
    COMMAND /bin/sh "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/build-portable.sh"
            "${CMAKE_BINARY_DIR}" "${CMAKE_BINARY_DIR}/packages"
    COMMENT "Building reproducible NeuroShade portable tarball"
    VERBATIM)
