#ifndef _WFHTTPSTREAMINGCLIENT_H_
#define _WFHTTPSTREAMINGCLIENT_H_

#include <string>
#include <functional>
#include "WFTask.h"
#include "WFTaskFactory.h"


class WFHttpStreamingClient
{
public:
	using chunk_callback_t = std::function<void (protocol::HttpMessageChunk *)>;

public:
 	WFHttpTask *create_streaming_task(int redirect_max,
									  int retry_max,
									  chunk_callback_t chunk_calback,
									  http_callback_t task_callback);

	int init(const std::string& url, int watch_timeout);
	void deinit() { }

private:
    ParsedURI uri;
    int timeout;
};

int WFHttpStreamingClient::init(const std::string& url, int watch_timeout)
{
	if (URIParser::parse(url, this->uri) >= 0)
	{
		this->timeout = watch_timeout;
		return 0;
	}

	if (this->uri.state == URI_STATE_INVALID)
		errno = EINVAL;

	return -1;
}

#endif
