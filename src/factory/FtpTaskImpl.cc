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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <utility>
#include "PackageWrapper.h"
#include "WFTaskError.h"
#include "WFTaskFactory.h"
#include "StringUtil.h"
#include "FtpTaskImpl.inl"

using namespace protocol;

#define FTP_KEEPALIVE_DEFAULT	(60 * 1000)

/**********Client**********/

class ComplexFtpTask : public WFComplexClientTask<FtpRequest, FtpResponse>
{
public:
	using prepare_t = std::function<void (const struct sockaddr *, socklen_t)>;
	using start_t = std::function<void ()>;

	void set_session(const std::string& session) { session_ = session; }

	void set_transfer(prepare_t prepare, start_t start)
	{
		prepare_ = std::move(prepare);
		start_ = std::move(start);
		is_transfer_ = true;
	}

	void set_rename(const std::string& to)
	{
		rename_to_ = to;
		is_rename_ = true;
	}

protected:
	virtual bool init_success();
	virtual bool check_request();
	virtual CommMessageOut *message_out();
	virtual CommMessageIn *message_in();
	virtual int keep_alive_timeout();
	virtual int first_timeout();
	virtual bool finish_once();

private:
	enum ConnState
	{
		ST_USER_REQUEST,
		ST_PASS_REQUEST,
		ST_TYPE_REQUEST,
		ST_READY
	};

	struct FtpConnection
	{
		enum ConnState state;
		bool epsv_disabled;
	};

	enum Round
	{
		ROUND_NONE,
		ROUND_GREETING,
		ROUND_LOGIN,
		ROUND_USER
	};

	/* What the reply being received in the user round is for. */
	enum Phase
	{
		PHASE_PASSIVE,
		PHASE_RNFR,
		PHASE_FINAL
	};

	/* Receives the replies of the commands in the user round one by one. */
	class ReplyWrapper : public PackageWrapper
	{
	protected:
		virtual ProtocolMessage *next_in(ProtocolMessage *message)
		{
			return task_->next_reply((FtpResponse *)message);
		}

	protected:
		ComplexFtpTask *task_;

	public:
		ReplyWrapper(ComplexFtpTask *task) : PackageWrapper(NULL)
		{
			task_ = task;
		}

		friend class ComplexFtpTask;
	};

	FtpConnection *get_ftp_connection() const
	{
		WFConnection *conn = this->get_connection();
		return conn ? (FtpConnection *)conn->get_context() : NULL;
	}

	int parse_passive(const FtpResponse *reply);
	int send_command(const std::string& command, const std::string& argument);
	FtpResponse *new_reply();
	ProtocolMessage *next_reply(FtpResponse *reply);

private:
	std::string username_;
	std::string password_;
	std::string session_;
	std::string rename_to_;
	prepare_t prepare_;
	start_t start_;
	struct sockaddr_storage data_addr_;
	socklen_t data_addrlen_;
	FtpResponse reply_;
	ReplyWrapper wrapper_;
	enum Round round_;
	enum Phase phase_;
	bool is_transfer_;
	bool is_rename_;
	bool epsv_;
	bool close_;
	int state_;
	int error_;

public:
	ComplexFtpTask(int retry_max, __ftp_callback_t&& callback) :
		WFComplexClientTask(retry_max, std::move(callback)),
		wrapper_(this)
	{
		data_addrlen_ = 0;
		round_ = ROUND_NONE;
		phase_ = PHASE_FINAL;
		is_transfer_ = false;
		is_rename_ = false;
		epsv_ = false;
		close_ = false;
		state_ = WFT_STATE_SUCCESS;
		error_ = 0;
	}
};

static bool __ftp_line_valid(const std::string& str)
{
	static const std::string forbidden("\r\n\0", 3);

	return str.find_first_of(forbidden) == std::string::npos;
}

bool ComplexFtpTask::check_request()
{
	const std::string& cmd = this->req.get_command();
	const std::string& arg = this->req.get_argument();

	/* Commands sent later in the user round are not encoded by the request,
	 * so check them here. */
	if ((is_transfer_ || is_rename_) &&
		(cmd.empty() || cmd.find(' ') != std::string::npos ||
		 !__ftp_line_valid(cmd) || !__ftp_line_valid(arg) ||
		 !__ftp_line_valid(rename_to_)))
	{
		this->state = WFT_STATE_SYS_ERROR;
		this->error = EBADMSG;
		return false;
	}

	/* Called again when retrying with a new connection. */
	round_ = ROUND_NONE;
	return true;
}

