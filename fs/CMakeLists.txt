set(AURORA_FS_SOURCES
	app/fs.c
	app/log.c
)

set(AURORA_FS_CLI_SOURCES
	app/main.c
)

add_library(aurora_studio-native SHARED ${AURORA_FS_SOURCES})

target_include_directories(aurora_studio-native
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/inc
)

set_target_properties(aurora_studio-native PROPERTIES
	POSITION_INDEPENDENT_CODE ON
)

add_executable(aurora-fs ${AURORA_FS_CLI_SOURCES})

target_link_libraries(aurora-fs
	PRIVATE
		aurora_studio-native
)

target_include_directories(aurora-fs
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/inc
)
