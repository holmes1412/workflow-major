### Sogou C++ Workflow  
搜狗公司C++服务器引擎，支撑搜狗几乎所有后端C++在线服务，包括所有搜索服务，云输入法，在线广告等，每日处理超百亿请求。  
你可以使用它：
* 快速搭建http服务器：
~~~cpp
#include <stdio.h>
#include "workflow/WFHttpServer.h"
int main()
{
    WFHttpServer server([](WFHttpTask *task){ task->get_resp()->append_output_body("<html>Hello World!</html>"); });
    if (server.start(8888) == 0) {  // start server on port 8888
        getchar(); // press "Enter" to end.
        server.stop();
    }
    return 0;
}
~~~
* 作为万能异步客户端。目前支持http，redis，mysql和kafka协议，例如访问本地redis服务：
~~~cpp
#include "workflow/WFTaskFactory.h"
#include "worfflow/WFFacilities.h"
int main(void)
{
	WFFacilities::WaitGroup wait_group(1);
    WFRedisTask *task = WFTaskFactory::create_redis_task("redis://127.0.0.1/", 0, [](WFRedisTask *task) {
        if (task->get_state() == WFT_STATE_SUCCESS) {
            protocol::RedisValue val;
            task->get_resp()->get_result(val);
            if (!val.is_error())
                printf("SET SUCCESS!\n);
        }
        wait_group.done();
    });
    task->get_req()->set_request("SET", { "Hello", "World" });
    task->start();
    wait_group.wait();
    return 0
}
~~~
