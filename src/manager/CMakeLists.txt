cmake_minimum_required(VERSION 3.6)
project(manager)

set(SRC
	DnsCache.cc
	RouteManager.cc
	WFGlobal.cc
)

if (UPSTREAM STREQUAL "y")
	set(SRC
		${SRC}
		UpstreamManager.cc
	)
endif ()

add_library(${PROJECT_NAME} OBJECT ${SRC})
