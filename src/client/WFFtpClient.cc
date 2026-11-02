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
#include <strings.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <functional>
#include "URIParser.h"
#include "StringUtil.h"
#include "WFTaskError.h"
#include "WFTaskFactory.h"
#include "WFResourcePool.h"
#include "Workflow.h"
#include "FtpTaskImpl.inl"
#include "WFFtpClient.h"

#define FTP_RETRY_MAX			1
#define FTP_MAX_CONNECTIONS		4

/* A dedicated control connection, identified by the session id. */
struct __WFFtpSession
{
	std::string id;
};

/* A server and user with its sessions. A task takes a session from the pool,
 * so that only one task runs on a control connection at a time. */
class __WFFtpServer
{
public:
	ParsedURI uri;
	WFResourcePool *pool;

public:
	void incref() { this->ref++; }

	void decref()
	{
		if (--this->ref == 0)
			delete this;
	}

private:
	__WFFtpSession *sessions;
	std::atomic<int> ref;

public:
	__WFFtpServer(const std::string& id, size_t n) : ref(1)
	{
		std::vector<void *> res(n);

		this->sessions = new __WFFtpSession[n];
		for (size_t i = 0; i < n; i++)
		{
			this->sessions[i].id = id + ":" + std::to_string(i);
			res[i] = &this->sessions[i];
		}

		this->pool = new WFResourcePool(res.data(), n);
	}

private:
	~__WFFtpServer()
	{
		delete this->pool;
		delete []this->sessions;
	}
};

/* Servers of the tasks created by url. */
class __WFFtpServerMap
{
public:
	/* Returns the server with a reference. */
	__WFFtpServer *get(const ParsedURI& uri);

	void set_max_connections(size_t max)
	{
		std::lock_guard<std::mutex> lock(this->mutex);
		this->max_connections = max > 0 ? max : 1;
	}

private:
	std::mutex mutex;
	std::unordered_map<std::string, __WFFtpServer *> servers;
	size_t max_connections;
	unsigned long long count;

public:
	__WFFtpServerMap()
	{
		this->max_connections = FTP_MAX_CONNECTIONS;
		this->count = 0;
	}

	~__WFFtpServerMap()
	{
		for (auto& kv : this->servers)
			kv.second->decref();
	}
};

__WFFtpServer *__WFFtpServerMap::get(const ParsedURI& uri)
{
	std::string key = uri.userinfo ? uri.userinfo : "";
	__WFFtpServer *server;

	key += "@";
	key += uri.host;
	key += ":";
	key += uri.port ? uri.port : "21";

	std::lock_guard<std::mutex> lock(this->mutex);
	auto it = this->servers.find(key);

	if (it != this->servers.end())
		server = it->second;
	else
	{
		server = new __WFFtpServer("url" + std::to_string(this->count++),
								   this->max_connections);
		server->uri = uri;
		this->servers.emplace(std::move(key), server);
	}

	server->incref();
	return server;
}

static __WFFtpServerMap __ftp_server_map;

enum FtpOperation
{
	FTP_OP_COMMAND,
	FTP_OP_LIST,
	FTP_OP_RETR,
	FTP_OP_STOR,
	FTP_OP_RENAME,
	FTP_OP_QUIT
};

enum FtpStep
{
	FTP_STEP_BEGIN,
	FTP_STEP_SESSION,
	FTP_STEP_CONTROL,
	FTP_STEP_DATA,
	FTP_STEP_FINISH,
	FTP_STEP_DONE
};

static bool __ftp_command_disallowed(const char *command)
{
	static const char *const disallowed[] = {
		"USER", "PASS", "ACCT", "REIN", "QUIT",
		"PASV", "EPSV", "PORT", "EPRT", "LPSV", "LPRT", "SPSV",
		"RETR", "STOR", "STOU", "APPE", "LIST", "NLST", "MLSD",
		"TYPE", "MODE", "STRU", "REST", "ABOR",
		"AUTH", "PBSZ", "PROT", "CCC", "ADAT", "CONF", "ENC", "MIC"
	};

	for (const char *cmd : disallowed)
	{
		if (strcasecmp(command, cmd) == 0)
			return true;
	}

	return false;
}

class FtpClientTask : public WFFtpTask
{
public:
	/* A NULL server means an invalid url, and the error is already set. */
	FtpClientTask(__WFFtpServer *server, enum FtpOperation op,
				  ftp_callback_t&& callback) :
		WFFtpTask(std::move(callback))
	{
		if (server)
			server->incref();

		this->server = server;
		this->session = NULL;
		this->res = NULL;
		this->op = op;
		this->step = FTP_STEP_BEGIN;
		this->rename_sent = false;
		this->data_task = NULL;
		this->data_series = NULL;
		this->data_cond = NULL;
		this->data_started = false;
		this->data_state = WFT_STATE_UNDEFINED;
		this->data_error = 0;
	}