CommMessageOut *ComplexFtpTask::message_out()
{
	FtpConnection *conn;
	FtpRequest *req;

	state_ = WFT_STATE_SUCCESS;
	error_ = 0;
	if (this->get_seq() == 0)
	{
		/* Receive the greeting on a new connection. */
		round_ = ROUND_GREETING;
		return new FtpRequest;
	}

	conn = this->get_ftp_connection();
	if (!conn)
	{
		errno = EBADMSG;
		return NULL;
	}

	if (conn->state != ST_READY)
	{
		req = new FtpRequest;
		if (conn->state == ST_USER_REQUEST)
			req->set_command("USER", username_);
		else if (conn->state == ST_PASS_REQUEST)
			req->set_command("PASS", password_);
		else
			req->set_command("TYPE", "I");

		round_ = ROUND_LOGIN;
		return req;
	}

	/* All the commands of the user round are sent on this connection, so
	 * no other task can use the connection in between. */
	round_ = ROUND_USER;
	close_ = false;
	if (is_transfer_)
	{
		epsv_ = !conn->epsv_disabled;
		phase_ = PHASE_PASSIVE;
		req = new FtpRequest;
		req->set_command(epsv_ ? "EPSV" : "PASV");
		return req;
	}

	if (is_rename_)
		phase_ = PHASE_RNFR;
	else
	{
		/* The user command may be not idempotent. */
		phase_ = PHASE_FINAL;
		this->disable_retry();
	}

	return this->WFComplexClientTask::message_out();
}

FtpResponse *ComplexFtpTask::new_reply()
{
	reply_ = FtpResponse();
	reply_.set_size_limit(this->resp.get_size_limit());
	return &reply_;
}

CommMessageIn *ComplexFtpTask::message_in()
{
	FtpResponse *resp;

	if (round_ == ROUND_USER)
	{
		if (phase_ == PHASE_FINAL)
			wrapper_.set_message(&this->resp);
		else
			wrapper_.set_message(this->new_reply());

		return &wrapper_;
	}

	resp = new FtpResponse;
	resp->set_size_limit(this->resp.get_size_limit());
	return resp;
}

int ComplexFtpTask::parse_passive(const FtpResponse *reply)
{
	const char *text = reply->get_reply_text().c_str();
	const struct sockaddr *addr;
	socklen_t addrlen;
	unsigned long port;
	unsigned int h[4];
	unsigned int p[2];
	const char *pos;
	char *end;

	if (epsv_)
	{
		/* 229 Entering Extended Passive Mode (|||port|) */
		pos = strchr(text, '(');
		if (reply->get_reply_code() != 229 || !pos || !pos[1] ||
			pos[2] != pos[1] || pos[3] != pos[1])
		{
			return -1;
		}

		port = strtoul(pos + 4, &end, 10);
		if (end == pos + 4 || *end != pos[1])
			return -1;
	}
	else
	{
		/* 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
		if (reply->get_reply_code() != 227)
			return -1;

		pos = text;
		while (*pos && (*pos < '0' || *pos > '9'))
			pos++;

		if (sscanf(pos, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3],
				   &p[0], &p[1]) != 6 || p[0] > 255 || p[1] > 255)
		{
			return -1;
		}

		/* The address in the reply may be a private one behind NAT, so
		 * always use the address of the control connection. */
		port = p[0] * 256 + p[1];
	}

	if (port == 0 || port > 65535)
		return -1;

	this->get_target()->get_addr(&addr, &addrlen);
	if (addrlen > sizeof data_addr_)
		return -1;

	memcpy(&data_addr_, addr, addrlen);
	data_addrlen_ = addrlen;
	if (addr->sa_family == AF_INET)
		((struct sockaddr_in *)&data_addr_)->sin_port = htons(port);
	else if (addr->sa_family == AF_INET6)
		((struct sockaddr_in6 *)&data_addr_)->sin6_port = htons(port);
	else
		return -1;

	return 0;
}

int ComplexFtpTask::send_command(const std::string& command,
								 const std::string& argument)
{
	std::string line = command;
	int ret;

	if (!argument.empty())
		line.append(" ").append(argument);

	line.append("\r\n");
	ret = wrapper_.feedback(line.data(), line.size());
	if (ret == (int)line.size())
		return 0;

	/* Rarely happens because the line is short and nothing else is being
	 * sent. The state of the session is unknown then. */
	state_ = WFT_STATE_SYS_ERROR;
	error_ = ret < 0 ? errno : EAGAIN;
	close_ = true;
	return -1;
}

/* Called in a network thread when a reply of the user round is received.
 * Returns the message for the next reply, or NULL if the round finishes. */
