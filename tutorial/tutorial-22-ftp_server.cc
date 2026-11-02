#include <sys/stat.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "workflow/WFFtpServer.h"
#include "workflow/WFFacilities.h"

static WFFacilities::WaitGroup wait_group(1);

void sig_handler(int signo)
{
	wait_group.done();
}

int main(int argc, char *argv[])
{
	struct stat st;

	if (argc != 5)
	{
		fprintf(stderr, "USAGE: %s <port> <root> <user> <password>\n"
						"  Serve the directory 'root' as '/' to the user.\n",
				argv[0]);
		return 1;
	}

	if (stat(argv[2], &st) < 0 || !S_ISDIR(st.st_mode))
	{
		fprintf(stderr, "Invalid root directory: %s\n", argv[2]);
		return 1;
	}

	std::string user = argv[3];
	std::string password = argv[4];
	WFFtpServer server(argv[2], [user, password](const std::string& u,
												 const std::string& p) {
		return u == user && p == password;
	});

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	if (server.start(atoi(argv[1])) < 0)
	{
		perror("Cannot start server");
		return 1;
	}

	wait_group.wait();
	server.stop();
	return 0;
}

