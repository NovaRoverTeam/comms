cmake_minimum_required(VERSION 2.8.3)
project(comms)

find_package(catkin REQUIRED COMPONENTS
  roscpp
  std_msgs
  mainframe
  message_generation
)

generate_messages(DEPENDENCIES std_msgs)

catkin_package(CATKIN_DEPENDS message_runtime std_msgs)

add_definitions(-std=c++11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wextra")

# Stuff for the WebSocket client/server library
set(BOOST_COMPONENTS system coroutine context thread)
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 4.9)
        set(BOOST_COMPONENTS ${BOOST_COMPONENTS} regex)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DUSE_BOOST_REGEX")
    endif()
endif()
find_package(Boost 1.54.0 COMPONENTS ${BOOST_COMPONENTS} REQUIRED)
include_directories(SYSTEM ${Boost_INCLUDE_DIR})

find_package(OpenSSL REQUIRED)
include_directories(${OPENSSL_INCLUDE_DIR})

find_package(Boost 1.54.0 COMPONENTS ${BOOST_COMPONENTS} REQUIRED)
include_directories(SYSTEM ${Boost_INCLUDE_DIR})

find_package(Threads REQUIRED)

include_directories(include ${catkin_INCLUDE_DIRS})

add_executable(comms src/comms.cpp include/c_uart_interface_example/serial_port.cpp)
add_dependencies(comms ${catkin_EXPORTED_TARGETS})
target_link_libraries(comms ${catkin_LIBRARIES})
target_link_libraries(comms ${Boost_LIBRARIES})
target_link_libraries(comms ${OPENSSL_CRYPTO_LIBRARY})
target_link_libraries(comms ${CMAKE_THREAD_LIBS_INIT})


