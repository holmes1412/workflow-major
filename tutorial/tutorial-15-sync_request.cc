#include <stdio.h>
#include <errno.h>

#include "workflow/HttpUtil.h"
#include "workflow/WFCoTask.h"

int main()
{
	protocol::HttpRequest req;
	req.set_method(HttpMethodGet);
	req.set_http_version("HTTP/1.1");
	req.set_request_uri("/");
	req.set_header_pair("Host", "www.sogou.com");

//	WFFacilities::WFNetworkResult<protocol::HttpResponse> result;
	auto result = sync_request<protocol::HttpRequest,
							   protocol::HttpResponse>(TT_TCP_SSL,
													   "https://www.sogou.com",
													   std::move(req),
													   1);

	fprintf(stderr, "after sync_request() state=%d", result.task_state);

	if (result.task_state == WFT_STATE_SUCCESS)
	{
		fprintf(stderr, "\n");
		const void *body;
		size_t body_len;

		result.resp.get_parsed_body(&body, &body_len);
		fwrite(body, 1, body_len, stdout);
		fflush(stdout);
	}
	else
		fprintf(stderr, "error=%d\n", result.resp.get_status_code());

	return 0;
}

