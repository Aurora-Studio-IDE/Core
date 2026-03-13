#include "fs.h"
#include "log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define AURORA_FS_HISTORY_CAPACITY 64U

typedef enum aurora_fs_state_kind
{
	AURORA_FS_STATE_NONE = 0,
	AURORA_FS_STATE_FILE,
	AURORA_FS_STATE_DIR,
} aurora_fs_state_kind_t;

typedef struct aurora_fs_state_snapshot
{
	aurora_fs_state_kind_t kind;
	char *data;
	size_t data_size;
} aurora_fs_state_snapshot_t;

typedef struct aurora_fs_history_entry
{
	char *path;
	aurora_fs_state_snapshot_t before;
	aurora_fs_state_snapshot_t after;
} aurora_fs_history_entry_t;

static aurora_fs_history_entry_t aurora_fs_history[AURORA_FS_HISTORY_CAPACITY];
static size_t aurora_fs_history_start = 0;
static size_t aurora_fs_history_count = 0;
static size_t aurora_fs_history_cursor = 0;

static aurora_fs_err_t aurora_fs_map_errno(void);
static aurora_fs_err_t aurora_fs_cmd_write_like(int argc, const char *argv[], const char *mode);

#ifdef _WIN32
typedef struct _stat aurora_fs_stat_t;
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#else
typedef struct stat aurora_fs_stat_t;
#endif

static int aurora_fs_stat_path(const char *path, aurora_fs_stat_t *st)
{
#ifdef _WIN32
	return _stat(path, st);
#else
	return stat(path, st);
#endif
}

static int aurora_fs_mkdir_path(const char *path)
{
#ifdef _WIN32
	return _mkdir(path);
#else
	return mkdir(path, 0775);
#endif
}

static int aurora_fs_rmdir_path(const char *path)
{
#ifdef _WIN32
	return _rmdir(path);
#else
	return rmdir(path);
#endif
}

#ifdef _WIN32
static aurora_fs_err_t aurora_fs_map_win32_error(DWORD err)
{
	switch (err)
	{
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND:
			return AURORA_FS_ERR_NOT_FOUND;
		case ERROR_ACCESS_DENIED:
			return AURORA_FS_ERR_PERMISSION_DENIED;
		case ERROR_ALREADY_EXISTS:
		case ERROR_FILE_EXISTS:
			return AURORA_FS_ERR_ALREADY_EXISTS;
		default:
			return AURORA_FS_ERR_IO;
	}
}
#endif

/**
 * @brief Snapshot algorithm notes for undo/redo path state capture.
 * @details Captures path state as NONE/FILE/DIR, and stores full file bytes for FILE state.
 * @param path Target path.
 * @param snapshot Output snapshot container.
 * @return aurora_fs_err_t
 * @note Time complexity is O(n) for a regular file of size n, O(1) for missing path or directory.
 */

static int aurora_fs_cmd_eq(const char *lhs, const char *rhs)
{
	if (lhs == NULL || rhs == NULL)
	{
		return 0;
	}

	return strcmp(lhs, rhs) == 0;
}

static char *aurora_fs_strdup(const char *src)
{
	char *dup;
	size_t size;

	if (src == NULL)
	{
		return NULL;
	}

	size = strlen(src) + 1;
	dup = (char *)malloc(size);
	if (dup == NULL)
	{
		return NULL;
	}

	(void)memcpy(dup, src, size);
	return dup;
}

static void aurora_fs_state_reset(aurora_fs_state_snapshot_t *snapshot)
{
	if (snapshot == NULL)
	{
		return;
	}

	snapshot->kind = AURORA_FS_STATE_NONE;
	snapshot->data = NULL;
	snapshot->data_size = 0;
}

static void aurora_fs_state_free(aurora_fs_state_snapshot_t *snapshot)
{
	if (snapshot == NULL)
	{
		return;
	}

	free(snapshot->data);
	aurora_fs_state_reset(snapshot);
}

static void aurora_fs_history_entry_free(aurora_fs_history_entry_t *entry)
{
	if (entry == NULL)
	{
		return;
	}

	free(entry->path);
	entry->path = NULL;
	aurora_fs_state_free(&entry->before);
	aurora_fs_state_free(&entry->after);
}

