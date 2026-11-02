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

#ifndef _FTPMESSAGE_H_
#define _FTPMESSAGE_H_

#include <stddef.h>
#include <string>
#include <utility>
#include "ProtocolMessage.h"

/**
 * @file   FtpMessage.h
 * @brief  FTP control connection and data connection messages
 */

namespace protocol
{

/* A command on the control connection. A request with an empty command
 * sends nothing, which is used for receiving the server greeting. As a
 * server request, it receives one command line. */
class FtpRequest : public ProtocolMessage
{
public:
	void set_command(std::string command)
	{
		this->command = std::move(command);
		this->argument.clear();
	}

	void set_command(std::string command, std::string argument)
	{
		this->command = std::move(command);
		this->argument = std::move(argument);
	}

	const std::string& get_command() const { return this->command; }
	const std::string& get_argument() const { return this->argument; }

protected:
	virtual int encode(struct iovec vectors[], int max);
	virtual int append(const void *buf, size_t *size);

protected:
	std::string command;
	std::string argument;

private:
	std::string line;

public:
	FtpRequest() = default;

public:
	FtpRequest(FtpRequest&& req) = default;
	FtpRequest& operator = (FtpRequest&& req) = default;
};

/* A reply on the control connection. Preliminary replies (1yz) are kept
 * and receiving continues until the completion reply arrives, so the
 * response of 'RETR' contains both '150' and '226'. */
class FtpResponse : public ProtocolMessage
{
public:
	/* The final reply. Reply code is 0 before any reply is received. */
	int get_reply_code() const { return this->code; }
	const std::string& get_reply_text() const { return this->text; }

	/* For server. Lines of a multi-line reply are separated by '\n'. */
	void set_reply(int code, std::string text)
	{
		this->code = code;
		this->text = std::move(text);
	}

	/* The last preliminary reply, such as '150'. Code is 0 if none. */
	int get_preliminary_code() const { return this->pre_code; }
	const std::string& get_preliminary_text() const { return this->pre_text; }

	/* Data received from the data connection, for LIST/NLST/MLSD/RETR. */
	std::string *get_data() { return &this->data; }
	const std::string *get_data() const { return &this->data; }
	void set_data(std::string data) { this->data = std::move(data); }

protected:
	virtual int encode(struct iovec vectors[], int max);
	virtual int append(const void *buf, size_t *size);

private:
	int append_line(const char *line, size_t len);

protected:
	int code;
	std::string text;
	int pre_code;
	std::string pre_text;
	std::string data;

private:
	std::string line;
	std::string reply_text;
	int reply_code;
	bool multiline;
	size_t size_received;

public:
	FtpResponse()
	{
		this->code = 0;
		this->pre_code = 0;
		this->reply_code = 0;
		this->multiline = false;
		this->size_received = 0;
	}

public:
	FtpResponse(FtpResponse&& resp) = default;
	FtpResponse& operator = (FtpResponse&& resp) = default;
};

/* Raw bytes on the data connection. As a request, it sends all the data.
 * As a response, it keeps receiving until the server closes the connection,
 * because the end of a transfer is signaled by closing the connection. */
class FtpDataMessage : public ProtocolMessage
{
public:
	std::string *get_data() { return &this->data; }
	const std::string *get_data() const { return &this->data; }
	void set_data(std::string data) { this->data = std::move(data); }

protected:
	virtual int encode(struct iovec vectors[], int max);
	virtual int append(const void *buf, size_t *size);

protected:
	std::string data;

public:
	FtpDataMessage() = default;

public:
	FtpDataMessage(FtpDataMessage&& msg) = default;
	FtpDataMessage& operator = (FtpDataMessage&& msg) = default;
};

using FtpDataRequest = FtpDataMessage;
using FtpDataResponse = FtpDataMessage;

}

#endif

