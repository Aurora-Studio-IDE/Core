#include "fs.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        aurora_fs_print_usage(stdout);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "egg") == 0)
    {
        (void)printf("If C wasn't so fast, I would have written this in JavaScript.\n");
        return 0;
    }

	return aurora_fs_core_main(argc, argv);
}