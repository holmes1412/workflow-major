#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFHttpChunkedClient.h"
#include "workflow/json_parser.h"

#define REDIRECT_MAX	   5
#define MAX_CONTENT_LENGTH 10240
volatile bool stop_flag;

std::string url = "https://api.deepseek.com/v1/chat/completions";
std::string auth = "Bearer ";
std::string prompt;
std::string type;
WFHttpChunkedClient client;

const char *model = "deepseek-reasoner";
bool stream = true;
int ttft = 100; // time to first token = 100 seconds

enum
{
	CHAT_STATE_BEGIN = 0,
	CHAT_STATE_THINKING,
	CHAT_STATE_OUTPUT,
	CHAT_STATE_FINISH,
};

int state = CHAT_STATE_BEGIN;

void next_query(SeriesWork *series);

bool parse_json_message(const json_value_t *choice)
{
	const json_value_t *val;
	const char *str;

	const json_object_t *choice_obj = json_value_object(choice);

	const json_value_t *obj = json_object_find("message", choice_obj);
	fprintf(stderr, "find message\n");
	if (!obj || json_value_type(obj) != JSON_VALUE_OBJECT)
		return false;

	const json_object_t *message_obj = json_value_object(obj);

	if (strcmp(model, "deepseek-reasoner") == 0)
	{
		fprintf(stderr, "find reasoning\n");
		val = json_object_find("reasoning_content", message_obj);	
		if (!val || json_value_type(val) != JSON_VALUE_STRING)
			return false;

		fprintf(stderr, "\n<think>");
		str = json_value_string(val);
		if (str && str[0] != '\0')
			fprintf(stderr, "\n%s\n", str);
		fprintf(stderr, "<\\think>\n");
		
	}

	message_obj = json_value_object(obj);
	fprintf(stderr, "find content\n");
	val = json_object_find("content", message_obj);
	if (!val || json_value_type(val) != JSON_VALUE_STRING)
		return false;

	str = json_value_string(val);
	if (str && str[0] != '\0')
		fprintf(stderr, "\n%s\n", str);

	return true;
}

bool parse_json_delta(const json_value_t *choice)
{
	const json_value_t *val;
	const char *name;
	const char *str;
	bool found = false;

	const json_object_t *co = json_value_object(choice);
	const json_value_t *obj = json_object_find("delta", co);
	if (!obj || json_value_type(obj) != JSON_VALUE_OBJECT)
		return false;

	const json_object_t *delta_obj = json_value_object(obj);
	json_object_for_each(name, val, delta_obj)
	{
		// "role" == "assistant"
		if (state == CHAT_STATE_BEGIN && strcmp(name, "role") == 0)
		{
			if (strcmp(model, "deepseek-reasoner") == 0)
			{
				fprintf(stderr, "\n<think>\n");
				state = CHAT_STATE_THINKING;
			} else {
				state = CHAT_STATE_OUTPUT;
			}
			found = true;
			break;
		}

		if (strcmp(name, "content") == 0 &&
			json_value_type(val) == JSON_VALUE_STRING)
		{
			str = json_value_string(val);
			if (str && str[0] != '\0')
			{
				if (state == CHAT_STATE_THINKING)
				{
					state = CHAT_STATE_OUTPUT;
					fprintf(stderr, "\n<\\think>\n");
				}

				fprintf(stderr, "%s", str);
				found = true;
			}	
		} else if (strcmp(name, "reasoning_content") == 0 &&
				   json_value_type(val) == JSON_VALUE_STRING) {
			{
				str = json_value_string(val);
				if (str && str[0] != '\0')
				{
					fprintf(stderr, "%s", str);
					found = true;
				}
			}
		}
	} // end json_object_for_each delta

	if (found == false)
	{	
		obj = json_object_find("finish_reason", co);
		if (obj && json_value_type(obj) == JSON_VALUE_STRING)
		{
			str = json_value_string(obj);
			if (strncmp(str, "stop", 4) == 0)
			{
				state = CHAT_STATE_FINISH;
				fprintf(stderr, "\n");
			}
		}
	}
	if (!found && state != CHAT_STATE_FINISH)
		return false;

	return true;
}

