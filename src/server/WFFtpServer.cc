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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <utility>
#include <functional>
#include "WFTaskFactory.h"
#include "WFFtpServer.h"

#define FTP_TRANSFER_BUFSIZE	(64 * 1024)
#define FTP_LOGIN_ATTEMPTS		3

struct __FtpSession
{
	std::string user;
	bool logged_in;
	bool closing;
	int failures;
	std::string cwd;
	std::string rename_from;
	int pasv_fd;
	int fd;
	struct sockaddr_storage local_addr;
	socklen_t local_addrlen;

	__FtpSession(int fd) :
		logged_in(false), closing(false), failures(0), cwd("/"), pasv_fd(-1)
	{
		this->fd = fd;
	}

	~__FtpSession()
	{
		if (this->pasv_fd >= 0)
			close(this->pasv_fd);
	}
};

/* The deleter of the connection context, which is called right before the
 * connection is closed. Tell the client unless a closing reply is sent,
 * such as after an idle timeout. */
static void __ftp_session_deleter(void *context)
{
	static const char reply[] = "421 Closing control connection.\r\n";
	auto *session = (__FtpSession *)context;

	if (!session->closing && write(session->fd, reply, sizeof reply - 1) < 0)
	{
		/* The client may have closed the connection. */
	}

	delete session;
}

enum FtpTransferType
{
	FTP_TRANSFER_LIST,
	FTP_TRANSFER_NLST,
	FTP_TRANSFER_RETR,
	FTP_TRANSFER_STOR
};

struct __FtpTransfer
{
	enum FtpTransferType type;
	int listen_fd;
	int file_fd;
	std::string path;
	struct sockaddr_storage peer;
	int timeout;
	int code;
	std::string text;

	__FtpTransfer() :
		listen_fd(-1), file_fd(-1),
		code(451), text("Local error in processing.")
	{
	}

	~__FtpTransfer()
	{
		if (this->listen_fd >= 0)
			close(this->listen_fd);

		if (this->file_fd >= 0)
			close(this->file_fd);
	}
};

static int __set_fd_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if (flags >= 0)
		flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	return flags;
}

static int __wait_fd(int fd, short events, int timeout)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	do
		ret = poll(&pfd, 1, timeout);
	while (ret < 0 && errno == EINTR);

	if (ret == 0)
		errno = ETIMEDOUT;

	return ret > 0 ? 0 : -1;
}

static int __send_all(int fd, const char *buf, size_t size, int timeout)
{
	ssize_t n;

	while (size > 0)
	{
		n = send(fd, buf, size, 0);
		if (n >= 0)
		{
			buf += n;
			size -= n;
		}
		else if (errno == EAGAIN)
		{
			if (__wait_fd(fd, POLLOUT, timeout) < 0)
				return -1;
		}
		else if (errno != EINTR)
			return -1;
	}

	return 0;
}

static ssize_t __recv_some(int fd, char *buf, size_t size, int timeout)
{
	ssize_t n;

	while (1)
	{
		n = recv(fd, buf, size, 0);
		if (n >= 0)
			return n;

		if (errno == EAGAIN)
		{
			if (__wait_fd(fd, POLLIN, timeout) < 0)
				return -1;
		}
		else if (errno != EINTR)
			return -1;
	}
}

static int __write_all(int fd, const char *buf, size_t size)
{
	ssize_t n;

	while (size > 0)
	{
		n = write(fd, buf, size);
		if (n >= 0)
		{
			buf += n;
			size -= n;
		}
		else if (errno != EINTR)
			return -1;
	}

	return 0;
}

static bool __same_host(const struct sockaddr_storage *a,
						const struct sockaddr_storage *b)
{
	const void *addr1;
	const void *addr2;
	size_t len;

	if (a->ss_family != b->ss_family)
		return false;

	if (a->ss_family == AF_INET)
	{
		addr1 = &((const struct sockaddr_in *)a)->sin_addr;
		addr2 = &((const struct sockaddr_in *)b)->sin_addr;
		len = sizeof (struct in_addr);
	}
	else if (a->ss_family == AF_INET6)
	{
		addr1 = &((const struct sockaddr_in6 *)a)->sin6_addr;
		addr2 = &((const struct sockaddr_in6 *)b)->sin6_addr;
		len = sizeof (struct in6_addr);
	}
	else
		return false;

	return memcmp(addr1, addr2, len) == 0;
}

