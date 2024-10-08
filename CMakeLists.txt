cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(namespace "lsm")
project("lsm_sampling" VERSION 0.1.0)

set(debug false)
set(tests True)
set(bench True)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

if (debug) 
    add_compile_options(-g -O0)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
else()
    add_compile_options(-O3)
endif()


add_library(${PROJECT_NAME} STATIC)

# Test build instructions
if (tests) 
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/tests")

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lsmtree")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/lsmtree/meta")
    add_executable(pagedfile_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/pagedfile_tests.cpp)
    target_link_libraries(pagedfile_tests PUBLIC ${PROJECT_NAME} check subunit pthread)
    target_compile_options(pagedfile_tests PUBLIC -llib)

    add_executable(isamtree_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/isamtree_tests.cpp)
    target_link_libraries(isamtree_tests PUBLIC ${PROJECT_NAME} check subunit pthread)
    target_compile_options(isamtree_tests PUBLIC -llib)

    add_executable(memtable_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/memtable_tests.cpp)
    target_link_libraries(memtable_tests PUBLIC ${PROJECT_NAME} check subunit pthread)
    target_compile_options(memtable_tests PUBLIC -llib)

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/memrun_tests")
    add_executable(inmemrun_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/inmemrun_tests.cpp)
    target_link_libraries(inmemrun_tests PUBLIC ${PROJECT_NAME} check subunit pthread)
    target_compile_options(inmemrun_tests PUBLIC -llib)

    add_executable(lsmtree_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/lsmtree_tests.cpp)
    target_link_libraries(lsmtree_tests PUBLIC ${PROJECT_NAME} check subunit pthread)
    target_compile_options(lsmtree_tests PUBLIC -llib)

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/memlevel_tests")
    add_executable(memlevel_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/memlevel_tests.cpp)
    target_link_libraries(memlevel_tests PUBLIC ${PROJECT_NAME} check subunit pthread)
    target_compile_options(memlevel_tests PUBLIC -llib)

endif()

# Benchmark build instructions
if (bench)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  "${CMAKE_CURRENT_SOURCE_DIR}/bin/benchmarks")

    add_executable(lsm_insert ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lsm_insert.cpp)
    target_link_libraries(lsm_insert PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(lsm_insert PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/lsm_insert")

    add_executable(lsm_bench ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lsm_bench.cpp)
    target_link_libraries(lsm_bench PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(lsm_bench PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/lsm_bench")

    add_executable(lsm_sample ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lsm_sample.cpp)
    target_link_libraries(lsm_sample PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(lsm_sample PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/lsm_sample")

    add_executable(lsm_mixed ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lsm_mixed.cpp)
    target_link_libraries(lsm_mixed PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(lsm_mixed PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/lsm_mixed")

    add_executable(lsm_sample_selectivity ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lsm_sample_selectivity.cpp)
    target_link_libraries(lsm_sample_selectivity PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(lsm_sample_selectivity PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/lsm_sample_selectivity")

    add_executable(lsm_insert_sample ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lsm_insert_sample.cpp)
    target_link_libraries(lsm_insert_sample PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(lsm_insert_sample PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/lsm_insert_sample")
    
    
	add_executable(lsm_warmup_construct ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/lsm_warmup_construct.cpp)
    target_link_libraries(lsm_warmup_construct PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(lsm_warmup_construct PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/lsm_warmup_construct")


    add_executable(btree_sample ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/btree_sample.cpp)
    target_link_libraries(btree_sample PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(btree_sample PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/btree_sample")
    
	add_executable(btree_sample_selectivity ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/btree_sample_selectivity.cpp)
    target_link_libraries(btree_sample_selectivity PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(btree_sample_selectivity PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/btree_sample_selectivity")
    
    add_executable(btree_insert ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/btree_insert.cpp)
    target_link_libraries(btree_insert PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(btree_insert PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/btree_insert")

    #add_executable(btree_mixed ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/btree_mixed.cpp)
    #target_link_libraries(btree_mixed PUBLIC ${PROJECT_NAME})
    #target_compile_options(btree_mixed PUBLIC -llib)
    #file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/btree_mixed")


    add_executable(static_sample ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/static_sample.cpp)
    target_link_libraries(static_sample PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(static_sample PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/static_sample")
    
	add_executable(static_sample_selectivity ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/static_sample_selectivity.cpp)
    target_link_libraries(static_sample_selectivity PUBLIC ${PROJECT_NAME} pthread)
    target_compile_options(static_sample_selectivity PUBLIC -llib)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/data/static_sample")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/utilities")
add_executable(data_generator ${CMAKE_CURRENT_SOURCE_DIR}/src/utilities/data_generator.cpp)

target_sources(${PROJECT_NAME} 
    PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/src/io/PagedFile.cpp
)

target_include_directories(${PROJECT_NAME} 
    PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PUBLIC gsl)

if (debug) 
    target_compile_options(${PROJECT_NAME} PUBLIC -g)
endif()



