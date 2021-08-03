#include <stdio.h>
#include <stdlib.h>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFChannel.h"
#include "workflow/WFFacilities.h"

#include "message.h"

using channel_process_t =
std::function<void (WFChannelTask<protocol::TutorialMessage> *)>;

using namespace protocol;

WFFacilities::WaitGroup wait_group(2);

void process(WFChannelTask<TutorialMessage> *task)
{
	void *body;
	size_t body_size;
	TutorialMessage *msg = task->get_msg();

	if (task->get_state() == WFT_STATE_SUCCESS)
	{
		msg->get_message_body_nocopy(&body, &body_size);
		if (body_size != 0)
			fprintf(stderr, "process() msg: %.*s\n", (int)body_size, (char *)body);
		else
			fprintf(stderr, "process() state: %d error: %d\n", task->get_state(),
					task->get_error());
	}
	wait_group.done();
}

int main(int argc, const char *argv[])
{
	std::string url;

	if (argc != 2)
	{
		fprintf(stderr, "[USAGE] %s URL\n", argv[0]);
		return 0;
	}

	url = argv[1];

	// 0. construct channel with process function
	auto *channel = WFChannelFactory<TutorialMessage>::create_channel(process);

	// 1. init and set uri
	ParsedURI uri;
	if (URIParser::parse(url, uri) != 0)
	{
		fprintf(stderr, "[ERROR] parse url failed\n");
		return 0;
	}
	channel->set_uri(uri);

	char buf[1024] = "TutorialMessage from channel_client.\n";
	// 2. task1 -> connect -> upgrade -> [recieve many | send many] -> task1-cb
	auto *task1 = WFChannelFactory<TutorialMessage>::create_out_task(channel,
			[](WFChannelTask<TutorialMessage> *task) {
		fprintf(stderr, "finish sending task1 and now channel can receive.\n");
	});
	task1->get_msg()->set_message_body(buf, 1024);

	//3. you can use one series to make sending in sequence
	auto *task2 = WFChannelFactory<TutorialMessage>::create_out_task(channel,
			[](WFChannelTask<TutorialMessage> *task) {	
		if (task->get_state() == WFT_STATE_SUCCESS)
		{
			TutorialMessage *msg = task->get_msg();
			void *body;
			size_t body_size;
			msg->get_message_body_nocopy(&body, &body_size);
			if (body_size != 0)
				fprintf(stderr, "task2 get msg: %.*s\n",
						(int)body_size, (char *)body);
		}
		else
			fprintf(stderr, "task2 state: %d error: %d\n", task->get_state(),
					task->get_error());
	});
	task2->get_msg()->set_message_body(buf, 1024);

	// when using the tradition server, timer task is just for test.
	auto *task3 = WFTaskFactory::create_timer_task(100000 /* 0.1s */, nullptr);
	SeriesWork *series = Workflow::create_series_work(task1, nullptr);
	series->push_back(task3);
	series->push_back(task2);
	series->start();
	wait_group.wait(); // testing normal server, so expecting 2 response

	return 0;
}

