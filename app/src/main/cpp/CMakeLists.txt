# CMakeLists.txt for OMT Bridge in Open Camera
# This compiles the JNI bridge that connects Java to our C++ OMT libraries
#
# ABI POLICY (changed 2026-10-15): the codec sources are selected per ANDROID_ABI. Until now this
# file hard-wired the arm64 binding (vmxcodec_arm.cpp + -DARM64) for every ABI, which is what made
# an x86_64 APK impossible -- the x86/AVX2 sources were already in libvmx/src and are what the
# desktop host QA (qa/build-host.sh) compiles and runs. See the two branches below.
#
# Adding an ABI here is deliberately an error rather than a silent fallback: an ABI that quietly
# compiled the wrong SIMD backend would produce black frames, not a red build.

cmake_minimum_required(VERSION 3.22.1)
project("opencamera_omt")

# =============================================================================
# OMT (Open Media Transport) Libraries
# =============================================================================

# Build libomt from source (our custom C++ implementation with quality support)
# Same structure as omtandroid project
get_filename_component(OMT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(LIBOMT_DIR ${OMT_REPO_ROOT}/libomt)
set(LIBVMX_DIR ${OMT_REPO_ROOT}/libvmx)

# -----------------------------------------------------------------------------
# Per-ABI codec binding.
#
# vmxcodec.h derives ARM64 / X64 / AVX2 from the compiler's own predefined macros
# (__aarch64__ / __x86_64__), so the macro set a TU sees is a function of the ABI, not of these
# flags. That is why -DARM64 is passed on arm64 only: passing it under x86_64 would make the x86
# sources read the ARM headers (vmxcodec_arm.h pulls in sse2neon.h) and would give libomt a
# different VMX_INSTANCE layout than the libvmx it links against.
if(ANDROID_ABI STREQUAL "arm64-v8a")
  # NEON through sse2neon.h. -DARM64 is what the shipping APK has always been built with; the
  # comment predating this change was right that omitting it yields black frames.
  set(OMT_VMX_SOURCES
      ${LIBVMX_DIR}/src/vmxcodec.cpp
      ${LIBVMX_DIR}/src/vmxcodec_arm.cpp)
  # -DARM64 as a flag, not target_compile_definitions: it is passed inside
  # target_compile_options alongside the other flags, and a bare `ARM64` there is a filename to
  # clang, not a define.
  set(OMT_ARCH_DEFINES -DARM64)
  set(OMT_BASELINE_FLAGS)
elseif(ANDROID_ABI STREQUAL "x86_64")
  set(OMT_VMX_SOURCES
      ${LIBVMX_DIR}/src/vmxcodec.cpp
      ${LIBVMX_DIR}/src/vmxcodec_x86.cpp
      ${LIBVMX_DIR}/src/vmxcodec_avx2.cpp)
  # No -DARM64. X64 and AVX2 come from vmxcodec.h via __x86_64__.
  set(OMT_ARCH_DEFINES)

  # ISA FLOOR -- deliberately the minimum the source needs, not the desktop's -march=haswell.
  # qa/build-host.sh compiles the host copy with -march=haswell (AVX2+FMA+BMI2). That is fine for a
  # host binary and unacceptable in a library that loads on an emulator image or a CI runner of
  # unknown vintage, because the compiler would then emit those encodings into the SSE backend too.
  #
  # vmxcodec_x86.cpp uses _mm_blend_epi16 / _mm_cvtepu8_epi32 / _mm_packus_epi32 / _mm_testz_si128 /
  # _mm_extract_epi32 -- all SSE4.1. That is the real floor: without it the file does not compile,
  # and nothing above it is required. x86-64-v2 (SSE4.2+POPCNT) would also work, but asking for
  # more than the source needs buys nothing and narrows the set of CPUs that can load the library,
  # so the floor stays at SSE4.1. No -march= override anywhere on this ABI.
  set(OMT_BASELINE_FLAGS -msse4.1)

  # vmxcodec_avx2.cpp is the ONLY file compiled with AVX2, exactly as its own header comment asks
  # ("it should be the only one compiled with the AVX2 flag, with all others with SSE4.2"). It is
  # reached only through the runtime gate in VMX_Create: instance->avx2 is set when
  # __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2"), and every *256 entry point is
  # dispatched behind `if (instance->avx2)` in vmxcodec_x86.cpp. -mavx2 alone does NOT turn on
  # LZCNT/BMI in clang, and this file needs both (_tzcnt_u64, _bextr_u64, __lzcnt64), so they are
  # named explicitly — safe precisely because the gate that admits this TU already requires an
  # AVX2+BMI2 CPU, which has both. The AVX2 encodings stay confined to this TU.
  set_source_files_properties(${LIBVMX_DIR}/src/vmxcodec_avx2.cpp
      PROPERTIES COMPILE_OPTIONS "-mavx2;-mlzcnt;-mbmi")
else()
  message(FATAL_ERROR
      "Unsupported ANDROID_ABI '${ANDROID_ABI}'. This file knows how to bind the codec for "
      "arm64-v8a (NEON/sse2neon) and x86_64 (SSE4.1 baseline + runtime-gated AVX2) only. "
      "Add an explicit branch -- a silent fallback here would ship a codec compiled against the "
      "wrong SIMD backend, which shows up as black frames, not as a build error.")
endif()

# Build VMX codec from source with the SIMD backend this ABI actually has.
add_library(vmx SHARED
    ${OMT_VMX_SOURCES}
)

target_include_directories(vmx PRIVATE
    ${LIBVMX_DIR}/src
)

# CRITICAL FLAGS for VMX:
# -DARM64 (arm64 only): Enables NEON SIMD optimizations (without this = black frames!)
# -DANDROID: Android-specific code paths
# -fms-extensions -fdeclspec: Required for __declspec(align) in vmxcodec.h
target_compile_options(vmx PRIVATE
    -O3
    -fPIC
    ${OMT_BASELINE_FLAGS}
    ${OMT_ARCH_DEFINES}
    -DANDROID
    -fms-extensions
    -fdeclspec
    -Wno-macro-redefined
)

target_link_libraries(vmx
    log
)

# Build libomt from source (modular C++ implementation)
# Ported from libomtnet with proper separation of concerns
add_library(omt SHARED
    ${LIBOMT_DIR}/cpp/libomt_android.cpp
    ${LIBOMT_DIR}/cpp/OMTAsyncPool.cpp
    ${LIBOMT_DIR}/cpp/OMTChannel.cpp
    ${LIBOMT_DIR}/cpp/OMTSender.cpp
    ${LIBOMT_DIR}/cpp/OMTDiscovery.cpp
)

# Include directories for libomt compilation
target_include_directories(omt PRIVATE
    ${LIBOMT_DIR}/cpp
    ${LIBOMT_DIR}
    ${LIBVMX_DIR}/src
)

# Compiler flags for libomt
# OMT_ARCH_DEFINES must match the vmx target: libomt includes vmxcodec.h, and if the two targets
# disagreed on ARM64/X64 they would disagree on VMX_INSTANCE's layout and read each other's members
# at the wrong offsets. libomt has no NEON of its own -- the flag was never load-bearing here.
target_compile_options(omt PRIVATE
    -O3
    -fPIC
    ${OMT_BASELINE_FLAGS}
    ${OMT_ARCH_DEFINES}
    -DANDROID
    -fms-extensions
    -fdeclspec
    -Wno-macro-redefined
    -std=c++17
)

# Link libomt against vmx and Android libs
target_link_libraries(omt
    vmx
    log
)

# =============================================================================
# OMT Bridge (JNI interface for Open Camera)
# =============================================================================

add_library(omtbridge SHARED
    OMTBridge.cpp
)

# Include directories for OMTBridge
target_include_directories(omtbridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBOMT_DIR}
)

