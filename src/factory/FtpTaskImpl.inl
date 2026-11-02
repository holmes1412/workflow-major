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

#include <sys/socket.h>
#include <string>
#include <functional>
#include "FtpMessage.h"
#include "WFTaskFactory.h"

// Internal, for WFFtpClient only.

using __WFFtpTask = WFNetworkTask<protocol::FtpRequest,
								  protocol::FtpResponse>;
using __ftp_callback_t = std::function<void (__WFFtpTask *)>;

using __WFFtpDataTask = WFNetworkTask<protocol::FtpDataRequest,
									  protocol::FtpDataResponse>;
using __ftp_data_callback_t = std::function<void (__WFFtpDataTask *)>;

class __WFFtpTaskFactory
{
private:
	using prepare_t = std::function<void (const struct sockaddr *, socklen_t)>;
	using start_t = std::function<void ()>;
	using extract_t = std::function<void (const void *, size_t)>;

public:
	/* A task on the control connection that logs in on a new connection and
	 * sends the command in the request. A non-empty 'session' makes all the
	 * tasks of the same session use one dedicated connection. */
	static __WFFtpTask *create_ftp_task(const ParsedURI& uri,
										const std::string& session,
										int retry_max,
										__ftp_callback_t callback);

	/* 'RNFR' in the request, and then 'RNTO to' if the server accepts it.
	 * The reply is of 'RNFR' if it failed. */
	static __WFFtpTask *create_rename_task(const ParsedURI& uri,
										   const std::string& session,
										   int retry_max,
										   const std::string& to,
										   __ftp_callback_t callback);

	/* A task for a transfer command in the request, such as 'RETR'. The
	 * passive address is requested first, and the command is sent right
	 * after the passive reply, on the same connection. Then 'prepare' is
	 * called with the address of the data connection, and 'start' is called
	 * to start it. Both are called in a network thread. The receive timeout
	 * of the task limits the waiting for the completion reply. */
	static __WFFtpTask *create_transfer_task(const ParsedURI& uri,
											 const std::string& session,
											 int retry_max,
											 prepare_t prepare,
											 start_t start,
											 __ftp_callback_t callback);

	/* A task on the data connection. An upload task sends the request data
	 * and closes the connection. A download task receives the response data
	 * until the server closes the connection. If 'extract' is not empty, the
	 * downloaded data is passed to it piece by piece instead of being kept. */
	static __WFFtpDataTask *create_data_task(const struct sockaddr *addr,
											 socklen_t addrlen,
											 bool upload,
											 extract_t extract,
											 __ftp_data_callback_t callback);
};

