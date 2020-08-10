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
* 
