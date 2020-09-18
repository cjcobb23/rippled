
message("**************************************** HEY")
#if (reporting)
#  find_library(cassandra NAMES cassandra cassandra-cpp-driver REQUIRED)
#  find_path(cassandra_includes NAMES cassandra.h REQUIRED)
#endif()

add_library (cassandra SHARED IMPORTED GLOBAL)
ExternalProject_Add(cassandra_src
    PREFIX ${nih_cache_path}
    GIT_REPOSITORY https://github.com/datastax/cpp-driver.git
    GIT_TAG master
    INSTALL_COMMAND ""
    )

ExternalProject_Get_Property (cassandra_src SOURCE_DIR)
ExternalProject_Get_Property (cassandra_src BINARY_DIR)
set (cassandra_src_SOURCE_DIR "${SOURCE_DIR}")
file (MAKE_DIRECTORY ${cassandra_src_SOURCE_DIR}/include)
message(${BINARY_DIR})
message(${ep_lib_prefix})
message(${ep_lib_suffix})
message(${BINARY_DIR}/${ep_lib_prefix}cassandra.so)

set_target_properties (cassandra PROPERTIES
    IMPORTED_LOCATION
      ${BINARY_DIR}/${ep_lib_prefix}cassandra.so
    INTERFACE_INCLUDE_DIRECTORIES
      ${SOURCE_DIR}/include)
add_dependencies(cassandra cassandra_src)

file(TO_CMAKE_PATH "${cassandra_src_SOURCE_DIR}" cassandra_src_SOURCE_DIR)



exclude_if_included (cassandra)
if (reporting)
  target_link_libraries (ripple_libs INTERFACE cassandra)
endif()

if (reporting)
    target_include_directories (cassandra INTERFACE ${cassandra_src_SOURCE_DIR}/include)
endif()