	virtual ~FtpClientTask()
	{
		if (this->data_series)
			this->data_series->dismiss();

		if (this->server)
			this->server->decref();
	}

	void set_error(int state, int error)
	{
		this->state = state;
		this->error = error;
	}

public:
	std::string upload;
	std::string rename_to;

protected:
	virtual void dispatch();
	virtual SubTask *done();

private:
	bool check_request();
	bool is_transfer() const;
	__WFFtpTask *create_control_task(const std::string& command,
									 const std::string& argument);
	void control_callback(__WFFtpTask *task);
	void prepare_data(const struct sockaddr *addr, socklen_t addrlen);
	void start_data();
	void data_callback(__WFFtpDataTask *task);
	void finish();

private:
	__WFFtpServer *server;
	__WFFtpSession *session;
	void *res;
	enum FtpOperation op;
	enum FtpStep step;
	bool rename_sent;
	__WFFtpDataTask *data_task;
	SeriesWork *data_series;
	WFConditional *data_cond;
	bool data_started;
	int data_state;
	int data_error;
	std::string data;
};

bool FtpClientTask::is_transfer() const
{
	return this->op == FTP_OP_LIST || this->op == FTP_OP_RETR ||
		   this->op == FTP_OP_STOR;
}

bool FtpClientTask::check_request()
{
	const char *cmd = this->req.get_command().c_str();
	bool allowed;

	if (!this->server)
		return false;

	switch (this->op)
	{
	case FTP_OP_COMMAND:
		allowed = *cmd && !__ftp_command_disallowed(cmd);
		break;
	case FTP_OP_LIST:
		allowed = strcasecmp(cmd, "LIST") == 0 ||
				  strcasecmp(cmd, "NLST") == 0 ||
				  strcasecmp(cmd, "MLSD") == 0;
		break;
	case FTP_OP_RETR:
		allowed = strcasecmp(cmd, "RETR") == 0;
		break;
	case FTP_OP_STOR:
		allowed = strcasecmp(cmd, "STOR") == 0 ||
				  strcasecmp(cmd, "APPE") == 0;
		break;
	case FTP_OP_RENAME:
		allowed = strcasecmp(cmd, "RNFR") == 0;
		break;
	default:
		allowed = strcasecmp(cmd, "QUIT") == 0;
		break;
	}

	if (!allowed)
		this->set_error(WFT_STATE_TASK_ERROR, WFT_ERR_FTP_COMMAND_DISALLOWED);

	return allowed;
}

__WFFtpTask *FtpClientTask::create_control_task(const std::string& command,
												const std::string& argument)
{
	auto&& cb = std::bind(&FtpClientTask::control_callback, this,
						  std::placeholders::_1);
	__WFFtpTask *task;

	if (this->is_transfer())
	{
		auto&& prepare = std::bind(&FtpClientTask::prepare_data, this,
								   std::placeholders::_1,
								   std::placeholders::_2);
		auto&& start = std::bind(&FtpClientTask::start_data, this);

		task = __WFFtpTaskFactory::create_transfer_task(this->server->uri,
														this->session->id,
														FTP_RETRY_MAX,
														std::move(prepare),
														std::move(start),
														std::move(cb));
	}
	else
	{
		task = __WFFtpTaskFactory::create_ftp_task(this->server->uri,
												   this->session->id,
												   FTP_RETRY_MAX,
												   std::move(cb));
	}

	task->get_req()->set_command(command, argument);
	task->get_resp()->set_size_limit(this->resp.get_size_limit());
	if (this->op == FTP_OP_QUIT)
		task->set_keep_alive(0);

	return task;
}

void FtpClientTask::control_callback(__WFFtpTask *task)
{
	size_t size_limit = this->resp.get_size_limit();

	this->state = task->get_state();
	this->error = task->get_error();
	this->resp = std::move(*task->get_resp());
	this->resp.set_size_limit(size_limit);
}

