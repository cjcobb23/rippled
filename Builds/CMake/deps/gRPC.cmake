
# currently linking to unsecure versions...if we switch, we'll
# need to add ssl as a link dependency to the grpc targets
option (use_secure_grpc "use TLS version of grpc libs." OFF)
if (use_secure_grpc)
  set (grpc_suffix "")
else ()
  set (grpc_suffix "_unsecure")
endif ()

project(ripple_libs)
FetchContent_Declare(
  gRPC
  GIT_REPOSITORY https://github.com/grpc/grpc
  GIT_TAG        v1.27.3
  )
set(FETCHCONTENT_QUIET OFF)
FetchContent_MakeAvailable(gRPC)

target_link_libraries(ripple_libs INTERFACE grpc++)

#[=================================[
   generate protobuf sources for
   grpc defs and bundle into a
   static lib
#]=================================]
set (GRPC_GEN_DIR "${CMAKE_BINARY_DIR}/proto_gen_grpc")
file (MAKE_DIRECTORY ${GRPC_GEN_DIR})
set (GRPC_PROTO_SRCS)
set (GRPC_PROTO_HDRS)
set (GRPC_PROTO_ROOT "${CMAKE_SOURCE_DIR}/src/ripple/proto/org")
file(GLOB_RECURSE GRPC_DEFINITION_FILES LIST_DIRECTORIES false "${GRPC_PROTO_ROOT}/*.proto")
foreach(file ${GRPC_DEFINITION_FILES})
  get_filename_component(_abs_file ${file} ABSOLUTE)
  get_filename_component(_abs_dir ${_abs_file} DIRECTORY)
  get_filename_component(_basename ${file} NAME_WE)
  get_filename_component(_proto_inc ${GRPC_PROTO_ROOT} DIRECTORY) # updir one level
  file(RELATIVE_PATH _rel_root_file ${_proto_inc} ${_abs_file})
  get_filename_component(_rel_root_dir ${_rel_root_file} DIRECTORY)
  file(RELATIVE_PATH _rel_dir ${CMAKE_CURRENT_SOURCE_DIR} ${_abs_dir})

  set (src_1 "${GRPC_GEN_DIR}/${_rel_root_dir}/${_basename}.grpc.pb.cc")
  set (src_2 "${GRPC_GEN_DIR}/${_rel_root_dir}/${_basename}.pb.cc")
  set (hdr_1 "${GRPC_GEN_DIR}/${_rel_root_dir}/${_basename}.grpc.pb.h")
  set (hdr_2 "${GRPC_GEN_DIR}/${_rel_root_dir}/${_basename}.pb.h")
  add_custom_command(
    OUTPUT ${src_1} ${src_2} ${hdr_1} ${hdr_2}
    COMMAND protobuf::protoc
    ARGS --grpc_out=${GRPC_GEN_DIR}
         --cpp_out=${GRPC_GEN_DIR}
         --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc++>
         -I ${_proto_inc} -I ${_rel_dir}
         ${_abs_file}
    DEPENDS ${_abs_file} protobuf::protoc grpc++
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running gRPC C++ protocol buffer compiler on ${file}"
    VERBATIM)
    set_source_files_properties(${src_1} ${src_2} ${hdr_1} ${hdr_2} PROPERTIES GENERATED TRUE)
    list(APPEND GRPC_PROTO_SRCS ${src_1} ${src_2})
    list(APPEND GRPC_PROTO_HDRS ${hdr_1} ${hdr_2})
endforeach()

add_library (grpc_pbufs STATIC ${GRPC_PROTO_SRCS} ${GRPC_PROTO_HDRS})
#target_include_directories (grpc_pbufs PRIVATE src)
target_include_directories (grpc_pbufs SYSTEM PUBLIC ${GRPC_GEN_DIR})
target_link_libraries (grpc_pbufs protobuf::libprotobuf grpc++)
target_compile_options (grpc_pbufs
  PRIVATE
    $<$<BOOL:${MSVC}>:-wd4065>
    $<$<NOT:$<BOOL:${MSVC}>>:-Wno-deprecated-declarations>
  PUBLIC
    $<$<BOOL:${MSVC}>:-wd4996>
    $<$<BOOL:${is_xcode}>:
      --system-header-prefix="google/protobuf"
      -Wno-deprecated-dynamic-exception-spec
    >)
add_library (Ripple::grpc_pbufs ALIAS grpc_pbufs)
target_link_libraries (ripple_libs INTERFACE Ripple::grpc_pbufs)
exclude_if_included (grpc_pbufs)