ProtocolMessage *ComplexFtpTask::next_reply(FtpResponse *reply)
{
	FtpConnection *conn;
	int code;

	if (phase_ == PHASE_FINAL)
		return NULL;

	code = reply->get_reply_code();
	if (phase_ == PHASE_RNFR)
	{
		if (code == 350)
		{
			this->disable_retry();
			if (this->send_command("RNTO", rename_to_) == 0)
			{
				phase_ = PHASE_FINAL;
				return &this->resp;
			}
		}

		/* The reply of 'RNFR' is the result if it failed. */
		this->resp = std::move(*reply);
		return NULL;
	}

	if (this->parse_passive(reply) == 0)
	{
		/* The transfer command may be not idempotent. */
		this->disable_retry();
		if (this->send_command(this->req.get_command(),
							   this->req.get_argument()) < 0)
		{
			return NULL;
		}

		/* The command is sent before connecting, so the server always gets
		 * the command first. */
		prepare_((struct sockaddr *)&data_addr_, data_addrlen_);
		start_();
		phase_ = PHASE_FINAL;
		return &this->resp;
	}

	conn = this->get_ftp_connection();
	if (conn && epsv_ && code / 100 == 5)
	{
		/* EPSV is not supported. Use PASV instead. */
		conn->epsv_disabled = true;
		epsv_ = false;
		if (this->send_command("PASV", "") == 0)
			return this->new_reply();

		return NULL;
	}

	state_ = WFT_STATE_TASK_ERROR;
	error_ = WFT_ERR_FTP_PASSIVE_FAILED;
	this->resp = std::move(*reply);
	return NULL;
}

int ComplexFtpTask::keep_alive_timeout()
{
	FtpConnection *conn;
	FtpResponse *resp;
	int code;

	if (round_ == ROUND_USER)
	{
		/* Without a preliminary reply, the server will not use the data
		 * connection. Close the control connection to make the server
		 * release the passive port, so the data connection terminates. */
		if (close_ || (is_transfer_ && phase_ == PHASE_FINAL &&
					   this->resp.get_preliminary_code() == 0))
		{
			return 0;
		}

		return this->keep_alive_timeo;
	}

	resp = (FtpResponse *)this->get_message_in();
	code = resp->get_reply_code();
	switch (round_)
	{
	case ROUND_GREETING:
		if (code / 100 != 2)
		{
			error_ = WFT_ERR_FTP_SERVICE_UNAVAILABLE;
			break;
		}

		conn = new FtpConnection;
		conn->state = ST_USER_REQUEST;
		conn->epsv_disabled = false;
		this->get_connection()->set_context(conn, [](void *ctx) {
			delete (FtpConnection *)ctx;
		});
		return FTP_KEEPALIVE_DEFAULT;

	case ROUND_LOGIN:
		conn = this->get_ftp_connection();
		if (conn->state == ST_USER_REQUEST && code == 331)
		{
			conn->state = ST_PASS_REQUEST;
			return FTP_KEEPALIVE_DEFAULT;
		}

		if (code / 100 == 2)
		{
			/* 230 for 'USER' means that no password is needed. */
			if (conn->state == ST_TYPE_REQUEST)
				conn->state = ST_READY;
			else
				conn->state = ST_TYPE_REQUEST;

			return FTP_KEEPALIVE_DEFAULT;
		}

		if (conn->state == ST_TYPE_REQUEST)
			error_ = WFT_ERR_FTP_TYPE_FAILED;
		else
			error_ = WFT_ERR_FTP_LOGIN_FAILED;

		break;

	default:
		return 0;
	}

	state_ = WFT_STATE_TASK_ERROR;
	return 0;
}

int ComplexFtpTask::first_timeout()
{
	if (round_ != ROUND_USER)
		return 0;

	/* After the preliminary reply of a transfer, the completion reply comes
	 * when the data connection is finished. Wait for it as long as the
	 * receive timeout, which is unlimited by default. */
	if (is_transfer_ && this->resp.get_preliminary_code() != 0)
		return this->receive_timeo > 0 ? this->receive_timeo : -1;

	return this->watch_timeo;
}

bool ComplexFtpTask::init_success()
{
	std::string info;

	if (!uri_.scheme || strcasecmp(uri_.scheme, "ftp") != 0)
	{
		this->state = WFT_STATE_TASK_ERROR;
		this->error = WFT_ERR_URI_SCHEME_INVALID;
		return false;
	}

	username_ = "anonymous";
	password_ = "anonymous@";
	if (uri_.userinfo)
	{
		const char *colon = strchr(uri_.userinfo, ':');

		if (colon)
		{
			username_.assign(uri_.userinfo, colon - uri_.userinfo);
			password_.assign(colon + 1);
			StringUtil::url_decode(password_);
		}
		else
		{
			username_.assign(uri_.userinfo);
			password_.clear();
		}

		StringUtil::url_decode(username_);
	}

	info = "ftp|user:" + username_ + "|pass:" + password_;
	if (!session_.empty())
	{
		info += "|session:" + session_;
		this->set_fixed_addr(true);
		this->set_fixed_conn(true);
	}

	this->WFComplexClientTask::set_transport_type(TT_TCP);
	this->WFComplexClientTask::set_info(info);
	return true;
}