target_compile_options(omtbridge PRIVATE
    ${OMT_BASELINE_FLAGS}
)

# Find the log library
find_library(log-lib log)

# Link all libraries together
target_link_libraries(omtbridge
    ${log-lib}
    omt
    vmx
)

# =============================================================================
# Build provenance stamp
# =============================================================================
# Each of vmx / omt / omtbridge gets its own copy of a generated translation unit carrying
# "abi=<this abi> git=<rev> dirty=<0|1> omt-src-sha256=<hash of every native input>". A prebuilt or
# cached lib copied in from another tree disagrees with its sibling, and qa/abi-provenance.sh says
# so from the shipped APK alone. See cmake/GenProvenance.cmake for the reasoning.
set(OMT_PROVENANCE_CPP ${CMAKE_BINARY_DIR}/omt_provenance.cpp)

# Globbed here as well so ninja re-runs the generator when any input's CONTENT changes;
# GenProvenance.cmake re-globs at build time for the hash itself.
file(GLOB_RECURSE OMT_PROVENANCE_INPUTS CONFIGURE_DEPENDS
     ${LIBVMX_DIR}/src/*.cpp ${LIBVMX_DIR}/src/*.h
     ${LIBOMT_DIR}/cpp/*.cpp ${LIBOMT_DIR}/cpp/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h
     ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
     ${CMAKE_CURRENT_SOURCE_DIR}/cmake/*.cmake)

add_custom_command(OUTPUT ${OMT_PROVENANCE_CPP}
    COMMAND ${CMAKE_COMMAND}
        -DOMT_REPO_ROOT=${OMT_REPO_ROOT}
        -DOMT_ABI=${ANDROID_ABI}
        -DOMT_OUT=${OMT_PROVENANCE_CPP}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenProvenance.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenProvenance.cmake ${OMT_PROVENANCE_INPUTS}
    VERBATIM)
add_custom_target(omt_provenance_generate DEPENDS ${OMT_PROVENANCE_CPP})
set_source_files_properties(${OMT_PROVENANCE_CPP} PROPERTIES GENERATED TRUE)

foreach(_omt_lib vmx omt omtbridge)
    target_sources(${_omt_lib} PRIVATE ${OMT_PROVENANCE_CPP})
    add_dependencies(${_omt_lib} omt_provenance_generate)
endforeach()
