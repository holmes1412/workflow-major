/*
  Copyright (c) 2026 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Author: Li Yingxin (liyingxin@sogou-inc.com)
*/

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include "workflow/FtpMessage.h"
#include "workflow/WFFtpClient.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFTaskError.h"

using namespace protocol;

class TestFtpRequest : public FtpRequest
{
public:
	int test_encode(struct iovec vectors[], int max)
	{
		return this->encode(vectors, max);
	}
};

class TestFtpResponse : public FtpResponse
{
public:
	int test_append(const std::string& str, size_t *size)
	{
		*size = str.size();
		return this->append(str.data(), size);
	}

	int renew_count = 0;

protected:
	virtual void renew() { this->renew_count++; }
};

class TestFtpDataMessage : public FtpDataMessage
{
public:
	int test_append(const std::string& str)
	{
		size_t size = str.size();
		return this->append(str.data(), &size);
	}

	int test_encode(struct iovec vectors[], int max)
	{
		return this->encode(vectors, max);
	}
};

TEST(ftp_unittest, request_encode)
{
	TestFtpRequest req;
	struct iovec vectors[4];

	EXPECT_EQ(req.test_encode(vectors, 4), 0);

	req.set_command("PWD");
	ASSERT_EQ(req.test_encode(vectors, 4), 1);
	EXPECT_EQ(std::string((char *)vectors[0].iov_base, vectors[0].iov_len),
			  "PWD\r\n");

	req.set_command("RETR", "/pub/a file.txt");
	ASSERT_EQ(req.test_encode(vectors, 4), 1);
	EXPECT_EQ(std::string((char *)vectors[0].iov_base, vectors[0].iov_len),
			  "RETR /pub/a file.txt\r\n");

	req.set_command("DELE", "a\r\nRMD /");
	EXPECT_EQ(req.test_encode(vectors, 4), -1);
	EXPECT_EQ(errno, EBADMSG);

	req.set_command("DE LE", "a");
	EXPECT_EQ(req.test_encode(vectors, 4), -1);
}

TEST(ftp_unittest, response_single_line)
{
	TestFtpResponse resp;
	size_t size;

	EXPECT_EQ(resp.test_append("220 Service ready.\r\n", &size), 1);
	EXPECT_EQ(size, 20u);
	EXPECT_EQ(resp.get_reply_code(), 220);
	EXPECT_EQ(resp.get_reply_text(), "Service ready.");
	EXPECT_EQ(resp.get_preliminary_code(), 0);
	EXPECT_EQ(resp.renew_count, 0);
}

TEST(ftp_unittest, response_multi_line)
{
	TestFtpResponse resp;
	size_t size;

	EXPECT_EQ(resp.test_append("211-Features:\r\n MDTM\r\n", &size), 0);
	EXPECT_EQ(resp.test_append("211-not the end\r\n SIZE\r\n211 End\r\n",
							   &size), 1);
	EXPECT_EQ(resp.get_reply_code(), 211);
	EXPECT_EQ(resp.get_reply_text(),
			  "Features:\n MDTM\n211-not the end\n SIZE\nEnd");
}

TEST(ftp_unittest, response_preliminary)
{
	TestFtpResponse resp;
	std::string data = "150 Opening data connection.\r\n"
					   "226 Transfer complete.\r\n";
	size_t size;

	EXPECT_EQ(resp.test_append(data + "220 extra\r\n", &size), 1);
	EXPECT_EQ(size, data.size());
	EXPECT_EQ(resp.get_preliminary_code(), 150);
	EXPECT_EQ(resp.get_preliminary_text(), "Opening data connection.");
	EXPECT_EQ(resp.get_reply_code(), 226);
	EXPECT_EQ(resp.get_reply_text(), "Transfer complete.");
	EXPECT_EQ(resp.renew_count, 1);
}

