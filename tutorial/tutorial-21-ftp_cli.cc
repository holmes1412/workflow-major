#include <stdio.h>
#include <string.h>
#include <string>

#include "workflow/WFFtpClient.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFGlobal.h"

static WFFacilities::WaitGroup wait_group(1);
static int exit_code = 1;

void ftp_callback(WFFtpTask *task)
{
	protocol::FtpResponse *resp = task->get_resp();
	int state = task->get_state();
	int error = task->get_error();

	if (state != WFT_STATE_SUCCESS)
	{
		fprintf(stderr, "Error: %s\n",
				WFGlobal::get_error_string(state, error));
	}

	/* The server reply, including the one of a login failure. */
	if (resp->get_reply_code() != 0)
	{
		fprintf(stderr, "%d %s\n", resp->get_reply_code(),
				resp->get_reply_text().c_str());
	}

	if (state == WFT_STATE_SUCCESS && resp->get_reply_code() / 100 == 2)
		exit_code = 0;

	/* Data of 'ls'. The data of 'get' is extracted while downloading. */
	fwrite(resp->get_data()->data(), 1, resp->get_data()->size(), stdout);
	wait_group.done();
}

static std::string read_stdin()
{
	std::string data;
	char buf[8192];
	size_t n;

	while ((n = fread(buf, 1, sizeof buf, stdin)) > 0)
		data.append(buf, n);

	return data;
}

int main(int argc, char *argv[])
{
	WFFtpTask *task;
	std::string data;

	if (argc < 3)
	{
		fprintf(stderr, "USAGE: %s <operation> <url> [arguments]\n"
						"  url: ftp://[user[:password]@]host[:port]/path\n"
						"  operation:\n"
						"    ls <url>               list a directory\n"
						"    get <url>              download a file to stdout\n"
						"    put <url>              upload a file from stdin\n"
						"    mv <url> <to>          rename a file\n"
						"    cmd <url> <command> [argument]\n"
						"  The path is relative to the login directory. "
						"Use '%%2F' for an absolute path,\n"
						"  such as ftp://host/%%2Fpub/file.\n", argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "ls") == 0 && argc == 3)
		task = WFFtpTaskFactory::create_list_task(argv[2], ftp_callback);
	else if (strcmp(argv[1], "get") == 0 && argc == 3)
	{
		task = WFFtpTaskFactory::create_get_task(argv[2], ftp_callback);
		task->set_extract([](WFFtpTask *, const void *buf, size_t size) {
			fwrite(buf, 1, size, stdout);
		});
	}
	else if (strcmp(argv[1], "put") == 0 && argc == 3)
	{
		data = read_stdin();
		task = WFFtpTaskFactory::create_put_task(argv[2], data.data(),
												 data.size(), ftp_callback);
	}
	else if (strcmp(argv[1], "mv") == 0 && argc == 4)
	{
		task = WFFtpTaskFactory::create_rename_task(argv[2], argv[3],
													ftp_callback);
	}
	else if (strcmp(argv[1], "cmd") == 0 && argc >= 4)
	{
		task = WFFtpTaskFactory::create_command_task(argv[2], argv[3],
													 argc > 4 ? argv[4] : "",
													 ftp_callback);
	}
	else
	{
		fprintf(stderr, "Invalid operation or arguments.\n");
		return 1;
	}

	task->start();
	wait_group.wait();
	return exit_code;
}