void FtpClientTask::prepare_data(const struct sockaddr *addr,
								 socklen_t addrlen)
{
	auto&& cb = std::bind(&FtpClientTask::data_callback, this,
						  std::placeholders::_1);
	std::function<void (const void *, size_t)> extract;
	bool upload = (this->op == FTP_OP_STOR);

	if (this->data_series)
	{
		/* Prepared again by a retry. Take back the data to upload. */
		if (upload)
			this->upload = std::move(*this->data_task->get_req()->get_data());

		this->data_series->dismiss();
	}

	if (!upload && this->extract)
	{
		extract = [this](const void *buf, size_t size) {
			this->extract(this, buf, size);
		};
	}

	this->data_task = __WFFtpTaskFactory::create_data_task(addr, addrlen,
														   upload,
														   std::move(extract),
														   std::move(cb));
	this->data_task->get_resp()->set_size_limit(this->resp.get_size_limit());
	if (upload)
		this->data_task->get_req()->set_data(std::move(this->upload));

	this->data_series = Workflow::create_series_work(this->data_task, nullptr);
}

void FtpClientTask::start_data()
{
	SeriesWork *series = this->data_series;

	this->data_cond = WFTaskFactory::create_conditional(
										WFTaskFactory::create_empty_task());
	this->data_started = true;
	this->data_series = NULL;
	this->data_task = NULL;
	series->start();
}

void FtpClientTask::data_callback(__WFFtpDataTask *task)
{
	this->data_state = task->get_state();
	this->data_error = task->get_error();
	this->data = std::move(*task->get_resp()->get_data());
	this->data_cond->signal(NULL);
}

void FtpClientTask::finish()
{
	if (this->session)
	{
		this->server->pool->post(this->session);
		this->session = NULL;
	}

	if (this->data_started && this->state == WFT_STATE_SUCCESS)
	{
		/* A data connection error is the result if the server thinks the
		 * transfer succeeded. Otherwise the reply tells the failure. */
		if (this->data_state != WFT_STATE_SUCCESS &&
			this->resp.get_reply_code() / 100 == 2)
		{
			this->state = this->data_state;
			this->error = this->data_error;
		}

		this->resp.set_data(std::move(this->data));
	}

	this->step = FTP_STEP_DONE;
}

void FtpClientTask::dispatch()
{
	SeriesWork *series = series_of(this);
	SubTask *task;

	switch (this->step)
	{
	case FTP_STEP_BEGIN:
		if (this->check_request())
		{
			/* Wait for a free session of the server. */
			task = WFTaskFactory::create_empty_task();
			this->step = FTP_STEP_SESSION;
			series->push_front(this);
			series->push_front(this->server->pool->get(task, &this->res));
		}
		else
		{
			/* Call back in another thread, not the one starting it. */
			this->step = FTP_STEP_FINISH;
			series->push_front(this);
			series->push_front(WFTaskFactory::create_timer_task(0, 0, nullptr));
		}

		break;

	case FTP_STEP_SESSION:
		this->session = (__WFFtpSession *)this->res;
		this->step = FTP_STEP_CONTROL;
		series->push_front(this);
		series->push_front(this->create_control_task(this->req.get_command(),
													 this->req.get_argument()));
		break;

	case FTP_STEP_CONTROL:
		if (this->op == FTP_OP_RENAME && !this->rename_sent &&
			this->state == WFT_STATE_SUCCESS &&
			this->resp.get_reply_code() == 350)
		{
			this->rename_sent = true;
			series->push_front(this);
			series->push_front(this->create_control_task("RNTO",
														 this->rename_to));
			break;
		}

		if (this->data_started)
		{
			this->step = FTP_STEP_DATA;
			series->push_front(this);
			series->push_front(this->data_cond);
			break;
		}

		this->finish();
		break;

	default:
		this->finish();
		break;
	}

	this->subtask_done();
}

SubTask *FtpClientTask::done()
{
	SeriesWork *series = series_of(this);

	if (this->step == FTP_STEP_DONE)
	{
		if (this->callback)
			this->callback(this);

		delete this;
	}

	return series->pop();
}

/**********Client**********/

WFFtpClient::WFFtpClient(int id)
{
	this->server = new __WFFtpServer(std::to_string(id), 1);
}

WFFtpClient::~WFFtpClient()
{
	this->server->decref();
}

int WFFtpClient::init(const std::string& url)
{
	ParsedURI uri;

	if (URIParser::parse(url, uri) >= 0)
	{
		if (uri.scheme && strcasecmp(uri.scheme, "ftp") == 0)
		{
			this->server->uri = std::move(uri);
			return 0;
		}

		errno = EINVAL;
		return -1;
	}

	if (uri.state == URI_STATE_INVALID)
		errno = EINVAL;

	return -1;
}

WFFtpTask *WFFtpClient::create_command_task(const std::string& command,
											const std::string& argument,
											ftp_callback_t callback)
{
	auto *task = new FtpClientTask(this->server, FTP_OP_COMMAND,
								   std::move(callback));

	task->get_req()->set_command(command, argument);
	return task;
}

