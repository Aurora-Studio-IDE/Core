#ifndef AURORA_LOG_H
#define AURORA_LOG_H

#include "err.h"
#include <stdio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct aurora_fs_log_ctx
	{
		FILE *log_file;
		bool init_flag;
	} aurora_fs_log_ctx_t;

	extern aurora_fs_log_ctx_t aurora_fs_log_ctx;

	/**
	 * @brief Initialize the fs logger with an output stream.
	 * @param fp Target stream for log messages.
	 * @return aurora_fs_err_t
	 */
	aurora_fs_err_t aurora_fs_log_init(FILE *fp);

	/**
	 * @brief Write a log message.
	 * @param str Message string to write.
	 * @param err If true, message is treated as an error log.
	 * @return aurora_fs_err_t
	 */
	aurora_fs_err_t aurora_fs_log(const char *str, bool err);

#ifdef __cplusplus
}
#endif
#endif