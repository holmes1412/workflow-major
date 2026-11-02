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

#ifndef _WFFTPCLIENT_H_
#define _WFFTPCLIENT_H_

#include <stddef.h>
#include <string>
#include <utility>
#include <functional>
#include "FtpMessage.h"
#include "WFTask.h"

class WFFtpTask;
using ftp_callback_t = std::function<void (WFFtpTask *)>;
using ftp_extract_t = std::function<void (WFFtpTask *, const void *, size_t)>;

/* The result of a task is the final reply in the response. A negative reply
 * like '550' is still a success of the task, so check the reply code. Data
 * of LIST/NLST/MLSD/RETR is in the response too. */
class WFFtpTask : public WFGenericTask
{
public:
	protocol::FtpRequest *get_req() { return &this->req; }
	protocol::FtpResponse *get_resp() { return &this->resp; }

	void set_callback(ftp_callback_t callback)
	{
		this->callback = std::move(callback);
	}

	/* Receive the data of LIST/NLST/MLSD/RETR piece by piece instead of
	 * keeping it in the response, and the size limit does not apply. It is
	 * called in a network thread, so do not block in it. */
	void set_extract(ftp_extract_t extract)
	{
		this->extract = std::move(extract);
	}

	/* In milliseconds. Limits the waiting for the completion reply after a
	 * transfer starts, which is as long as the transfer. -1 by default for
	 * unlimited, and a stalled data connection is still detected by the
	 * response timeout of each read. */
	void set_transfer_timeout(int timeout)
	{
		this->transfer_timeout = timeout;
	}

protected:
	protocol::FtpRequest req;
	protocol::FtpResponse resp;
	ftp_callback_t callback;
	ftp_extract_t extract;
	int transfer_timeout;

protected:
	WFFtpTask(ftp_callback_t&& callback) :
		callback(std::move(callback))
	{
		this->transfer_timeout = -1;
	}

	virtual ~WFFtpTask() { }
};

class __WFFtpServer;

/* Tasks of a client run one after another on a dedicated control connection,
 * which is logged in once and kept alive. Like WFMySQLConnection, clients
 * with the same id and url share the connection, so use a unique id for each
 * client. Transfers use passive mode and binary type. */
class WFFtpClient
{
public:
	/* url: ftp://user:password@host:port, and port 21 by default. User
	 * 'anonymous' is used if no userinfo. Returns -1 if url is invalid. */
	int init(const std::string& url);
	void deinit() { }

public:
	/* Commands that change the session or transfer, like USER/PASV/RETR,
	 * are disallowed. Working directory set by 'CWD' will be lost if the
	 * connection is re-established, so absolute path is recommended. */
	WFFtpTask *create_command_task(const std::string& command,
								   const std::string& argument,
								   ftp_callback_t callback);

	/* 'LIST' by default. Command can be changed to 'NLST' or 'MLSD' by
	 * task->get_req()->set_command() before the task starts. */
	WFFtpTask *create_list_task(const std::string& path,
								ftp_callback_t callback);

	/* 'RETR'. Data is in task->get_resp()->get_data(), or extracted by
	 * task->set_extract(). */
	WFFtpTask *create_get_task(const std::string& path,
							   ftp_callback_t callback);

	/* 'STOR', or 'APPE' by task->get_req()->set_command(). */
	WFFtpTask *create_put_task(const std::string& path,
							   const void *buf, size_t size,
							   ftp_callback_t callback);

	/* 'RNFR' and 'RNTO'. The reply is of 'RNFR' if it failed. */
	WFFtpTask *create_rename_task(const std::string& from,
								  const std::string& to,
								  ftp_callback_t callback);

	/* 'QUIT' and close the connection. */
	WFFtpTask *create_quit_task(ftp_callback_t callback);

private:
	__WFFtpServer *server;

public:
	WFFtpClient(int id);
	~WFFtpClient();

	WFFtpClient(const WFFtpClient&) = delete;
	WFFtpClient& operator = (const WFFtpClient&) = delete;
};

/* Tasks created by url, such as ftp://user:password@host:port/dir/file. As
 * RFC 1738, the path is relative to the login directory, and a path like
 * '/%2Fpub/file' is absolute. Tasks of the same server and user share the
 * logged in control connections, and at most 'max_connections' of them run
 * at a time. */
class WFFtpTaskFactory
{
public:
	/* 'RETR' the file. */
	static WFFtpTask *create_get_task(const std::string& url,
									  ftp_callback_t callback);

	/* 'LIST' the directory, or 'NLST'/'MLSD' by set_command(). */
	static WFFtpTask *create_list_task(const std::string& url,
									   ftp_callback_t callback);

	/* 'STOR' the file, or 'APPE' by set_command(). */
	static WFFtpTask *create_put_task(const std::string& url,
									  const void *buf, size_t size,
									  ftp_callback_t callback);

	/* Rename the file to 'to', which is a path but not url. */
	static WFFtpTask *create_rename_task(const std::string& url,
										 const std::string& to,
										 ftp_callback_t callback);

	/* A command to the server of url, and the path of url is not used. */
	static WFFtpTask *create_command_task(const std::string& url,
										  const std::string& command,
										  const std::string& argument,
										  ftp_callback_t callback);

	/* 4 by default. Takes effect on the servers first used after it is
	 * called. */
	static void set_max_connections(size_t max);
};

#endif