TEST(ftp_unittest, response_split)
{
	TestFtpResponse resp;
	std::string data = "331 Username ok, send password.\n";
	size_t size;

	for (size_t i = 0; i + 1 < data.size(); i++)
		ASSERT_EQ(resp.test_append(data.substr(i, 1), &size), 0);

	EXPECT_EQ(resp.test_append(data.substr(data.size() - 1), &size), 1);
	EXPECT_EQ(resp.get_reply_code(), 331);
	EXPECT_EQ(resp.get_reply_text(), "Username ok, send password.");
}

TEST(ftp_unittest, response_any_split)
{
	const std::string data = "150-Opening\r\n data\r\n150 connection.\r\n"
							 "226-Transfer\n226 complete.\r\n";
	const std::string extra = "220 next\r\n";

	/* Three pieces at every position, the last one with extra bytes. */
	for (size_t i = 1; i < data.size(); i++)
	{
		for (size_t j = i; j < data.size(); j++)
		{
			std::string pieces[3] = {
				data.substr(0, i), data.substr(i, j - i), data.substr(j) + extra
			};
			TestFtpResponse resp;
			size_t size;

			for (int k = 0; k < 2; k++)
			{
				if (!pieces[k].empty())
					ASSERT_EQ(resp.test_append(pieces[k], &size), 0);
			}

			ASSERT_EQ(resp.test_append(pieces[2], &size), 1) << i << "," << j;
			ASSERT_EQ(size, pieces[2].size() - extra.size()) << i << "," << j;
			ASSERT_EQ(resp.get_preliminary_code(), 150);
			ASSERT_EQ(resp.get_preliminary_text(),
					  "Opening\n data\nconnection.");
			ASSERT_EQ(resp.get_reply_code(), 226);
			ASSERT_EQ(resp.get_reply_text(), "Transfer\ncomplete.");
			ASSERT_EQ(resp.renew_count, 1);
		}
	}
}

TEST(ftp_unittest, response_error)
{
	TestFtpResponse bad;
	TestFtpResponse big;
	size_t size;

	EXPECT_EQ(bad.test_append("hello\r\n", &size), -1);
	EXPECT_EQ(errno, EBADMSG);

	big.set_size_limit(16);
	EXPECT_EQ(big.test_append("230-0123456789\r\n", &size), 0);
	EXPECT_EQ(big.test_append("230 end\r\n", &size), -1);
	EXPECT_EQ(errno, EMSGSIZE);
}

TEST(ftp_unittest, data_message)
{
	TestFtpDataMessage msg;
	struct iovec vectors[4];

	EXPECT_EQ(msg.test_encode(vectors, 4), 0);
	EXPECT_EQ(msg.test_append("hello "), 0);
	EXPECT_EQ(msg.test_append("world"), 0);
	EXPECT_EQ(*msg.get_data(), "hello world");
	ASSERT_EQ(msg.test_encode(vectors, 4), 1);
	EXPECT_EQ(vectors[0].iov_len, 11u);

	msg.set_size_limit(12);
	EXPECT_EQ(msg.test_append("!!"), -1);
	EXPECT_EQ(errno, EMSGSIZE);
}

static int __listen_local(unsigned short *port)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int on = 1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
		listen(fd, 16) < 0 ||
		getsockname(fd, (struct sockaddr *)&addr, &len) < 0)
	{
		close(fd);
		return -1;
	}

	*port = ntohs(addr.sin_port);
	return fd;
}

static bool __wait_readable(int fd, int timeout)
{
	struct pollfd pfd = { };

	pfd.fd = fd;
	pfd.events = POLLIN;
	return poll(&pfd, 1, timeout) > 0;
}

static void __send_all(int fd, const std::string& data)
{
	size_t pos = 0;
	ssize_t n;

	while (pos < data.size())
	{
		n = send(fd, data.data() + pos, data.size() - pos, 0);
		if (n <= 0)
			return;

		pos += n;
	}
}