void parse_json(const char *msg, size_t size)
{
	char *json_buf = (char *)malloc(size + 1);
	if (!json_buf)
	{
		perror("malloc");
		return;
	}

	memcpy(json_buf, msg, size);
	json_buf[size] = '\0';

	json_value_t *root = json_value_parse(json_buf);
	free(json_buf);

	if (!root || json_value_type(root) != JSON_VALUE_OBJECT)
	{
		fprintf(stderr, "Error : Invalid JSON.\n---\n%.*s\n---\n", (int)size, msg);
		return;
	}

	const json_object_t *obj = json_value_object(root);
	const json_value_t *choices = json_object_find("choices", obj);
	if (!choices || json_value_type(choices) != JSON_VALUE_ARRAY)
	{
		fprintf(stderr, "Error : No necessary object 'choices'\n");
		json_value_destroy(root);
		return;
	}

	const json_array_t *arr = json_value_array(choices);
	const json_value_t *choice;
	bool ret;

	json_array_for_each(choice, arr)
	{
		if (json_value_type(choice) != JSON_VALUE_OBJECT)
			continue;

		if (stream)
			ret = parse_json_delta(choice);
		else
			ret = parse_json_message(choice);

		if (ret == false)
		{
			fprintf(stderr, "Error : Invalid Data.\n%.*s\n", (int)size, msg);
			break;
		}
	} // end json_object_for_each choices

	json_value_destroy(root);
}

void extract(WFHttpChunkedTask *task)
{
	protocol::HttpMessageChunk *chunk = task->get_chunk();
	const void *msg;
	size_t size;

	if (chunk->get_chunk_data(&msg, &size))
	{
		const char *p = static_cast<const char *>(msg);
		const char *msg_end = p + size;
		const char *begin;
		const char *end;
		size_t len;

		while (p < msg_end)
		{
			begin = strstr(p, "data: ");
			if (!begin || begin >= msg_end)
				break;

			begin += 6;

			end = strstr(begin, "data: "); // \r\n
			end = end ? end : msg_end;
			p = end;

			while (end > begin && (*(end - 1) == '\n' || *(end - 1) == '\r'))
				--end;

			len = end - begin;
			if (len > 0 && state != CHAT_STATE_FINISH)
				parse_json(begin, len);
		}
	}
	else
		fprintf(stderr, "Error. Invalid chunk data.\n");
}

void http_callback(WFHttpTask *task)
{
	protocol::HttpRequest *req = task->get_req();
	protocol::HttpResponse *resp = task->get_resp();
	int state = task->get_state();
	int error = task->get_error();

	fprintf(stderr, "Task state: %d\n", state);

	if (state != WFT_STATE_SUCCESS)
	{
		fprintf(stderr, "Task error: %d\n", error);
		if (error == ETIMEDOUT)
			fprintf(stderr, "Timeout reason: %d\n", task->get_timeout_reason());
		return;
	}

	std::string output = protocol::HttpUtil::decode_chunked_body(resp);
	parse_json(output.data(), output.length());

	fprintf(stderr, "%s %s %s\r\n", req->get_method(),
									req->get_http_version(),
									req->get_request_uri());

	std::string name;
	std::string value;
	protocol::HttpHeaderCursor req_cursor(req);

	while (req_cursor.next(name, value))
		fprintf(stderr, "%s: %s\r\n", name.c_str(), value.c_str());
	fprintf(stderr, "\r\n");

	fprintf(stderr, "%s %s %s\r\n", resp->get_http_version(),
									resp->get_status_code(),
									resp->get_reason_phrase());

	protocol::HttpHeaderCursor resp_cursor(resp);

	while (resp_cursor.next(name, value))
		fprintf(stderr, "%s: %s\r\n", name.c_str(), value.c_str());
	fprintf(stderr, "\r\n");

	return next_query(series_of(task));
}

void callback(WFHttpChunkedTask *task)
{
	protocol::HttpRequest *req = task->get_req();
	protocol::HttpResponse *resp = task->get_resp();
	int state = task->get_state();
	int error = task->get_error();

	fprintf(stderr, "Task state: %d\n", state);

	if (state != WFT_STATE_SUCCESS)
	{
		fprintf(stderr, "Task error: %d\n", error);
//		if (error == ETIMEDOUT)
//			fprintf(stderr, "Timeout reason: %d\n", task->get_timeout_reason());
		return;
	}

	std::string name;
	std::string value;

	fprintf(stderr, "%s %s %s\r\n", req->get_method(),
									req->get_http_version(),
									req->get_request_uri());

	protocol::HttpHeaderCursor req_cursor(req);

	while (req_cursor.next(name, value))
		fprintf(stderr, "%s: %s\r\n", name.c_str(), value.c_str());
	fprintf(stderr, "\r\n");

	fprintf(stderr, "%s %s %s\r\n", resp->get_http_version(),
									resp->get_status_code(),
									resp->get_reason_phrase());

	protocol::HttpHeaderCursor resp_cursor(resp);

	while (resp_cursor.next(name, value))
		fprintf(stderr, "%s: %s\r\n", name.c_str(), value.c_str());
	fprintf(stderr, "\r\n");

	return next_query(series_of(task));
}