bool ComplexFtpTask::finish_once()
{
	FtpResponse *resp;

	if (round_ == ROUND_USER)
	{
		if (this->state == WFT_STATE_SUCCESS && state_ != WFT_STATE_SUCCESS)
		{
			this->state = state_;
			this->error = error_;
		}

		/* 'EPSV' or 'PASV' of a transfer. */
		if (is_transfer_)
			delete this->get_message_out();

		return true;
	}

	resp = (FtpResponse *)this->get_message_in();
	if (this->state == WFT_STATE_SUCCESS && state_ != WFT_STATE_SUCCESS)
	{
		/* Return the negative reply to the user. */
		this->state = state_;
		this->error = error_;
		this->resp = std::move(*resp);
		this->disable_retry();
	}

	delete this->get_message_out();
	delete resp;
	return false;
}

/* Passes every piece of the received data to 'extract' without keeping it. */
class FtpDataStream : public FtpDataResponse
{
public:
	std::function<void (const void *, size_t)> extract;

protected:
	virtual int append(const void *buf, size_t *size)
	{
		this->extract(buf, *size);
		return 0;
	}
};

class ComplexFtpDataTask : public WFComplexClientTask<FtpDataRequest,
													  FtpDataResponse>
{
public:
	void set_extract(std::function<void (const void *, size_t)> extract)
	{
		stream_.extract = std::move(extract);
	}

protected:
	virtual CommMessageIn *message_in();
	virtual int first_timeout();
	virtual bool finish_once();

private:
	FtpDataStream stream_;
	bool upload_;
	bool sent_;

public:
	ComplexFtpDataTask(bool upload, __ftp_data_callback_t&& callback) :
		WFComplexClientTask(0, std::move(callback))
	{
		upload_ = upload;
		sent_ = false;
	}
};

CommMessageIn *ComplexFtpDataTask::message_in()
{
	if (stream_.extract)
		return &stream_;

	return this->WFComplexClientTask::message_in();
}

int ComplexFtpDataTask::first_timeout()
{
	if (!upload_)
		return 0;

	/* All the data is sent and nothing will be received. Close the
	 * connection very soon to tell the server that the file ends. */
	sent_ = true;
	return 1;
}

bool ComplexFtpDataTask::finish_once()
{
	if (this->state == WFT_STATE_SYS_ERROR &&
		(upload_ ? sent_ && this->error == ETIMEDOUT :
				   this->error == ECONNRESET))
	{
		/* Closing the connection is the end of the data. */
		this->state = WFT_STATE_SUCCESS;
		this->error = 0;
		this->timeout_reason = TOR_NOT_TIMEOUT;
	}

	return true;
}

/**********Client Factory**********/

__WFFtpTask *__WFFtpTaskFactory::create_ftp_task(const ParsedURI& uri,
												 const std::string& session,
												 int retry_max,
												 __ftp_callback_t callback)
{
	auto *task = new ComplexFtpTask(retry_max, std::move(callback));

	task->set_session(session);
	task->init(uri);
	task->set_keep_alive(FTP_KEEPALIVE_DEFAULT);
	return task;
}

__WFFtpTask *__WFFtpTaskFactory::create_rename_task(const ParsedURI& uri,
													const std::string& session,
													int retry_max,
													const std::string& to,
													__ftp_callback_t callback)
{
	auto *task = new ComplexFtpTask(retry_max, std::move(callback));

	task->set_session(session);
	task->set_rename(to);
	task->init(uri);
	task->set_keep_alive(FTP_KEEPALIVE_DEFAULT);
	return task;
}

__WFFtpTask *
__WFFtpTaskFactory::create_transfer_task(const ParsedURI& uri,
										 const std::string& session,
										 int retry_max,
										 prepare_t prepare,
										 start_t start,
										 __ftp_callback_t callback)
{
	auto *task = new ComplexFtpTask(retry_max, std::move(callback));

	task->set_session(session);
	task->set_transfer(std::move(prepare), std::move(start));
	task->init(uri);
	task->set_keep_alive(FTP_KEEPALIVE_DEFAULT);
	return task;
}

__WFFtpDataTask *
__WFFtpTaskFactory::create_data_task(const struct sockaddr *addr,
									 socklen_t addrlen,
									 bool upload,
									 extract_t extract,
									 __ftp_data_callback_t callback)
{
	auto *task = new ComplexFtpDataTask(upload, std::move(callback));

	if (extract)
		task->set_extract(std::move(extract));

	task->init(TT_TCP, addr, addrlen, "");
	return task;
}