/* A tiny blocking FTP server. Like vsftpd, it accepts a data connection
 * before the preliminary reply, and never accepts it if the file is missing. */
class MiniFtpServer
{
public:
	unsigned short start(bool epsv)
	{
		unsigned short port = 0;

		signal(SIGPIPE, SIG_IGN);
		this->epsv = epsv;
		this->listen_fd = __listen_local(&port);
		this->acceptor = std::thread(&MiniFtpServer::accept_routine, this);
		return port;
	}

	void stop()
	{
		this->stopping = true;
		this->acceptor.join();
		for (std::thread& t : this->sessions)
			t.join();

		close(this->listen_fd);
	}

	void set_file(const std::string& path, std::string data)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->files[path] = std::move(data);
	}

	bool get_file(const std::string& path, std::string& data)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		auto it = this->files.find(path);

		if (it == this->files.end())
			return false;

		data = it->second;
		return true;
	}

	bool remove_file(const std::string& path)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->files.erase(path) > 0;
	}

	std::string get_last_retr()
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		return this->last_retr;
	}

	std::atomic<int> logins{0};
	std::atomic<int> max_sessions{0};

private:
	void accept_routine()
	{
		int fd;

		while (!this->stopping)
		{
			if (!__wait_readable(this->listen_fd, 50))
				continue;

			fd = accept(this->listen_fd, NULL, NULL);
			if (fd >= 0)
				this->sessions.emplace_back(&MiniFtpServer::session_routine,
											this, fd);
		}
	}

	bool read_line(int fd, std::string& buf, std::string& line)
	{
		size_t pos;
		char tmp[4096];
		ssize_t n;

		while ((pos = buf.find("\r\n")) == std::string::npos)
		{
			if (this->stopping)
				return false;

			if (!__wait_readable(fd, 50))
				continue;

			n = recv(fd, tmp, sizeof tmp, 0);
			if (n <= 0)
				return false;

			buf.append(tmp, n);
		}

		line = buf.substr(0, pos);
		buf.erase(0, pos + 2);
		return true;
	}

	void transfer(int fd, int data_fd, const std::string& cmd,
				  const std::string& arg)
	{
		std::string data;
		char tmp[4096];
		ssize_t n;

		if (cmd == "RETR")
		{
			/* Both replies in one write, after the data is sent. */
			this->get_file(arg, data);
			__send_all(data_fd, data);
			close(data_fd);
			__send_all(fd, "150 Opening data connection.\r\n"
						   "226 Transfer complete.\r\n");
		}
		else if (cmd == "LIST")
		{
			__send_all(fd, "150 Here comes the listing.\r\n");
			{
				std::lock_guard<std::mutex> lock(this->mutex);
				for (const auto& kv : this->files)
					data += kv.first + "\r\n";
			}

			__send_all(data_fd, data);
			close(data_fd);
			__send_all(fd, "226 Directory send OK.\r\n");
		}
		else
		{
			__send_all(fd, "150 Ok to send data.\r\n");
			while ((n = recv(data_fd, tmp, sizeof tmp, 0)) > 0)
				data.append(tmp, n);

			close(data_fd);
			this->set_file(arg, std::move(data));
			__send_all(fd, "226 Transfer complete.\r\n");
		}
	}

	void session_routine(int fd)
	{
		std::string buf, line, cmd, arg, data, rename_from;
		unsigned short port;
		int pasv_fd = -1;
		int data_fd;
		char reply[128];
		size_t pos;
		int active = ++this->active_sessions;
		int max = this->max_sessions;

		while (active > max &&
			   !this->max_sessions.compare_exchange_weak(max, active))
		{
		}

		__send_all(fd, "220 Mini FTP ready.\r\n");
		while (this->read_line(fd, buf, line))
		{
			pos = line.find(' ');
			cmd = line.substr(0, pos);
			arg = pos == std::string::npos ? "" : line.substr(pos + 1);
			if (cmd == "RETR")
			{
				std::lock_guard<std::mutex> lock(this->mutex);
				this->last_retr = arg;
			}

			/* The login directory is "/". */
			if ((cmd == "RETR" || cmd == "STOR" || cmd == "RNFR" ||
				 cmd == "RNTO" || cmd == "DELE") && arg[0] != '/')
			{
				arg = "/" + arg;
			}

			if (cmd == "USER")
				__send_all(fd, "331 Password required.\r\n");
			else if (cmd == "PASS" && arg == "pass")
			{
				this->logins++;
				__send_all(fd, "230 Logged in.\r\n");
			}
			else if (cmd == "PASS")
				__send_all(fd, "530 Login incorrect.\r\n");
			else if (cmd == "TYPE" || cmd == "NOOP")
				__send_all(fd, "200 Ok.\r\n");
			else if (cmd == "PWD")
				__send_all(fd, "257 \"/\" is the current directory.\r\n");
			else if (cmd == "FEAT")
			{
				__send_all(fd, "211-Features:\r\n EPSV\r\n PASV\r\n"
							   "211 End\r\n");
			}
			else if (cmd == "EPSV" && !this->epsv)
				__send_all(fd, "502 Not implemented.\r\n");
			else if (cmd == "EPSV" || cmd == "PASV")
			{
				if (pasv_fd >= 0)
					close(pasv_fd);

				pasv_fd = __listen_local(&port);
				if (cmd == "EPSV")
				{
					snprintf(reply, sizeof reply,
							 "229 Extended Passive Mode (|||%u|)\r\n", port);
				}
				else
				{
					snprintf(reply, sizeof reply,
							 "227 Passive Mode (127,0,0,1,%u,%u)\r\n",
							 port >> 8, port & 255);
				}

				__send_all(fd, reply);
			}
			else if (cmd == "RETR" || cmd == "LIST" || cmd == "STOR")
			{
				if (cmd == "RETR" && !this->get_file(arg, data))
				{
					__send_all(fd, "550 No such file.\r\n");
					continue;
				}

				data_fd = -1;
				if (pasv_fd >= 0 && __wait_readable(pasv_fd, 5000))
					data_fd = accept(pasv_fd, NULL, NULL);

				if (pasv_fd >= 0)
					close(pasv_fd);

				pasv_fd = -1;
				if (data_fd < 0)
					__send_all(fd, "425 Can't open data connection.\r\n");
				else
					this->transfer(fd, data_fd, cmd, arg);
			}
			else if (cmd == "RNFR" && this->get_file(arg, data))
			{
				rename_from = arg;
				__send_all(fd, "350 Ready for RNTO.\r\n");
			}
			else if (cmd == "RNTO" && !rename_from.empty())
			{
				this->get_file(rename_from, data);
				this->remove_file(rename_from);
				this->set_file(arg, std::move(data));
				rename_from.clear();
				__send_all(fd, "250 Rename successful.\r\n");
			}
			else if (cmd == "DELE" && this->remove_file(arg))
				__send_all(fd, "250 Deleted.\r\n");
			else if (cmd == "QUIT")
			{
				__send_all(fd, "221 Goodbye.\r\n");
				break;
			}
			else
				__send_all(fd, "550 Failed.\r\n");
		}

		if (pasv_fd >= 0)
			close(pasv_fd);

		close(fd);
		this->active_sessions--;
	}

