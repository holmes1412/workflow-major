### Sogou C++ Workflow  
搜狗公司C++服务器引擎，支撑搜狗几乎所有后端C++在线服务，包括所有搜索服务，云输入法，在线广告等，每日处理超百亿请求。  
你可以用来：
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
* 作为万能异步客户端。目前支持http，redis，mysql和kafka协议。
* 实现自定义协议client/server，构建自己的rpc系统。
  * 搜狗RPC就是以它为基础，作为独立项目开源。该项目支持srpc，brpc和thrift等协议（[benchmark](https://github.com/holmes1412/sogou-rpc-benchmark)）。
* 构建任意复杂的任务流，支持常用的串并联，也支持更加复杂的DAG。
