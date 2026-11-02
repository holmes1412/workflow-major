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
#include <ctype.h>
#include <string.h>
#include <string>
#include "FtpMessage.h"

/* Lines of commands and replies are short, so a much longer one is from a
 * broken or malicious peer. */
#define FTP_LINE_MAX	(64 * 1024)

namespace protocol
{

int FtpRequest::encode(struct iovec vectors[], int max)
{
	static const std::string forbidden("\r\n\0", 3);

	if (this->command.empty())
		return 0;

	if (this->command.find_first_of(forbidden) != std::string::npos ||
		this->command.find(' ') != std::string::npos ||
		this->argument.find_first_of(forbidden) != std::string::npos)
	{
		errno = EBADMSG;
		return -1;
	}

	this->line = this->command;
	if (!this->argument.empty())
	{
		this->line.push_back(' ');
		this->line.append(this->argument);
	}

	this->line.append("\r\n");
	vectors[0].iov_base = (char *)this->line.data();
	vectors[0].iov_len = this->line.size();
	return 1;
}

int FtpRequest::append(const void *buf, size_t *size)
{
	const char *p = (const char *)buf;
	const char *eol = (const char *)memchr(p, '\n', *size);
	size_t len = eol ? eol + 1 - p : *size;
	size_t pos;

	if (len > this->size_limit - this->line.size() ||
		len > FTP_LINE_MAX - this->line.size())
	{
		errno = EMSGSIZE;
		return -1;
	}

	if (!eol)
	{
		this->line.append(p, len);
		return 0;
	}

	this->line.append(p, eol - p);
	if (!this->line.empty() && this->line.back() == '\r')
		this->line.pop_back();

	pos = this->line.find(' ');
	this->command = this->line.substr(0, pos);
	if (pos != std::string::npos)
		this->argument = this->line.substr(pos + 1);
	else
		this->argument.clear();

	this->line.clear();
	*size = len;
	return 1;
}

int FtpResponse::encode(struct iovec vectors[], int max)
{
	static const std::string forbidden("\r\0", 2);
	std::string code;
	size_t pos = 0;
	size_t eol;

	if (this->code == 0)
		return 0;

	if (this->code < 100 || this->code > 599 ||
		this->text.find_first_of(forbidden) != std::string::npos)
	{
		errno = EBADMSG;
		return -1;
	}

	code = std::to_string(this->code);
	this->line.clear();
	while ((eol = this->text.find('\n', pos)) != std::string::npos)
	{
		if (pos == 0)
			this->line += code + "-";
		else if (pos < eol && isdigit((unsigned char)this->text[pos]))
			this->line += " ";	/* Not to be taken as the last line. */

		this->line.append(this->text, pos, eol - pos);
		this->line += "\r\n";
		pos = eol + 1;
	}

	this->line += code + " ";
	this->line.append(this->text, pos, std::string::npos);
	this->line += "\r\n";
	vectors[0].iov_base = (char *)this->line.data();
	vectors[0].iov_len = this->line.size();
	return 1;
}

static inline int __reply_code(const char *line, size_t len)
{
	if (len < 3 || line[0] < '1' || line[0] > '5' ||
		line[1] < '0' || line[1] > '9' || line[2] < '0' || line[2] > '9')
	{
		return -1;
	}

	return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

int FtpResponse::append_line(const char *line, size_t len)
{
	bool last;

	if (this->reply_code == 0)
	{
		this->reply_code = __reply_code(line, len);
		if (this->reply_code < 0 ||
			(len > 3 && line[3] != ' ' && line[3] != '-'))
		{
			errno = EBADMSG;
			return -1;
		}

		this->multiline = (len > 3 && line[3] == '-');
		if (len > 4)
			this->reply_text.assign(line + 4, len - 4);

		last = !this->multiline;
	}
	else
	{
		last = (__reply_code(line, len) == this->reply_code &&
				(len == 3 || line[3] == ' '));
		this->reply_text.push_back('\n');
		if (!last)
			this->reply_text.append(line, len);
		else if (len > 4)
			this->reply_text.append(line + 4, len - 4);
	}

	if (!last)
		return 0;

	if (this->reply_code / 100 == 1)
	{
		/* Preliminary reply. The completion reply may come after a long
		 * time, like when a transfer is finished. */
		this->pre_code = this->reply_code;
		this->pre_text = std::move(this->reply_text);
		this->reply_text.clear();
		this->reply_code = 0;
		this->renew();
		return 0;
	}

	this->code = this->reply_code;
	this->text = std::move(this->reply_text);
	return 1;
}

int FtpResponse::append(const void *buf, size_t *size)
{
	const char *begin = (const char *)buf;
	const char *end = begin + *size;
	const char *p = begin;
	const char *eol;
	size_t len;
	int ret;

	while (p < end)
	{
		eol = (const char *)memchr(p, '\n', end - p);
		len = (eol ? eol + 1 : end) - p;
		if (len > this->size_limit - this->size_received ||
			len > FTP_LINE_MAX - this->line.size())
		{
			errno = EMSGSIZE;
			return -1;
		}

		this->size_received += len;
		if (!eol)
		{
			this->line.append(p, len);
			break;
		}

		this->line.append(p, eol - p);
		p = eol + 1;
		if (!this->line.empty() && this->line.back() == '\r')
			this->line.pop_back();

		ret = this->append_line(this->line.c_str(), this->line.size());
		this->line.clear();
		if (ret != 0)
		{
			if (ret > 0)
				*size = p - begin;

			return ret;
		}
	}

	return 0;
}

int FtpDataMessage::encode(struct iovec vectors[], int max)
{
	if (this->data.empty())
		return 0;

	vectors[0].iov_base = (char *)this->data.data();
	vectors[0].iov_len = this->data.size();
	return 1;
}

int FtpDataMessage::append(const void *buf, size_t *size)
{
	if (*size > this->size_limit - this->data.size())
	{
		errno = EMSGSIZE;
		return -1;
	}

	this->data.append((const char *)buf, *size);
	return 0;
}

}