static int __accept_data(int listen_fd, const struct sockaddr_storage *peer,
						 int timeout)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int fd;

	while (__wait_fd(listen_fd, POLLIN, timeout) >= 0)
	{
		addrlen = sizeof addr;
		fd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
		if (fd >= 0)
		{
			/* Only the client of the control connection. */
			if (__same_host(&addr, peer) && __set_fd_nonblock(fd) >= 0)
				return fd;

			close(fd);
		}
		else if (errno != EAGAIN && errno != EINTR && errno != ECONNABORTED)
			break;
	}

	return -1;
}

static void __append_list_entry(std::string& out, const char *name,
								const struct stat *st)
{
	static const char *const months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	time_t now = time(NULL);
	char perm[11];
	char date[16];
	char line[128];
	struct tm tm;
	int i;

	if (S_ISDIR(st->st_mode))
		perm[0] = 'd';
	else if (S_ISLNK(st->st_mode))
		perm[0] = 'l';
	else
		perm[0] = '-';

	for (i = 0; i < 9; i++)
		perm[i + 1] = (st->st_mode & (0400 >> i)) ? "rwxrwxrwx"[i] : '-';

	perm[10] = '\0';
	localtime_r(&st->st_mtime, &tm);
	if (st->st_mtime > now - 180 * 24 * 3600 && st->st_mtime < now + 3600)
	{
		snprintf(date, sizeof date, "%s %2d %02d:%02d", months[tm.tm_mon],
				 tm.tm_mday, tm.tm_hour, tm.tm_min);
	}
	else
	{
		snprintf(date, sizeof date, "%s %2d  %d", months[tm.tm_mon],
				 tm.tm_mday, tm.tm_year + 1900);
	}

	snprintf(line, sizeof line, "%s %3u ftp      ftp      %12lld %s ", perm,
			 (unsigned int)st->st_nlink, (long long)st->st_size, date);
	out += line;
	out += name;
	out += "\r\n";
}

static int __make_list(const std::string& path, bool names_only,
					   std::string& out)
{
	struct dirent *ent;
	struct stat st;
	std::string file;
	const char *name;
	DIR *dir;

	if (stat(path.c_str(), &st) < 0)
		return -1;

	if (!S_ISDIR(st.st_mode))
	{
		name = strrchr(path.c_str(), '/');
		name = name ? name + 1 : path.c_str();
		if (names_only)
			out.append(name).append("\r\n");
		else
			__append_list_entry(out, name, &st);

		return 0;
	}

	dir = opendir(path.c_str());
	if (!dir)
		return -1;

	while ((ent = readdir(dir)) != NULL)
	{
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		if (names_only)
		{
			out.append(ent->d_name).append("\r\n");
			continue;
		}

		file = path + "/" + ent->d_name;
		if (stat(file.c_str(), &st) == 0 || lstat(file.c_str(), &st) == 0)
			__append_list_entry(out, ent->d_name, &st);
	}

	closedir(dir);
	return 0;
}

/* Runs in the thread pool. The result is the final reply. */
static void __ftp_transfer(__FtpTransfer *t)
{
	bool file_error = false;
	std::string buf;
	ssize_t n = 0;
	int ret = 0;
	int fd;

	fd = __accept_data(t->listen_fd, &t->peer, t->timeout);
	close(t->listen_fd);
	t->listen_fd = -1;
	if (fd < 0)
	{
		t->code = 425;
		t->text = "Can't open data connection.";
		return;
	}

	switch (t->type)
	{
	case FTP_TRANSFER_RETR:
		buf.resize(FTP_TRANSFER_BUFSIZE);
		while ((n = read(t->file_fd, &buf[0], buf.size())) > 0)
		{
			ret = __send_all(fd, buf.data(), n, t->timeout);
			if (ret < 0)
				break;
		}

		file_error = (n < 0);
		break;

	case FTP_TRANSFER_STOR:
		buf.resize(FTP_TRANSFER_BUFSIZE);
		while ((n = __recv_some(fd, &buf[0], buf.size(), t->timeout)) > 0)
		{
			if (__write_all(t->file_fd, buf.data(), n) < 0)
			{
				file_error = true;
				break;
			}
		}

		ret = (n < 0) ? -1 : 0;
		break;

	default:
		if (__make_list(t->path, t->type == FTP_TRANSFER_NLST, buf) < 0)
			file_error = true;
		else
			ret = __send_all(fd, buf.data(), buf.size(), t->timeout);

		break;
	}

	if (file_error && errno == ENOSPC)
	{
		t->code = 452;
		t->text = "Insufficient storage space.";
	}
	else if (file_error)
	{
		t->code = 451;
		t->text = "Local error in processing.";
	}
	else if (ret < 0)
	{
		t->code = 426;
		t->text = "Connection closed; transfer aborted.";
	}
	else
	{
		t->code = 226;
		t->text = "Transfer complete.";
	}

	/* For download, closing is the end of the data. */
	close(fd);
}