static size_t aurora_fs_history_index(size_t logical_pos)
{
	return (aurora_fs_history_start + logical_pos) % AURORA_FS_HISTORY_CAPACITY;
}

static aurora_fs_history_entry_t *aurora_fs_history_entry_at(size_t logical_pos)
{
	return &aurora_fs_history[aurora_fs_history_index(logical_pos)];
}

static aurora_fs_err_t aurora_fs_capture_file_state(const char *path, aurora_fs_state_snapshot_t *snapshot)
{
	aurora_fs_stat_t st;
	FILE *fp;
	char *buffer;
	size_t read_size;

	if (path == NULL || snapshot == NULL)
	{
		return AURORA_FS_ERR_NULL_POINTER;
	}

	aurora_fs_state_reset(snapshot);

	if (aurora_fs_stat_path(path, &st) != 0)
	{
		if (errno == ENOENT)
		{
			return AURORA_FS_ERR_OK;
		}
		return aurora_fs_map_errno();
	}

	if (S_ISDIR(st.st_mode))
	{
		snapshot->kind = AURORA_FS_STATE_DIR;
		return AURORA_FS_ERR_OK;
	}

	if (!S_ISREG(st.st_mode))
	{
		return AURORA_FS_ERR_NOT_SUPPORTED;
	}

	fp = fopen(path, "rb");
	if (fp == NULL)
	{
		return AURORA_FS_ERR_OPEN_FAILED;
	}

	if (st.st_size < 0)
	{
		(void)fclose(fp);
		return AURORA_FS_ERR_OUT_OF_RANGE;
	}

	buffer = NULL;
	if (st.st_size > 0)
	{
		buffer = (char *)malloc((size_t)st.st_size);
		if (buffer == NULL)
		{
			(void)fclose(fp);
			return AURORA_FS_ERR_NO_MEMORY;
		}

		read_size = fread(buffer, 1, (size_t)st.st_size, fp);
		if (read_size != (size_t)st.st_size)
		{
			free(buffer);
			(void)fclose(fp);
			return AURORA_FS_ERR_READ_FAILED;
		}
	}

	if (fclose(fp) != 0)
	{
		free(buffer);
		return AURORA_FS_ERR_CLOSE_FAILED;
	}

	snapshot->kind = AURORA_FS_STATE_FILE;
	snapshot->data = buffer;
	snapshot->data_size = (size_t)st.st_size;
	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_apply_state(const char *path, const aurora_fs_state_snapshot_t *snapshot)
{
	aurora_fs_stat_t st;
	FILE *fp;

	if (path == NULL || snapshot == NULL)
	{
		return AURORA_FS_ERR_NULL_POINTER;
	}

	if (snapshot->kind == AURORA_FS_STATE_NONE)
	{
		if (aurora_fs_stat_path(path, &st) != 0)
		{
			if (errno == ENOENT)
			{
				return AURORA_FS_ERR_OK;
			}
			return aurora_fs_map_errno();
		}

		if (S_ISDIR(st.st_mode))
		{
			if (aurora_fs_rmdir_path(path) != 0)
			{
				return aurora_fs_map_errno();
			}
			return AURORA_FS_ERR_OK;
		}

		if (remove(path) != 0)
		{
			return aurora_fs_map_errno();
		}

		return AURORA_FS_ERR_OK;
	}

	if (snapshot->kind == AURORA_FS_STATE_DIR)
	{
		if (aurora_fs_stat_path(path, &st) == 0)
		{
			if (S_ISDIR(st.st_mode))
			{
				return AURORA_FS_ERR_OK;
			}
			return AURORA_FS_ERR_NOT_DIRECTORY;
		}

		if (errno != ENOENT)
		{
			return aurora_fs_map_errno();
		}

		if (aurora_fs_mkdir_path(path) != 0)
		{
			return aurora_fs_map_errno();
		}

		return AURORA_FS_ERR_OK;
	}

	if (snapshot->kind != AURORA_FS_STATE_FILE)
	{
		return AURORA_FS_ERR_NOT_SUPPORTED;
	}

	fp = fopen(path, "wb");
	if (fp == NULL)
	{
		return AURORA_FS_ERR_OPEN_FAILED;
	}

	if (snapshot->data_size > 0)
	{
		if (fwrite(snapshot->data, 1, snapshot->data_size, fp) != snapshot->data_size)
		{
			(void)fclose(fp);
			return AURORA_FS_ERR_WRITE_FAILED;
		}
	}

	if (fclose(fp) != 0)
	{
		return AURORA_FS_ERR_CLOSE_FAILED;
	}

	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_history_push(const char *path, aurora_fs_state_snapshot_t *before,
											  aurora_fs_state_snapshot_t *after)
{
	aurora_fs_history_entry_t *entry;

	if (path == NULL || before == NULL || after == NULL)
	{
		return AURORA_FS_ERR_NULL_POINTER;
	}

	while (aurora_fs_history_count > aurora_fs_history_cursor)
	{
		aurora_fs_history_count -= 1;
		aurora_fs_history_entry_free(aurora_fs_history_entry_at(aurora_fs_history_count));
	}

	if (aurora_fs_history_count == AURORA_FS_HISTORY_CAPACITY)
	{
		aurora_fs_history_entry_free(aurora_fs_history_entry_at(0));
		aurora_fs_history_start = (aurora_fs_history_start + 1) % AURORA_FS_HISTORY_CAPACITY;
		aurora_fs_history_count -= 1;
		if (aurora_fs_history_cursor > 0)
		{
			aurora_fs_history_cursor -= 1;
		}
	}

	entry = aurora_fs_history_entry_at(aurora_fs_history_count);
	entry->path = aurora_fs_strdup(path);
	if (entry->path == NULL)
	{
		return AURORA_FS_ERR_NO_MEMORY;
	}

	entry->before = *before;
	entry->after = *after;
	aurora_fs_state_reset(before);
	aurora_fs_state_reset(after);

	aurora_fs_history_count += 1;
	aurora_fs_history_cursor = aurora_fs_history_count;
	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_cmd_undo(void)
{
	aurora_fs_history_entry_t *entry;

	if (aurora_fs_history_cursor == 0)
	{
		return AURORA_FS_ERR_OUT_OF_RANGE;
	}

	entry = aurora_fs_history_entry_at(aurora_fs_history_cursor - 1);
	if (entry->path == NULL)
	{
		return AURORA_FS_ERR_FAILURE;
	}

	{
		aurora_fs_err_t err = aurora_fs_apply_state(entry->path, &entry->before);
		if (err != AURORA_FS_ERR_OK)
		{
			return err;
		}
	}

	aurora_fs_history_cursor -= 1;
	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_cmd_redo(void)
{
	aurora_fs_history_entry_t *entry;

	if (aurora_fs_history_cursor >= aurora_fs_history_count)
	{
		return AURORA_FS_ERR_OUT_OF_RANGE;
	}

	entry = aurora_fs_history_entry_at(aurora_fs_history_cursor);
	if (entry->path == NULL)
	{
		return AURORA_FS_ERR_FAILURE;
	}

	{
		aurora_fs_err_t err = aurora_fs_apply_state(entry->path, &entry->after);
		if (err != AURORA_FS_ERR_OK)
		{
			return err;
		}
	}

	aurora_fs_history_cursor += 1;
	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_execute_with_history(int argc, const char *argv[], aurora_fs_err_t (*op)(int argc, const char *argv[]))
{
	aurora_fs_state_snapshot_t before;
	aurora_fs_state_snapshot_t after;
	aurora_fs_err_t err;

	if (argc < 3 || argv == NULL || argv[2] == NULL || op == NULL)
	{
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	aurora_fs_state_reset(&before);
	aurora_fs_state_reset(&after);

	err = aurora_fs_capture_file_state(argv[2], &before);
	if (err != AURORA_FS_ERR_OK)
	{
		return err;
	}

	err = op(argc, argv);
	if (err != AURORA_FS_ERR_OK)
	{
		aurora_fs_state_free(&before);
		return err;
	}

	err = aurora_fs_capture_file_state(argv[2], &after);
	if (err != AURORA_FS_ERR_OK)
	{
		aurora_fs_state_free(&before);
		return err;
	}

	err = aurora_fs_history_push(argv[2], &before, &after);
	aurora_fs_state_free(&before);
	aurora_fs_state_free(&after);
	return err;
}

void aurora_fs_core_cleanup(void)
{
	size_t index;

	for (index = 0; index < aurora_fs_history_count; index++)
	{
		aurora_fs_history_entry_free(aurora_fs_history_entry_at(index));
	}

	aurora_fs_history_start = 0;
	aurora_fs_history_count = 0;
	aurora_fs_history_cursor = 0;
}

static aurora_fs_err_t aurora_fs_cmd_write(int argc, const char *argv[])
{
	return aurora_fs_cmd_write_like(argc, argv, "wb");
}

static aurora_fs_err_t aurora_fs_cmd_append(int argc, const char *argv[])
{
	return aurora_fs_cmd_write_like(argc, argv, "ab");
}

static aurora_fs_err_t aurora_fs_map_errno(void)
{
	switch (errno)
	{
		case ENOENT:
			return AURORA_FS_ERR_NOT_FOUND;
		case EACCES:
			return AURORA_FS_ERR_PERMISSION_DENIED;
		case EEXIST:
			return AURORA_FS_ERR_ALREADY_EXISTS;
		case ENOTDIR:
			return AURORA_FS_ERR_NOT_DIRECTORY;
		case EISDIR:
			return AURORA_FS_ERR_IS_DIRECTORY;
		case ENAMETOOLONG:
			return AURORA_FS_ERR_PATH_TOO_LONG;
		default:
			return AURORA_FS_ERR_IO;
	}
}

const char *aurora_fs_err_to_string(aurora_fs_err_t err)
{
	switch (err)
	{
		case AURORA_FS_ERR_OK:
			return "ok";
		case AURORA_FS_ERR_FAILURE:
			return "failure";
		case AURORA_FS_ERR_INVALID_ARGUMENT:
			return "invalid argument";
		case AURORA_FS_ERR_NULL_POINTER:
			return "null pointer";
		case AURORA_FS_ERR_OUT_OF_RANGE:
			return "out of range";
		case AURORA_FS_ERR_NOT_INITIALIZED:
			return "not initialized";
		case AURORA_FS_ERR_ALREADY_INITIALIZED:
			return "already initialized";
		case AURORA_FS_ERR_NOT_SUPPORTED:
			return "not supported";
		case AURORA_FS_ERR_NO_MEMORY:
			return "no memory";
		case AURORA_FS_ERR_OVERFLOW:
			return "overflow";
		case AURORA_FS_ERR_UNDERFLOW:
			return "underflow";
		case AURORA_FS_ERR_BUFFER_TOO_SMALL:
			return "buffer too small";
		case AURORA_FS_ERR_TIMEOUT:
			return "timeout";
		case AURORA_FS_ERR_CANCELLED:
			return "cancelled";
		case AURORA_FS_ERR_BUSY:
			return "busy";
		case AURORA_FS_ERR_IO:
			return "i/o error";
		case AURORA_FS_ERR_NOT_FOUND:
			return "not found";
		case AURORA_FS_ERR_ALREADY_EXISTS:
			return "already exists";
		case AURORA_FS_ERR_PERMISSION_DENIED:
			return "permission denied";
		case AURORA_FS_ERR_PATH_TOO_LONG:
			return "path too long";
		case AURORA_FS_ERR_INVALID_PATH:
			return "invalid path";
		case AURORA_FS_ERR_IS_DIRECTORY:
			return "is directory";
		case AURORA_FS_ERR_NOT_DIRECTORY:
			return "not directory";
		case AURORA_FS_ERR_READ_ONLY:
			return "read only";
		case AURORA_FS_ERR_BROKEN_PIPE:
			return "broken pipe";
		case AURORA_FS_ERR_OPEN_FAILED:
			return "open failed";
		case AURORA_FS_ERR_READ_FAILED:
			return "read failed";
		case AURORA_FS_ERR_WRITE_FAILED:
			return "write failed";
		case AURORA_FS_ERR_SEEK_FAILED:
			return "seek failed";
		case AURORA_FS_ERR_CLOSE_FAILED:
			return "close failed";
		case AURORA_FS_ERR_SYNC_FAILED:
			return "sync failed";
		case AURORA_FS_ERR_UNKNOWN:
			return "unknown";
		default:
			return "unrecognized error";
	}
}

void aurora_fs_print_usage(FILE *out)
{
	FILE *stream = out == NULL ? stdout : out;

	(void)fprintf(stream, "aurora-fs <command> [args]\n"
						  "commands:\n"
						  "  help\n"
						  "  list [path]\n"
						  "  exists <path>\n"
						  "  read <path>\n"
						  "  write <path> <content>\n"
						  "  append <path> <content>\n"
						  "  delete <path>\n"
						  "  mkdir <path>\n"
						  "  rmdir <path>\n"
						  "  undo\n"
						  "  redo\n");
}

static aurora_fs_err_t aurora_fs_cmd_list(int argc, const char *argv[])
{
	const char *path;

#ifdef _WIN32
	char pattern[MAX_PATH];
	size_t path_len;
	WIN32_FIND_DATAA find_data;
	HANDLE find_handle;

	path = argc >= 3 && argv[2] != NULL ? argv[2] : ".";
	path_len = strlen(path);

	if (path_len + 3 >= sizeof(pattern))
	{
		return AURORA_FS_ERR_PATH_TOO_LONG;
	}

	(void)memcpy(pattern, path, path_len + 1);
	if (path_len > 0 && pattern[path_len - 1] != '\\' && pattern[path_len - 1] != '/')
	{
		pattern[path_len] = '\\';
		pattern[path_len + 1] = '*';
		pattern[path_len + 2] = '\0';
	}
	else
	{
		pattern[path_len] = '*';
		pattern[path_len + 1] = '\0';
	}

	find_handle = FindFirstFileA(pattern, &find_data);
	if (find_handle == INVALID_HANDLE_VALUE)
	{
		return aurora_fs_map_win32_error(GetLastError());
	}

	do
	{
		if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0)
		{
			continue;
		}
		(void)puts(find_data.cFileName);
	}
	while (FindNextFileA(find_handle, &find_data) != 0);

	if (FindClose(find_handle) == 0)
	{
		return AURORA_FS_ERR_CLOSE_FAILED;
	}

	return AURORA_FS_ERR_OK;
#else
	DIR *dir;
	struct dirent *entry;

	path = argc >= 3 && argv[2] != NULL ? argv[2] : ".";
	dir = opendir(path);
	if (dir == NULL)
	{
		return aurora_fs_map_errno();
	}

	while ((entry = readdir(dir)) != NULL)
	{
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
		{
			continue;
		}
		(void)puts(entry->d_name);
	}

	if (closedir(dir) != 0)
	{
		return AURORA_FS_ERR_CLOSE_FAILED;
	}

	return AURORA_FS_ERR_OK;
#endif
}

static aurora_fs_err_t aurora_fs_cmd_exists(int argc, const char *argv[])
{
	aurora_fs_stat_t st;

	if (argc < 3 || argv[2] == NULL)
	{
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	if (aurora_fs_stat_path(argv[2], &st) == 0)
	{
		puts("true");
		return AURORA_FS_ERR_OK;
	}

	if (errno == ENOENT)
	{
		puts("false");
		return AURORA_FS_ERR_OK;
	}

	return aurora_fs_map_errno();
}

static aurora_fs_err_t aurora_fs_cmd_read(int argc, const char *argv[])
{
	char buffer[4096];
	size_t read_size;
	FILE *fp;

	if (argc < 3 || argv[2] == NULL)
	{
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	fp = fopen(argv[2], "rb");
	if (fp == NULL)
	{
		return AURORA_FS_ERR_OPEN_FAILED;
	}

	while ((read_size = fread(buffer, 1, sizeof(buffer), fp)) > 0)
	{
		if (fwrite(buffer, 1, read_size, stdout) != read_size)
		{
			(void)fclose(fp);
			return AURORA_FS_ERR_WRITE_FAILED;
		}
	}

	if (ferror(fp))
	{
		(void)fclose(fp);
		return AURORA_FS_ERR_READ_FAILED;
	}

	if (fclose(fp) != 0)
	{
		return AURORA_FS_ERR_CLOSE_FAILED;
	}

	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_cmd_write_like(int argc, const char *argv[], const char *mode)
{
	FILE *fp;

	if (argc < 4 || argv[2] == NULL || argv[3] == NULL)
	{
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	fp = fopen(argv[2], mode);
	if (fp == NULL)
	{
		return AURORA_FS_ERR_OPEN_FAILED;
	}

	if (fputs(argv[3], fp) == EOF)
	{
		(void)fclose(fp);
		return AURORA_FS_ERR_WRITE_FAILED;
	}

	if (fclose(fp) != 0)
	{
		return AURORA_FS_ERR_CLOSE_FAILED;
	}

	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_cmd_delete(int argc, const char *argv[])
{
	if (argc < 3 || argv[2] == NULL)
	{
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	if (remove(argv[2]) != 0)
	{
		return aurora_fs_map_errno();
	}

	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_cmd_mkdir(int argc, const char *argv[])
{
	if (argc < 3 || argv[2] == NULL)
	{
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	if (aurora_fs_mkdir_path(argv[2]) != 0)
	{
		return aurora_fs_map_errno();
	}

	return AURORA_FS_ERR_OK;
}

static aurora_fs_err_t aurora_fs_cmd_rmdir(int argc, const char *argv[])
{
	if (argc < 3 || argv[2] == NULL)
	{
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	if (aurora_fs_rmdir_path(argv[2]) != 0)
	{
		return aurora_fs_map_errno();
	}

	return AURORA_FS_ERR_OK;
}

aurora_fs_err_t aurora_fs_core_run(int argc, const char *argv[])
{
	const char *command;

	if (argc < 2 || argv == NULL || argv[1] == NULL)
	{
		aurora_fs_print_usage(stderr);
		return AURORA_FS_ERR_INVALID_ARGUMENT;
	}

	command = argv[1];

	if (aurora_fs_cmd_eq(command, "help"))
	{
		aurora_fs_print_usage(stdout);
		return AURORA_FS_ERR_OK;
	}

	if (aurora_fs_cmd_eq(command, "exists"))
	{
		return aurora_fs_cmd_exists(argc, argv);
	}

	if (aurora_fs_cmd_eq(command, "list"))
	{
		return aurora_fs_cmd_list(argc, argv);
	}

	if (aurora_fs_cmd_eq(command, "read"))
	{
		return aurora_fs_cmd_read(argc, argv);
	}

	if (aurora_fs_cmd_eq(command, "write"))
	{
		return aurora_fs_execute_with_history(argc, argv, aurora_fs_cmd_write);
	}

	if (aurora_fs_cmd_eq(command, "append"))
	{
		return aurora_fs_execute_with_history(argc, argv, aurora_fs_cmd_append);
	}

	if (aurora_fs_cmd_eq(command, "delete"))
	{
		return aurora_fs_execute_with_history(argc, argv, aurora_fs_cmd_delete);
	}

	if (aurora_fs_cmd_eq(command, "mkdir"))
	{
		return aurora_fs_execute_with_history(argc, argv, aurora_fs_cmd_mkdir);
	}

	if (aurora_fs_cmd_eq(command, "rmdir"))
	{
		return aurora_fs_execute_with_history(argc, argv, aurora_fs_cmd_rmdir);
	}

	if (aurora_fs_cmd_eq(command, "undo"))
	{
		return aurora_fs_cmd_undo();
	}

	if (aurora_fs_cmd_eq(command, "redo"))
	{
		return aurora_fs_cmd_redo();
	}

	aurora_fs_print_usage(stderr);
	return AURORA_FS_ERR_NOT_SUPPORTED;
}

int aurora_fs_core_main(int argc, char *argv[])
{
	aurora_fs_err_t run_result;
	aurora_fs_err_t log_result;
	char err_buffer[256];

	run_result = aurora_fs_core_run(argc, (const char **)argv);
	if (run_result == AURORA_FS_ERR_OK)
	{
		aurora_fs_core_cleanup();
		return 0;
	}

	(void)snprintf(err_buffer, sizeof(err_buffer), "aurora-fs failed: %s (%d)",
				   aurora_fs_err_to_string(run_result), (int)run_result);

	log_result = aurora_fs_log_init(stderr);
	if (log_result == AURORA_FS_ERR_OK)
	{
		(void)aurora_fs_log(err_buffer, true);
	}
	else
	{
		(void)fprintf(stderr, "%s\n", err_buffer);
	}

	aurora_fs_core_cleanup();

	return 1;
}