private:
	std::map<std::string, std::string> files;
	std::string last_retr;
	std::mutex mutex;
	std::thread acceptor;
	std::vector<std::thread> sessions;
	std::atomic<bool> stopping{false};
	std::atomic<int> active_sessions{0};
	int listen_fd;
	bool epsv;
};

struct FtpResult
{
	int state;
	int error;
	int code;
	std::string text;
	std::string data;
};

static FtpResult __run(WFFtpTask *task)
{
	WFFacilities::WaitGroup wait_group(1);
	FtpResult res;

	task->set_callback([&res, &wait_group](WFFtpTask *task) {
		res.state = task->get_state();
		res.error = task->get_error();
		res.code = task->get_resp()->get_reply_code();
		res.text = task->get_resp()->get_reply_text();
		res.data = std::move(*task->get_resp()->get_data());
		wait_group.done();
	});

	task->start();
	wait_group.wait();
	return res;
}

static std::string __url(unsigned short port, const char *password)
{
	return std::string("ftp://user:") + password + "@127.0.0.1:" +
		   std::to_string(port);
}

TEST(ftp_unittest, client_transfer)
{
	MiniFtpServer server;
	WFFtpClient client(1);
	std::string data(1024 * 1024, 0);
	std::string file;
	FtpResult res;

	for (size_t i = 0; i < data.size(); i++)
		data[i] = (char)(i * 7 + 3);

	ASSERT_EQ(client.init(__url(server.start(true), "pass")), 0);
	server.set_file("/a.txt", "hello");

	res = __run(client.create_command_task("FEAT", "", nullptr));
	EXPECT_EQ(res.state, WFT_STATE_SUCCESS);
	EXPECT_EQ(res.code, 211);
	EXPECT_EQ(res.text, "Features:\n EPSV\n PASV\nEnd");

	res = __run(client.create_get_task("/a.txt", nullptr));
	EXPECT_EQ(res.state, WFT_STATE_SUCCESS);
	EXPECT_EQ(res.code, 226);
	EXPECT_EQ(res.data, "hello");

	res = __run(client.create_put_task("/b.bin", data.data(), data.size(),
									   nullptr));
	EXPECT_EQ(res.state, WFT_STATE_SUCCESS);
	EXPECT_EQ(res.code, 226);
	EXPECT_TRUE(server.get_file("/b.bin", file));
	EXPECT_EQ(file, data);

	res = __run(client.create_put_task("/empty", NULL, 0, nullptr));
	EXPECT_EQ(res.code, 226);
	EXPECT_TRUE(server.get_file("/empty", file));
	EXPECT_TRUE(file.empty());

	res = __run(client.create_list_task("/", nullptr));
	EXPECT_EQ(res.code, 226);
	EXPECT_EQ(res.data, "/a.txt\r\n/b.bin\r\n/empty\r\n");

	res = __run(client.create_rename_task("/a.txt", "/c.txt", nullptr));
	EXPECT_EQ(res.code, 250);
	EXPECT_TRUE(server.get_file("/c.txt", file));

	res = __run(client.create_command_task("DELE", "/c.txt", nullptr));
	EXPECT_EQ(res.code, 250);
	EXPECT_FALSE(server.get_file("/c.txt", file));

	/* All the tasks share one connection. */
	EXPECT_EQ(server.logins.load(), 1);
	server.stop();
}