/* Resolve 'arg' to a normalized virtual path, which never goes above '/'. */
static std::string __virtual_path(const std::string& cwd,
								  const std::string& arg)
{
	std::string path = (!arg.empty() && arg[0] == '/') ? arg : cwd + "/" + arg;
	std::vector<std::string> parts;
	std::string vpath;
	std::string part;
	size_t pos = 0;
	size_t next;

	while (pos <= path.size())
	{
		next = path.find('/', pos);
		if (next == std::string::npos)
			next = path.size();

		part = path.substr(pos, next - pos);
		if (part == "..")
		{
			if (!parts.empty())
				parts.pop_back();
		}
		else if (!part.empty() && part != ".")
			parts.push_back(std::move(part));

		pos = next + 1;
	}

	for (const std::string& p : parts)
		vpath.append("/").append(p);

	return vpath.empty() ? "/" : vpath;
}

/* Whether the real path, or that of the nearest existing parent, is in
 * root. Symbolic links pointing out of root are not followed. */
static bool __in_root(const std::string& root, std::string path)
{
	char buf[PATH_MAX];
	size_t pos;

	while (!realpath(path.c_str(), buf))
	{
		pos = path.rfind('/');
		if (errno != ENOENT || pos == std::string::npos)
			return false;

		path.resize(pos > 0 ? pos : 1);
	}

	pos = root.size();
	return strncmp(buf, root.c_str(), pos) == 0 &&
		   (buf[pos] == '\0' || buf[pos] == '/');
}

static std::string __quote_path(const std::string& vpath)
{
	std::string quoted = "\"";

	for (char c : vpath)
	{
		quoted.push_back(c);
		if (c == '"')
			quoted.push_back('"');
	}

	return quoted + "\"";
}

/* Skip options like '-la', which are sent by some clients. */
static std::string __list_argument(const std::string& arg)
{
	size_t pos = 0;

	while (pos < arg.size() && arg[pos] == '-')
	{
		pos = arg.find(' ', pos);
		if (pos == std::string::npos)
			return "";

		pos = arg.find_first_not_of(' ', pos);
		if (pos == std::string::npos)
			return "";
	}

	return arg.substr(pos);
}

WFFtpServer::WFFtpServer(const struct WFServerParams *params,
						 const struct WFFtpDataParams *data_params,
						 const std::string& root, ftp_auth_t auth) :
	WFServer(params, std::bind(&WFFtpServer::process, this,
							   std::placeholders::_1)),
	auth(std::move(auth)),
	port_next(0)
{
	char buf[PATH_MAX];

	this->root = realpath(root.c_str(), buf) ? buf : root;
	if (this->root == "/")
		this->root.clear();

	this->data_params = *data_params;
	this->executor_ready = false;
	if (data_params->max_transfers > 0 && this->queue.init() >= 0)
	{
		if (this->executor.init(data_params->max_transfers) >= 0)
			this->executor_ready = true;
		else
			this->queue.deinit();
	}
}

WFFtpServer::~WFFtpServer()
{
	if (this->executor_ready)
	{
		this->executor.deinit();
		this->queue.deinit();
	}
}

WFConnection *WFFtpServer::new_connection(int accept_fd)
{
	static const char greeting[] = "220 Workflow FTP server ready.\r\n";
	WFConnection *conn = this->WFServer::new_connection(accept_fd);
	__FtpSession *session;
	ssize_t len = sizeof greeting - 1;

	if (conn)
	{
		session = new __FtpSession(accept_fd);
		session->local_addrlen = sizeof session->local_addr;
		if (getsockname(accept_fd, (struct sockaddr *)&session->local_addr,
						&session->local_addrlen) >= 0 &&
			write(accept_fd, greeting, len) == len)
		{
			conn->set_context(session, __ftp_session_deleter);
			return conn;
		}

		delete session;
		this->delete_connection(conn);
	}

	return NULL;
}

