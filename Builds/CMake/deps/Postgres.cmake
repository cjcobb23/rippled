
find_library(postgres NAMES libpq-dev pq-dev postgresql-devel)
message(postgres)
message(${postgres})
if(NOT postgres)

    add_library(postgres SHARED IMPORTED GLOBAL)
    ExternalProject_Add(postgres_src
        PREFIX ${nih_cache_path}
        GIT_REPOSITORY https://github.com/postgres/postgres.git
        GIT_TAG master
        CONFIGURE_COMMAND ./configure --without-readline
        BUILD_COMMAND $(CMAKE_COMMAND) -E env --unset=MAKELEVEL make
        UPDATE_COMMAND ""
        BUILD_IN_SOURCE 1
        INSTALL_COMMAND ""
        )


    message(${CMAKE_MAKE_PROGRAM})

    message(${nih_cache_path})

    ExternalProject_Get_Property (postgres_src SOURCE_DIR)
    ExternalProject_Get_Property (postgres_src BINARY_DIR)
    set (postgres_src_SOURCE_DIR "${SOURCE_DIR}")
    file (MAKE_DIRECTORY ${postgres_src_SOURCE_DIR})

    message("BINARY DIR:")
    message(${BINARY_DIR})
    message(${ep_lib_prefix})
    message(${ep_lib_suffix})
    message(${BINARY_DIR}/${ep_lib_prefix}pq.so)
    message("SOURCE DIR: ")
    message(${SOURCE_DIR})
    list(APPEND INCLUDE_DIRS ${SOURCE_DIR}/src/include)
    list(APPEND INCLUDE_DIRS ${SOURCE_DIR}/src/interfaces/libpq)


    set_target_properties (postgres PROPERTIES
        IMPORTED_LOCATION
          ${BINARY_DIR}/src/interfaces/libpq/${ep_lib_prefix}pq.so
        INTERFACE_INCLUDE_DIRECTORIES
          "${INCLUDE_DIRS}")
    add_dependencies(postgres postgres_src)
    message("postgres_src_SOURCE_DIR:")
    message(${postgres_src_SOURCE_DIR})

    file(TO_CMAKE_PATH "${postgres_src_SOURCE_DIR}" postgres_src_SOURCE_DIR)
    message(${postgres_src_SOURCE_DIR})

    target_link_libraries(ripple_libs INTERFACE postgres)
endif()

