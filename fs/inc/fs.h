#ifndef AURORA_FS_H
#define AURORA_FS_H

#include "err.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief Run filesystem core command handler.
	 * @param argc Argument count.
	 * @param argv Argument vector. argv[1] is treated as command name.
	 * @return aurora_fs_err_t
	 */
	aurora_fs_err_t aurora_fs_core_run(int argc, const char *argv[]);

	/**
	 * @brief Optional C main-like entry for embedding.
	 * @param argc Argument count.
	 * @param argv Argument vector.
	 * @return int Returns 0 on success, non-zero on error.
	 */
	int aurora_fs_core_main(int argc, char *argv[]);

	/**
	 * @brief Print supported command usage.
	 * @param out Output stream.
	 */
	void aurora_fs_print_usage(FILE *out);

	/**
	 * @brief Convert error code to text.
	 * @param err Error code.
	 * @return const char* Human-readable error message.
	 */
	const char *aurora_fs_err_to_string(aurora_fs_err_t err);

	/**
	 * @brief Release internal resources owned by fs core.
	 */
	void aurora_fs_core_cleanup(void);

#ifdef __cplusplus
}
#endif
#endif