### Sogou C++ Workflow  
搜狗公司C++服务器引擎，支撑搜狗几乎所有后端C++在线服务，包括所有搜索服务，云输入法，在线广告等，每日处理超百亿请求。  
##### 你可以用来：
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
* 实现自定义协议client/server，构建自己的RPC系统。
  * 搜狗RPC就是以它为基础，作为独立项目开源。该项目支持srpc，brpc和thrift等协议（[benchmark](https://github.com/holmes1412/sogou-rpc-benchmark)）。
* 构建任意复杂的任务流，支持常用的串并联，也支持更加复杂的DAG。
* 作为并行编程工具使用。因为除了网络任务，我们也包含计算任务的调度。所有类型的任务都可以放入同一个任务流中。
* 在Linux系统下作为磁盘异步IO工具使用，性能超过任何标准系统调用。磁盘IO也是一种任务。
* 实现服务网格。
  * 内置了强大的服务治理和负载均衡系统。

##### 系统支持
* 项目支持Linux，MacOS，Windows等操作系统。
  *  Windows版暂时以独立branch发布，使用iocp实现异步网络。用户接口与Linux版一致。
* 支持所有CPU平台，包括32或64位x86，大端或小端arm处理器。
* 项目依赖于OpenSSL，推荐OpenSSL 1.1及以上版本。
* 项目使用了C++11的功能，需要用支持C++11的编译器编译。但不依赖boost或asio。
