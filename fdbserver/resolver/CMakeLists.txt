fdb_find_sources(FDBSERVER_RESOLVER_SRCS)

add_flow_target(STATIC_LIBRARY NAME fdbserver_resolver SRCS ${FDBSERVER_RESOLVER_SRCS})
add_fdbserver_link_test(fdbserver_resolverlinktest
  fdbserver_resolver
  fdbserver_logsystem
  fdbserver_core)
add_fdbserver_unit_test(fdbserver_resolver_test resolver
  fdbserver_resolver
  fdbserver_logsystem
  fdbserver_core)

configure_fdbserver_common_includes(fdbserver_resolver)
target_include_directories(fdbserver_resolver
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fdbserver_resolver PRIVATE fdbserver_core fdbserver_logsystem)

# Tasks that btree-v2's checkReads interleaves while it checks a batch's reads.
set(BTREE_V2_TASKS 16 CACHE STRING "Task capacity of btree-v2's checkReads (1 to 255)")
target_compile_definitions(fdbserver_resolver PRIVATE BTREE_V2_TASKS=${BTREE_V2_TASKS})