TEST(ftp_unittest, client_pasv)
{
	MiniFtpServer server;
	WFFtpClient client(2);
	std::string file;
	FtpResult res;

	ASSERT_EQ(client.init(__url(server.start(false), "pass")), 0);
	res = __run(client.create_put_task("/p.txt", "pasv", 4, nullptr));
	EXPECT_EQ(res.state, WFT_STATE_SUCCESS);
	EXPECT_EQ(res.code, 226);

	res = __run(client.create_get_task("/p.txt", nullptr));
	EXPECT_EQ(res.code, 226);
	EXPECT_EQ(res.data, "pasv");
	server.stop();
}

TEST(ftp_unittest, client_errors)
{
	MiniFtpServer server;
	unsigned short port = server.start(true);
	WFFtpClient client(3);
	WFFtpClient wrong(4);
	FtpResult res;

	ASSERT_EQ(client.init(__url(port, "pass")), 0);
	ASSERT_EQ(wrong.init(__url(port, "wrong")), 0);
	EXPECT_EQ(WFFtpClient(5).init("http://127.0.0.1/"), -1);

	res = __run(client.create_command_task("PASV", "", nullptr));
	EXPECT_EQ(res.state, WFT_STATE_TASK_ERROR);
	EXPECT_EQ(res.error, WFT_ERR_FTP_COMMAND_DISALLOWED);

	/* The data connection is never accepted. */
	res = __run(client.create_get_task("/missing", nullptr));
	EXPECT_EQ(res.state, WFT_STATE_SUCCESS);
	EXPECT_EQ(res.code, 550);

	res = __run(client.create_command_task("PWD", "", nullptr));
	EXPECT_EQ(res.code, 257);

	res = __run(wrong.create_command_task("PWD", "", nullptr));
	EXPECT_EQ(res.state, WFT_STATE_TASK_ERROR);
	EXPECT_EQ(res.error, WFT_ERR_FTP_LOGIN_FAILED);
	EXPECT_EQ(res.code, 530);

	res = __run(client.create_quit_task(nullptr));
	EXPECT_EQ(res.code, 221);

	res = __run(client.create_command_task("NOOP", "", nullptr));
	EXPECT_EQ(res.code, 200);
	server.stop();
}

