cmake_minimum_required(VERSION 3.6)
project(factory)

set(SRC
	WFGraphTask.cc
	DnsTaskImpl.cc
	WFTaskFactory.cc
	Workflow.cc
	HttpTaskImpl.cc
)

if (MYSQL STREQUAL "y")
	set(SRC
		${SRC}
		MySQLTaskImpl.cc
	)
endif ()

if (REDIS STREQUAL "y")
	set(SRC
		${SRC}
		RedisTaskImpl.cc
	)
endif ()

add_library(${PROJECT_NAME} OBJECT ${SRC})

if (KAFKA STREQUAL "y")
	set(SRC
		KafkaTaskImpl.cc
	)
	add_library("factory_kafka" OBJECT ${SRC})
	set_property(SOURCE KafkaTaskImpl.cc APPEND PROPERTY COMPILE_OPTIONS "-fno-rtti")
endif ()
