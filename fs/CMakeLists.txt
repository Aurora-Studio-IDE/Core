set(AURORA_FS_SOURCES
	app/fs.c
	app/log.c
)

add_library(aurora_studio-native SHARED ${AURORA_FS_SOURCES})

target_include_directories(aurora_studio-native
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/inc
)

set_target_properties(aurora_studio-native PROPERTIES
	POSITION_INDEPENDENT_CODE ON
)