int WFFtpServer::create_passive(const struct sockaddr *addr,
								socklen_t addrlen, unsigned short *port)
{
	unsigned int first = this->data_params.port_first;
	unsigned int count = 1;
	unsigned int start = 0;
	struct sockaddr_storage ss;
	socklen_t len;
	unsigned int i;
	int fd;

	if (first != 0 && this->data_params.port_last >= first)
	{
		count = this->data_params.port_last - first + 1;
		start = this->port_next++;
	}

	for (i = 0; i < count; i++)
	{
		memcpy(&ss, addr, addrlen);
		*port = first != 0 ? first + (start + i) % count : 0;
		if (ss.ss_family == AF_INET)
			((struct sockaddr_in *)&ss)->sin_port = htons(*port);
		else
			((struct sockaddr_in6 *)&ss)->sin6_port = htons(*port);

		fd = socket(ss.ss_family, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;

		len = sizeof ss;
		if (bind(fd, (struct sockaddr *)&ss, addrlen) >= 0 &&
			listen(fd, 1) >= 0 && __set_fd_nonblock(fd) >= 0 &&
			getsockname(fd, (struct sockaddr *)&ss, &len) >= 0)
		{
			if (ss.ss_family == AF_INET)
				*port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
			else
				*port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);

			return fd;
		}

		if (errno != EADDRINUSE)
			i = count;

		close(fd);
	}

	return -1;
}

void WFFtpServer::process_transfer(WFFtpServerTask *task,
								   const std::string& cmd,
								   const std::string& path)
{
	protocol::FtpResponse *resp = task->get_resp();
	auto *session = (__FtpSession *)task->get_connection()->get_context();
	socklen_t addrlen = sizeof (struct sockaddr_storage);
	const char *preliminary;
	__FtpTransfer *t;
	struct stat st;
	WFGoTask *go;
	size_t len;
	int flags;

	if (session->pasv_fd < 0 || !this->executor_ready)
	{
		resp->set_reply(425, "Use PASV or EPSV first.");
		return;
	}

	t = new __FtpTransfer;
	t->listen_fd = session->pasv_fd;
	t->timeout = this->data_params.timeout;
	t->path = path;
	session->pasv_fd = -1;
	task->get_peer_addr((struct sockaddr *)&t->peer, &addrlen);
	if (cmd == "RETR")
	{
		t->type = FTP_TRANSFER_RETR;
		t->file_fd = open(path.c_str(), O_RDONLY);
		if (t->file_fd < 0 || fstat(t->file_fd, &st) < 0 ||
			!S_ISREG(st.st_mode))
		{
			resp->set_reply(550, "Failed to open file.");
			delete t;
			return;
		}

		preliminary = "150 Opening data connection.\r\n";
	}
	else if (cmd == "STOR" || cmd == "APPE")
	{
		t->type = FTP_TRANSFER_STOR;
		flags = O_WRONLY | O_CREAT | (cmd == "STOR" ? O_TRUNC : O_APPEND);
		t->file_fd = open(path.c_str(), flags, 0644);
		if (t->file_fd < 0)
		{
			resp->set_reply(553, "Could not create file.");
			delete t;
			return;
		}

		preliminary = "150 Ok to send data.\r\n";
	}
	else
	{
		t->type = (cmd == "NLST") ? FTP_TRANSFER_NLST : FTP_TRANSFER_LIST;
		if (stat(path.c_str(), &st) < 0)
		{
			resp->set_reply(550, "No such file or directory.");
			delete t;
			return;
		}

		preliminary = "150 Here comes the directory listing.\r\n";
	}

	len = strlen(preliminary);
	if (task->push(preliminary, len) != (int)len)
	{
		resp->set_reply(425, "Can't open data connection.");
		delete t;
		return;
	}

	go = WFTaskFactory::create_go_task(&this->queue, &this->executor,
									   __ftp_transfer, t);
	go->set_callback([task, t](WFGoTask *) {
		task->get_resp()->set_reply(t->code, std::move(t->text));
		delete t;
	});
	series_of(task)->push_back(go);
}

