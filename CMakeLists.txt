cmake_minimum_required(VERSION 2.8)

project(INMOST_Projects)

option(USE_MPI "Compile with MPI support" ON)

add_executable(2d_diffusion_fem 2d_diffusion_fem.cpp)
add_executable(2d_diffusion_fem_ad 2d_diffusion_fem_ad.cpp)

find_package(INMOST REQUIRED)
if(NOT INMOST_FOUND)
    message("INMOST not found!")
endif()

include_directories(${INMOST_INCLUDE_DIRS})
message(${INMOST_INCLUDE_DIRS})
message(${INMOST_LIBRARIES})
target_link_libraries(2d_diffusion_fem ${INMOST_LIBRARIES})
target_link_libraries(2d_diffusion_fem_ad ${INMOST_LIBRARIES})

if(USE_MPI)
    message("Dealing with MPI")
    find_package(MPI REQUIRED)
    if(NOT MPI_FOUND)
        message("MPI not found!")
    endif()

    message((${MPI_INCLUDE_PATH}))
    include_directories(${MPI_INCLUDE_PATH})
    target_link_libraries(2d_diffusion_fem ${MPI_CXX_LIBRARIES})
    target_link_libraries(2d_diffusion_fem_ad ${MPI_CXX_LIBRARIES})

    if(MPI_LINK_FLAGS)
        set_target_properties(2d_diffusion_fem PROPERTIES LINK_FLAGS "${MPI_LINK_FLAGS}")
        set_target_properties(2d_diffusion_fem_ad PROPERTIES LINK_FLAGS "${MPI_LINK_FLAGS}")
    endif()
endif()
