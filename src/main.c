#include <stdio.h>
#include <string.h>

#include "lcr.h"

static const char usage[] =
	"usage: lcr COMMAND [ARG...]\n"
	"\n"
	"  run [options] IMAGE_DIR COMMAND [ARG...]    create and start a container\n"
	"  ps                                          list containers\n"
	"  exec [options] CONTAINER COMMAND [ARG...]   run a command inside a running container\n"
	"  kill CONTAINER [SIGNAL]                     send a signal to the container init\n"
	"  stats CONTAINER                             show cgroup memory, cpu and pid usage\n"
	"  logs CONTAINER                              print the output of a detached container\n"
	"  rm [-f] CONTAINER                           remove a stopped container\n"
	"  version                                     print the version\n"
	"\n"
	"run 'lcr run --help' for the run options\n";

int main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2) {
		fputs(usage, stderr);
		return 125;
	}
	cmd = argv[1];
	if (!strcmp(cmd, "run"))
		return cmd_run(argc - 1, argv + 1);
	if (!strcmp(cmd, "ps"))
		return cmd_ps(argc - 1, argv + 1);
	if (!strcmp(cmd, "exec"))
		return cmd_exec(argc - 1, argv + 1);
	if (!strcmp(cmd, "kill"))
		return cmd_kill(argc - 1, argv + 1);
	if (!strcmp(cmd, "stats"))
		return cmd_stats(argc - 1, argv + 1);
	if (!strcmp(cmd, "logs"))
		return cmd_logs(argc - 1, argv + 1);
	if (!strcmp(cmd, "rm"))
		return cmd_rm(argc - 1, argv + 1);
	if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
		printf("lcr %s\n", LCR_VERSION);
		return 0;
	}
	if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
		fputs(usage, stdout);
		return 0;
	}
	fprintf(stderr, "lcr: unknown command %s\n\n", cmd);
	fputs(usage, stderr);
	return 125;
}
