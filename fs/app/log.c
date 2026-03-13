#include "log.h"
#include <time.h>
#include <inttypes.h>

aurora_fs_log_ctx_t aurora_fs_log_ctx = {NULL, false};

aurora_fs_err_t aurora_fs_log_init(FILE *fp)
{
	if (fp == NULL)
	{
		return AURORA_FS_ERR_NULL_POINTER;
	}

	aurora_fs_log_ctx.log_file = fp;
	aurora_fs_log_ctx.init_flag = true;

	return AURORA_FS_ERR_OK;
}

aurora_fs_err_t aurora_fs_log(const char *str, bool err)
{
	time_t now = time(NULL);
	int write_count;

	if (now == (time_t)-1)
	{
		return AURORA_FS_ERR_FAILURE;
	}

	if (!aurora_fs_log_ctx.init_flag || aurora_fs_log_ctx.log_file == NULL)
	{
		return AURORA_FS_ERR_NOT_INITIALIZED;
	}

	if (str == NULL)
	{
		return AURORA_FS_ERR_NULL_POINTER;
	}

	if (err)
	{
		write_count = fprintf(aurora_fs_log_ctx.log_file, "[error | %jd] %s\n", (intmax_t)now, str);
	}

	else
	{
		write_count = fprintf(aurora_fs_log_ctx.log_file, "[log | %jd] %s\n", (intmax_t)now, str);
	}

	if (write_count < 0)
	{
		return AURORA_FS_ERR_WRITE_FAILED;
	}

	if (fflush(aurora_fs_log_ctx.log_file) != 0)
	{
		return AURORA_FS_ERR_WRITE_FAILED;
	}

	return AURORA_FS_ERR_OK;
}