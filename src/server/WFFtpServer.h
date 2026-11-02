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

#ifndef _WFFTPSERVER_H_
#define _WFFTPSERVER_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <stddef.h>
#include <atomic>
#include <string>
#include <functional>
#include "FtpMessage.h"
#include "Executor.h"
#include "WFServer.h"
#include "WFTaskFactory.h"

using WFFtpServerTask = WFNetworkTask<protocol::FtpRequest,
									  protocol::FtpResponse>;
using ftp_auth_t = std::function<bool (const std::string& user,
									   const std::string& password)>;

static constexpr struct WFServerParams FTP_SERVER_PARAMS_DEFAULT =
{
	.transport_type			=	TT_TCP,
	.max_connections		=	2000,
	.peer_response_timeout	=	10 * 1000,
	.receive_timeout		=	-1,
	.keep_alive_timeout		=	300 * 1000,
	.request_size_limit		=	8 * 1024,
	.ssl_accept_timeout		=	10 * 1000,
};

struct WFFtpDataParams
{
	size_t max_transfers;	/* number of threads running the transfers */
	int timeout;			/* of accepting, and each read or write */
	unsigned short port_first;	/* range of passive ports, 0 for any */
	unsigned short port_last;
};

static constexpr struct WFFtpDataParams FTP_DATA_PARAMS_DEFAULT =
{
	.max_transfers	=	16,
	.timeout		=	30 * 1000,
	.port_first		=	0,
	.port_last		=	0,
};

/* A FTP server of a local directory, which is '/' of all users. Only
 * passive mode (PASV/EPSV) is supported. Transfers run in a private thread
 * pool, and the control connection is closed if a command arrives before
 * the reply of the previous one, such as 'ABOR' during a transfer. */
class WFFtpServer : public WFServer<protocol::FtpRequest,
									protocol::FtpResponse>
{
public:
	/* 'auth' is called in a network thread, so do not block in it. */
	WFFtpServer(const std::string& root, ftp_auth_t auth) :
		WFFtpServer(&FTP_SERVER_PARAMS_DEFAULT, &FTP_DATA_PARAMS_DEFAULT,
					root, std::move(auth))
	{
	}

	WFFtpServer(const struct WFServerParams *params,
				const struct WFFtpDataParams *data_params,
				const std::string& root, ftp_auth_t auth);

	virtual ~WFFtpServer();

protected:
	virtual WFConnection *new_connection(int accept_fd);

private:
	void process(WFFtpServerTask *task);
	void process_transfer(WFFtpServerTask *task, const std::string& cmd,
						  const std::string& path);
	int create_passive(const struct sockaddr *addr, socklen_t addrlen,
					   unsigned short *port);

private:
	std::string root;
	ftp_auth_t auth;
	struct WFFtpDataParams data_params;
	std::atomic<unsigned int> port_next;
	ExecQueue queue;
	Executor executor;
	bool executor_ready;
};

#endif