WFFtpTask *WFFtpClient::create_list_task(const std::string& path,
										 ftp_callback_t callback)
{
	auto *task = new FtpClientTask(this->server, FTP_OP_LIST,
								   std::move(callback));

	task->get_req()->set_command("LIST", path);
	return task;
}

WFFtpTask *WFFtpClient::create_get_task(const std::string& path,
										ftp_callback_t callback)
{
	auto *task = new FtpClientTask(this->server, FTP_OP_RETR,
								   std::move(callback));

	task->get_req()->set_command("RETR", path);
	return task;
}

WFFtpTask *WFFtpClient::create_put_task(const std::string& path,
										const void *buf, size_t size,
										ftp_callback_t callback)
{
	auto *task = new FtpClientTask(this->server, FTP_OP_STOR,
								   std::move(callback));

	task->get_req()->set_command("STOR", path);
	if (size > 0)
		task->upload.assign((const char *)buf, size);

	return task;
}

WFFtpTask *WFFtpClient::create_rename_task(const std::string& from,
										   const std::string& to,
										   ftp_callback_t callback)
{
	auto *task = new FtpClientTask(this->server, FTP_OP_RENAME,
								   std::move(callback));

	task->get_req()->set_command("RNFR", from);
	task->rename_to = to;
	return task;
}

WFFtpTask *WFFtpClient::create_quit_task(ftp_callback_t callback)
{
	auto *task = new FtpClientTask(this->server, FTP_OP_QUIT,
								   std::move(callback));

	task->get_req()->set_command("QUIT");
	return task;
}

/**********Task Factory**********/

static FtpClientTask *__create_url_task(const std::string& url,
										enum FtpOperation op,
										ftp_callback_t&& callback,
										std::string& path)
{
	__WFFtpServer *server = NULL;
	FtpClientTask *task;
	ParsedURI uri;
	int state = WFT_STATE_TASK_ERROR;
	int error = WFT_ERR_URI_PARSE_FAILED;

	if (URIParser::parse(url, uri) >= 0)
	{
		if (!uri.scheme || strcasecmp(uri.scheme, "ftp") != 0)
			error = WFT_ERR_URI_SCHEME_INVALID;
		else if (uri.host && *uri.host)
		{
			/* As RFC 1738, the path is relative to the login directory. */
			if (uri.path)
				path = uri.path[0] == '/' ? uri.path + 1 : uri.path;

			StringUtil::url_decode(path);
			server = __ftp_server_map.get(uri);
		}
	}
	else if (uri.state == URI_STATE_ERROR)
	{
		state = WFT_STATE_SYS_ERROR;
		error = uri.error;
	}

	task = new FtpClientTask(server, op, std::move(callback));
	if (server)
		server->decref();
	else
		task->set_error(state, error);

	return task;
}

WFFtpTask *WFFtpTaskFactory::create_get_task(const std::string& url,
											 ftp_callback_t callback)
{
	std::string path;
	auto *task = __create_url_task(url, FTP_OP_RETR, std::move(callback), path);

	task->get_req()->set_command("RETR", path);
	return task;
}

WFFtpTask *WFFtpTaskFactory::create_list_task(const std::string& url,
											  ftp_callback_t callback)
{
	std::string path;
	auto *task = __create_url_task(url, FTP_OP_LIST, std::move(callback), path);

	task->get_req()->set_command("LIST", path);
	return task;
}

WFFtpTask *WFFtpTaskFactory::create_put_task(const std::string& url,
											 const void *buf, size_t size,
											 ftp_callback_t callback)
{
	std::string path;
	auto *task = __create_url_task(url, FTP_OP_STOR, std::move(callback), path);

	task->get_req()->set_command("STOR", path);
	if (size > 0)
		task->upload.assign((const char *)buf, size);

	return task;
}

WFFtpTask *WFFtpTaskFactory::create_rename_task(const std::string& url,
												const std::string& to,
												ftp_callback_t callback)
{
	std::string path;
	auto *task = __create_url_task(url, FTP_OP_RENAME, std::move(callback),
								   path);

	task->get_req()->set_command("RNFR", path);
	task->rename_to = to;
	return task;
}

WFFtpTask *WFFtpTaskFactory::create_command_task(const std::string& url,
												 const std::string& command,
												 const std::string& argument,
												 ftp_callback_t callback)
{
	std::string path;
	auto *task = __create_url_task(url, FTP_OP_COMMAND, std::move(callback),
								   path);

	task->get_req()->set_command(command, argument);
	return task;
}

void WFFtpTaskFactory::set_max_connections(size_t max)
{
	__ftp_server_map.set_max_connections(max);
}

