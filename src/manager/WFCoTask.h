#ifndef _WFCOTASK_H_
#define _WFCOTASK_H_

#include <ucontext.h> 
#include <stdlib.h>
#include <string.h>
#include <string>

#include <unistd.h>

#include "workflow/Workflow.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFFacilities.h"
#include "workflow/BasicQueue.h"

BasicQueue<ucontext_t *> queue(10);

void start_task(SubTask *task)
{
	Workflow::start_series_work(task, nullptr);

	ucontext_t *other_ctx = queue.dequeue();
	setcontext(other_ctx);
}

template<class REQ, class RESP>
WFFacilities::WFNetworkResult<RESP>
sync_request(TransportType type, const std::string& url,
			 REQ&& req, int retry_max)
{
	// 1. prepare for task
	ParsedURI uri;
	WFFacilities::WFNetworkResult<RESP> result;

	auto *task = new WFComplexClientTask<REQ, RESP>(retry_max,
						[&result](WFNetworkTask<REQ, RESP> *task) {

		// 2. prepare for callback
		result.seqid = task->get_task_seq();
		result.task_state = task->get_state();
		result.task_error = task->get_error();

		if (result.task_state == WFT_STATE_SUCCESS)
			result.resp = std::move(*task->get_resp());

		fprintf(stderr, "task callback is ready to setcontext().\n");
		queue.enqueue((ucontext_t *)task->user_data);
	});

	URIParser::parse(url, uri);
	task->init(std::move(uri));
	task->set_transport_type(type);
	*task->get_req() = std::forward<REQ>(req);

	// 3. prepare for ucontext
	char stack[10 * 1024];

	ucontext_t cur_ctx;
	if (getcontext(&cur_ctx) < 0)
	{
		perror("getcontext ");
		result.task_state = WFT_STATE_SYS_ERROR;
		result.task_error = errno;
		return std::move(result);
	}

	ucontext_t new_ctx = cur_ctx;
	new_ctx.uc_stack.ss_sp = stack;
	new_ctx.uc_stack.ss_size = sizeof(stack);
	new_ctx.uc_stack.ss_flags = 0;

	task->user_data = &cur_ctx;

	makecontext(&new_ctx, (void (*)(void))start_task, 1, (SubTask *)task);

	fprintf(stderr, "sync_request() makecontext and swapcontext\n");
	swapcontext(&cur_ctx, &new_ctx);

	fprintf(stderr, "sync_request() finished and continue.\n");
	return std::move(result);					
}

#endif

