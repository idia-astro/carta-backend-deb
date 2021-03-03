macro(install_uWebSockets)

    set(CMAKE_C_FLAGS "-O3 -DLIBUS_NO_SSL")

    include_directories(${CMAKE_SOURCE_DIR}/uWebSockets/src)

    set(UWEBSOCKETS_SOURCE_FILES
            ${SOURCE_FILES}
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Epoll.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Extensions.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Group.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/HTTPSocket.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Hub.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Networking.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Node.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Room.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/Socket.cpp
            ${CMAKE_SOURCE_DIR}/uWebSockets/src/WebSocket.cpp)

    add_library(uWebSockets ${UWEBSOCKETS_SOURCE_FILES})

endmacro()

