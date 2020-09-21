
message("**************************************** HEY")
#if (reporting)
#  find_library(cassandra NAMES cassandra cassandra-cpp-driver REQUIRED)
#  find_path(cassandra_includes NAMES cassandra.h REQUIRED)
#endif()


find_library(cassandra NAMES cassandra REQUIRED)
message(${cassandra})
if(NOT cassandra)
    message("NOT")
    find_library(libuv1 NAMES uv1 libuv1 LibUV libuv1:amd64 REQUIRED)

    message(${libuv1})

    if(NOT libuv1)
        add_library(libuv1 SHARED IMPORTED GLOBAL)
        ExternalProject_Add(libuv_src
            PREFIX ${nih_cache_path}
            GIT_REPOSITORY https://github.com/libuv/libuv.git
            GIT_TAG v1.x
            INSTALL_COMMAND ""
            )
        message("NOTUV")

        ExternalProject_Get_Property (libuv_src SOURCE_DIR)
        ExternalProject_Get_Property (libuv_src BINARY_DIR)
        set (libuv_src_SOURCE_DIR "${SOURCE_DIR}")
        file (MAKE_DIRECTORY ${libuv_src_SOURCE_DIR}/include)
        message(${BINARY_DIR})
        message(${ep_lib_prefix})
        message(${ep_lib_suffix})

        set_target_properties (libuv1 PROPERTIES
            IMPORTED_LOCATION
              ${BINARY_DIR}/${ep_lib_prefix}uv.so.1
            INTERFACE_INCLUDE_DIRECTORIES
              ${SOURCE_DIR}/include)
        add_dependencies(libuv1 libuv_src)

        file(TO_CMAKE_PATH "${libuv_src_SOURCE_DIR}" libuv_src_SOURCE_DIR)
        target_include_directories (libuv1 INTERFACE ${libuv_src_SOURCE_DIR}/include)
    endif()

    add_library (cassandra SHARED IMPORTED GLOBAL)
    message(${BINARY_DIR})
    ExternalProject_Add(cassandra_src
        PREFIX ${nih_cache_path}
        GIT_REPOSITORY https://github.com/datastax/cpp-driver.git
        GIT_TAG master
        CMAKE_ARGS
          -DLIBUV_ROOT_DIR=${BINARY_DIR}
          -DLIBUV_LIBARY=${BINARY_DIR}/libuv.so.1.0.0
          -DLIBUV_INCLUDE_DIR=${SOURCE_DIR}/include
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
    ExternalProject_Add_StepDependencies(cassandra_src build libuv1)

    file(TO_CMAKE_PATH "${cassandra_src_SOURCE_DIR}" cassandra_src_SOURCE_DIR)
    target_include_directories (cassandra INTERFACE ${cassandra_src_SOURCE_DIR}/include)
    target_link_libraries(cassandra INTERFACE libuv1)
    target_link_libraries(ripple_libs INTERFACE cassandra)
else()

  target_link_libraries (ripple_libs INTERFACE ${cassandra})
endif()


exclude_if_included (cassandra)