void WFFtpServer::process(WFFtpServerTask *task)
{
	protocol::FtpResponse *resp = task->get_resp();
	auto *session = (__FtpSession *)task->get_connection()->get_context();
	std::string rename_from = std::move(session->rename_from);
	const std::string& arg = task->get_req()->get_argument();
	std::string cmd = task->get_req()->get_command();
	const struct sockaddr *local;
	const struct sockaddr_in *sin;
	const struct sockaddr_in6 *sin6;
	std::string vpath;
	std::string path;
	unsigned short port;
	const unsigned char *ip;
	char buf[128];
	struct stat st;
	struct tm tm;

	session->rename_from.clear();
	for (char& c : cmd)
		c = toupper((unsigned char)c);

	if (cmd == "USER")
	{
		session->user = arg;
		session->logged_in = false;
		resp->set_reply(331, "Please specify the password.");
	}
	else if (cmd == "PASS")
	{
		if (session->user.empty())
			resp->set_reply(503, "Login with USER first.");
		else if (this->auth && this->auth(session->user, arg))
		{
			session->logged_in = true;
			session->cwd = "/";
			resp->set_reply(230, "Login successful.");
		}
		else
		{
			resp->set_reply(530, "Login incorrect.");
			if (++session->failures >= FTP_LOGIN_ATTEMPTS)
			{
				session->closing = true;
				task->set_keep_alive(0);
			}
		}
	}
	else if (cmd == "QUIT")
	{
		resp->set_reply(221, "Goodbye.");
		session->closing = true;
		task->set_keep_alive(0);
	}
	else if (cmd == "FEAT")
	{
		resp->set_reply(211, "Features:\n EPSV\n PASV\n SIZE\n MDTM\n UTF8\n"
							 "End");
	}
	else if (cmd == "SYST")
		resp->set_reply(215, "UNIX Type: L8");
	else if (cmd == "NOOP")
		resp->set_reply(200, "NOOP ok.");
	else if (cmd == "OPTS")
	{
		if (strcasecmp(arg.c_str(), "UTF8 ON") == 0)
			resp->set_reply(200, "Always in UTF8 mode.");
		else
			resp->set_reply(501, "Option not understood.");
	}
	else if (cmd == "PORT" || cmd == "EPRT" || cmd == "REST" ||
			 cmd == "MLSD" || cmd == "MLST" || cmd == "STAT" ||
			 cmd == "SITE" || cmd == "AUTH" || cmd == "PBSZ" ||
			 cmd == "PROT" || cmd == "STOU" || cmd == "ACCT" ||
			 cmd == "REIN" || cmd == "SMNT" || cmd == "HELP")
		resp->set_reply(502, "Command not implemented.");
	else if (!session->logged_in)
		resp->set_reply(530, "Please login with USER and PASS.");
	else if (cmd == "PWD" || cmd == "XPWD")
	{
		resp->set_reply(257, __quote_path(session->cwd) +
							 " is the current directory.");
	}
	else if (cmd == "TYPE")
	{
		if (strcasecmp(arg.c_str(), "I") == 0 ||
			strcasecmp(arg.c_str(), "L 8") == 0)
			resp->set_reply(200, "Switching to Binary mode.");
		else if (strcasecmp(arg.c_str(), "A") == 0 ||
				 strcasecmp(arg.c_str(), "A N") == 0)
			resp->set_reply(200, "Switching to ASCII mode.");
		else
			resp->set_reply(504, "Unsupported type.");
	}
	else if (cmd == "MODE" || cmd == "STRU")
	{
		if (strcasecmp(arg.c_str(), cmd == "MODE" ? "S" : "F") == 0)
			resp->set_reply(200, "Ok.");
		else
			resp->set_reply(504, "Unsupported parameter.");
	}
	else if (cmd == "PASV" || cmd == "EPSV")
	{
		if (session->pasv_fd >= 0)
		{
			close(session->pasv_fd);
			session->pasv_fd = -1;
		}

		local = (const struct sockaddr *)&session->local_addr;
		sin = (const struct sockaddr_in *)&session->local_addr;
		sin6 = (const struct sockaddr_in6 *)&session->local_addr;
		ip = NULL;
		if (local->sa_family == AF_INET)
			ip = (const unsigned char *)&sin->sin_addr;
		else if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
			ip = &sin6->sin6_addr.s6_addr[12];

		if (cmd == "EPSV" && strcasecmp(arg.c_str(), "ALL") == 0)
			resp->set_reply(200, "EPSV ALL ok.");
		else if (cmd == "PASV" && !ip)
			resp->set_reply(425, "Use EPSV for IPv6.");
		else if (cmd == "EPSV" && !arg.empty() && arg != (ip ? "1" : "2"))
			resp->set_reply(522, ip ? "Use (1)." : "Use (2).");
		else
		{
			session->pasv_fd = this->create_passive(local,
													session->local_addrlen,
													&port);
			if (session->pasv_fd < 0)
				resp->set_reply(425, "Can't open passive connection.");
			else if (cmd == "EPSV")
			{
				snprintf(buf, sizeof buf,
						 "Entering Extended Passive Mode (|||%u|).", port);
				resp->set_reply(229, buf);
			}
			else
			{
				snprintf(buf, sizeof buf,
						 "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
						 ip[0], ip[1], ip[2], ip[3], port >> 8, port & 255);
				resp->set_reply(227, buf);
			}
		}
	}
	else if (cmd == "LIST" || cmd == "NLST" || cmd == "CWD" ||
			 cmd == "CDUP" || cmd == "XCUP" || cmd == "MKD" ||
			 cmd == "XMKD" || cmd == "RMD" || cmd == "XRMD" ||
			 cmd == "DELE" || cmd == "RNFR" || cmd == "RNTO" ||
			 cmd == "SIZE" || cmd == "MDTM" || cmd == "RETR" ||
			 cmd == "STOR" || cmd == "APPE")
	{
		if (cmd == "LIST" || cmd == "NLST")
			vpath = __virtual_path(session->cwd, __list_argument(arg));
		else if (cmd == "CDUP" || cmd == "XCUP")
			vpath = __virtual_path(session->cwd, "..");
		else if (arg.empty())
		{
			resp->set_reply(501, "Syntax error in parameters or arguments.");
			return;
		}
		else
			vpath = __virtual_path(session->cwd, arg);

		path = this->root + vpath;
		if (!__in_root(this->root, path))
			resp->set_reply(550, "Permission denied.");
		else if (cmd == "LIST" || cmd == "NLST" || cmd == "RETR" ||
				 cmd == "STOR" || cmd == "APPE")
			this->process_transfer(task, cmd, path);
		else if (cmd == "CWD" || cmd == "CDUP" || cmd == "XCUP")
		{
			if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
			{
				session->cwd = vpath;
				resp->set_reply(250, "Directory successfully changed.");
			}
			else
				resp->set_reply(550, "Failed to change directory.");
		}
		else if (cmd == "MKD" || cmd == "XMKD")
		{
			if (mkdir(path.c_str(), 0755) == 0)
				resp->set_reply(257, __quote_path(vpath) + " created.");
			else
				resp->set_reply(550, "Create directory operation failed.");
		}
		else if (vpath == "/" && cmd != "SIZE" && cmd != "MDTM")
			resp->set_reply(550, "Permission denied.");
		else if (cmd == "RMD" || cmd == "XRMD")
		{
			if (rmdir(path.c_str()) == 0)
				resp->set_reply(250, "Remove directory operation successful.");
			else
				resp->set_reply(550, "Remove directory operation failed.");
		}
		else if (cmd == "DELE")
		{
			if (unlink(path.c_str()) == 0)
				resp->set_reply(250, "Delete operation successful.");
			else
				resp->set_reply(550, "Delete operation failed.");
		}
		else if (cmd == "RNFR")
		{
			if (lstat(path.c_str(), &st) == 0)
			{
				session->rename_from = path;
				resp->set_reply(350, "Ready for RNTO.");
			}
			else
				resp->set_reply(550, "RNFR command failed.");
		}
		else if (cmd == "RNTO")
		{
			if (rename_from.empty())
				resp->set_reply(503, "RNFR required first.");
			else if (rename(rename_from.c_str(), path.c_str()) == 0)
				resp->set_reply(250, "Rename successful.");
			else
				resp->set_reply(550, "Rename failed.");
		}
		else if (stat(path.c_str(), &st) < 0 || !S_ISREG(st.st_mode))
			resp->set_reply(550, "Could not get file status.");
		else if (cmd == "SIZE")
			resp->set_reply(213, std::to_string((long long)st.st_size));
		else
		{
			gmtime_r(&st.st_mtime, &tm);
			strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
			resp->set_reply(213, buf);
		}
	}
	else if (cmd == "ABOR")
		resp->set_reply(225, "No transfer to ABOR.");
	else if (cmd == "ALLO")
		resp->set_reply(202, "ALLO command ignored.");
	else
		resp->set_reply(500, "Unknown command.");
}