TEST(ftp_unittest, client_concurrent)
{
	MiniFtpServer server;
	WFFtpClient client(6);
	WFFacilities::WaitGroup wait_group(30);
	std::atomic<int> success(0);
	std::string file;

	ASSERT_EQ(client.init(__url(server.start(true), "pass")), 0);
	server.set_file("/shared", std::string(100000, 's'));
	for (int i = 0; i < 30; i++)
	{
		std::string name = "/f" + std::to_string(i);
		WFFtpTask *task;

		if (i % 2 == 0)
			task = client.create_get_task("/shared", nullptr);
		else
		{
			task = client.create_put_task(name, name.data(), name.size(),
										  nullptr);
		}

		task->set_callback([&](WFFtpTask *task) {
			if (task->get_state() == WFT_STATE_SUCCESS &&
				task->get_resp()->get_reply_code() == 226)
			{
				success++;
			}

			wait_group.done();
		});
		task->start();
	}

	wait_group.wait();
	EXPECT_EQ(success.load(), 30);
	EXPECT_TRUE(server.get_file("/f29", file));
	EXPECT_EQ(file, "/f29");
	EXPECT_EQ(server.logins.load(), 1);
	server.stop();
}

TEST(ftp_unittest, url_transfer)
{
	MiniFtpServer server;
	std::string url = __url(server.start(true), "pass");
	std::string file;
	FtpResult res;

	server.set_file("/dir/a b.txt", "space");
	res = __run(WFFtpTaskFactory::create_get_task(url + "/dir/a%20b.txt",
												  nullptr));
	EXPECT_EQ(res.state, WFT_STATE_SUCCESS);
	EXPECT_EQ(res.code, 226);
	EXPECT_EQ(res.data, "space");
	/* Relative to the login directory. */
	EXPECT_EQ(server.get_last_retr(), "dir/a b.txt");

	res = __run(WFFtpTaskFactory::create_get_task(url + "/%2Fdir/a%20b.txt",
												  nullptr));
	EXPECT_EQ(res.data, "space");
	EXPECT_EQ(server.get_last_retr(), "/dir/a b.txt");

	res = __run(WFFtpTaskFactory::create_put_task(url + "/up.txt", "url", 3,
												  nullptr));
	EXPECT_EQ(res.code, 226);
	EXPECT_TRUE(server.get_file("/up.txt", file));
	EXPECT_EQ(file, "url");

	res = __run(WFFtpTaskFactory::create_list_task(url + "/", nullptr));
	EXPECT_EQ(res.code, 226);
	EXPECT_EQ(res.data, "/dir/a b.txt\r\n/up.txt\r\n");

	res = __run(WFFtpTaskFactory::create_rename_task(url + "/up.txt",
													 "/moved.txt", nullptr));
	EXPECT_EQ(res.code, 250);
	EXPECT_TRUE(server.get_file("/moved.txt", file));

	res = __run(WFFtpTaskFactory::create_command_task(url, "DELE",
													  "/moved.txt", nullptr));
	EXPECT_EQ(res.code, 250);

	res = __run(WFFtpTaskFactory::create_get_task(url + "/missing", nullptr));
	EXPECT_EQ(res.state, WFT_STATE_SUCCESS);
	EXPECT_EQ(res.code, 550);

	res = __run(WFFtpTaskFactory::create_command_task(url, "RETR", "x",
													  nullptr));
	EXPECT_EQ(res.error, WFT_ERR_FTP_COMMAND_DISALLOWED);

	res = __run(WFFtpTaskFactory::create_get_task("http://127.0.0.1/a",
												  nullptr));
	EXPECT_EQ(res.state, WFT_STATE_TASK_ERROR);
	EXPECT_EQ(res.error, WFT_ERR_URI_SCHEME_INVALID);

	res = __run(WFFtpTaskFactory::create_get_task("ftp:///a", nullptr));
	EXPECT_EQ(res.state, WFT_STATE_TASK_ERROR);
	EXPECT_EQ(res.error, WFT_ERR_URI_PARSE_FAILED);
	server.stop();
}

