cmake_minimum_required(VERSION 3.6)
project(server)

set(SRC
	WFServer.cc
)

if (MYSQL STREQUAL "y")
	set(SRC
		${SRC}
		WFMySQLServer.cc
	)
endif ()

add_library(${PROJECT_NAME} OBJECT ${SRC})