void next_query(SeriesWork *series)
{
	int len;
	char query[MAX_CONTENT_LENGTH];
	protocol::HttpRequest *req;
	std::string body;

	fprintf(stderr, "Query> ");
	while (stop_flag == false && (fgets(query, MAX_CONTENT_LENGTH, stdin)))
	{
		len = strlen(query);
		if (len == 0 || strncmp(query, "\0", len) == 0)
		{
			fprintf(stderr, "Query> ");
			continue;
		}

		if (stop_flag == true)
			break;

		query[len - 1] = '\0';
		body = prompt.append(query).append(type);

		if (stream)
		{
			auto *task = client.create_chunked_task(url,
													REDIRECT_MAX,
													extract,
													callback);
			task->set_watch_timeout(ttft * 1000);
			req = task->get_req();
			series->push_back(task);
		} else {
			auto *task = WFTaskFactory::create_http_task(url,
														 REDIRECT_MAX,
														 0,
														 http_callback);
			task->set_watch_timeout(ttft * 1000 * 5); // use 500s no streaming
			req = task->get_req();
			series->push_back(task);
		}

		req->add_header_pair("Authorization", auth);
		req->add_header_pair("Content-Type", "application/json");
		req->add_header_pair("Connection", "keepalive");
		req->set_method("POST");
		req->append_output_body(body.c_str(), body.size());

		state = CHAT_STATE_BEGIN;
		break;
	}

	return;
}

void sig_handler(int signo)
{
	stop_flag = true;
}

int main(int argc, char *argv[])
{
	if (argc < 2 || argc > 4)
	{
		fprintf(stderr, "USAGE: %s <api_key> [model] [stream]\n"
				"	api_key - API KEY for DeepSeek\n"
				"	model - set 'deepseek-chat' for V3 or 'deepseek-reasoning' for R1\n"
				"	stream - set 'true' for streaming output or 'false' for one output\n",
				argv[0]);
		exit(1);
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	stop_flag = false;

	auth.append(argv[1]);

	if (argc >= 3)
		model = argv[2];

	if (argc == 4)
	{
		if (strcasecmp(argv[3], "false") == 0)
		{
			stream = false;
		}
		else if (strcasecmp(argv[3], "true") != 0)
		{
			fprintf(stderr, "Error! stream requires 'true' or 'false'.\n");
			exit(1);
		}
	}

	if (strcmp(model, "deepseek-reasoner") == 0)
	{	
		prompt = R"(
{
	"model": "deepseek-reasoner",
	"messages": [{"role": "user", "content": ")";
	} else if (strcmp(model, "deepseek-chat") == 0) {
		prompt = R"(
{
	"model": "deepseek-chat",
	"messages": [{"role": "user", "content": ")";\
	} else {
		fprintf(stderr, "Error: Invalid model %s\n", model);
		exit(1);
	}

	if (stream == true)
	{
		type = R"("}],
	"stream": true
  }

)";
	} else {
		type = R"("}],
	"stream": false
  }

)";
	}

	struct WFGlobalSettings settings = GLOBAL_SETTINGS_DEFAULT;
	settings.endpoint_params.connect_timeout = 60 * 1000;
	if (stream == false)
		settings.endpoint_params.response_timeout = ttft * 1000 * 5; // 500s for no streaming

	WORKFLOW_library_init(&settings);

	WFCounterTask *counter = WFTaskFactory::create_counter_task(0, [](WFCounterTask *task) {
		return next_query(series_of(task));
	});

	WFFacilities::WaitGroup wait_group(1);
	SeriesWork *series = Workflow::create_series_work(counter,
		[&wait_group](const SeriesWork *series) {
			wait_group.done();
	});

	series->start();
	wait_group.wait();

	return 0;	
}