TEST(ftp_unittest, stream)
{
	MiniFtpServer server;
	std::string url = __url(server.start(true), "pass");
	std::string data(3 * 1024 * 1024, 0);
	std::string received;
	WFFtpClient client(7);
	WFFtpTask *task;
	FtpResult res;
	int pieces = 0;

	for (size_t i = 0; i < data.size(); i++)
		data[i] = (char)(i * 13 + 5);

	server.set_file("/big", data);
	auto extract = [&](WFFtpTask *task, const void *buf, size_t size) {
		received.append((const char *)buf, size);
		pieces++;
	};

	task = WFFtpTaskFactory::create_get_task(url + "/big", nullptr);
	task->set_extract(extract);
	res = __run(task);
	EXPECT_EQ(res.code, 226);
	EXPECT_TRUE(res.data.empty());
	EXPECT_EQ(received, data);
	EXPECT_GT(pieces, 1);

	received.clear();
	ASSERT_EQ(client.init(url), 0);
	task = client.create_get_task("/big", nullptr);
	task->set_extract(extract);
	res = __run(task);
	EXPECT_EQ(res.code, 226);
	EXPECT_EQ(received, data);
	server.stop();
}

TEST(ftp_unittest, url_max_connections)
{
	MiniFtpServer server1;
	MiniFtpServer server2;
	std::string url1 = __url(server1.start(true), "pass");
	std::string url2 = __url(server2.start(true), "pass");
	WFFacilities::WaitGroup wait_group(24);
	std::atomic<int> success(0);

	WFFtpTaskFactory::set_max_connections(2);
	server1.set_file("/x", std::string(200000, 'x'));
	server2.set_file("/x", std::string(200000, 'x'));
	for (int i = 0; i < 24; i++)
	{
		const std::string& url = i % 2 ? url1 : url2;
		WFFtpTask *task = WFFtpTaskFactory::create_get_task(url + "/x",
															nullptr);

		task->set_callback([&](WFFtpTask *task) {
			if (task->get_state() == WFT_STATE_SUCCESS &&
				task->get_resp()->get_data()->size() == 200000)
			{
				success++;
			}

			wait_group.done();
		});
		task->start();
	}

	wait_group.wait();
	WFFtpTaskFactory::set_max_connections(4);
	EXPECT_EQ(success.load(), 24);
	EXPECT_EQ(server1.max_sessions.load(), 2);
	EXPECT_EQ(server2.max_sessions.load(), 2);
	EXPECT_EQ(server1.logins.load(), 2);
	EXPECT_EQ(server2.logins.load(), 2);
	server1.stop();
	server2.stop();
}

