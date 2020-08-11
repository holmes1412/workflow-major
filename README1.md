[![license MIT](https://img.shields.io/badge/License-Apache-yellow.svg)](https://git.sogou-inc.com/wujiaxu/Filter/blob/master/LICENSE)
[![C++](https://img.shields.io/badge/language-c++-red.svg)](https://en.cppreference.com/)
[![platform](https://img.shields.io/badge/platform-linux%20%7C%20macos-lightgrey.svg)](#%E9%A1%B9%E7%9B%AE%E7%9A%84%E4%B8%80%E4%BA%9B%E8%AE%BE%E8%AE%A1%E7%89%B9%E7%82%B9)
## Sogou C++ Workflow
搜狗公司C++服务器引擎，支撑搜狗几乎所有后端C++在线服务，包括所有搜索服务，云输入法，在线广告等，每日处理超百亿请求。  
#### 你可以用来：
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
* 构建异步任务流，支持常用的串并联，也支持更加复杂的DAG结构。
* 作为并行编程工具使用。因为除了网络任务，我们也包含计算任务的调度。所有类型的任务都可以放入同一个任务流中。
* 在Linux系统下作为文件异步IO工具使用，性能超过任何标准调用。磁盘IO也是一种任务。
* 实现任何计算与通讯关系非常复杂的高性能高并发的后端服务。

#### 编译和运行环境
* 项目支持Linux，MacOS，Windows等操作系统。
  *  Windows版暂时以独立branch发布，使用iocp实现异步网络。用户接口与Linux版一致。
* 支持所有CPU平台，包括32或64位x86，大端或小端arm处理器。
* 项目依赖于OpenSSL，推荐OpenSSL 1.1及以上版本。
* 项目使用了C++11的功能，需要用支持C++11的编译器编译。但不依赖boost或asio。

# 试一下！
  * Client基础
    * [创建第一个任务：wget](docs/tutorial-01-wget.md)
    * [实现一次redis写入与读出：redis_cli](docs/tutorial-02-redis_cli.md)
    * [任务序列的更多功能：wget_to_redis](docs/tutorial-03-wget_to_redis.md)
  * Server基础
    * [第一个server：http_echo_server](docs/tutorial-04-http_echo_server.md)
    * [异步server的示例：http_proxy](docs/tutorial-05-http_proxy.md)
  * 并行任务与工作流　
    * [一个简单的并行抓取：parallel_wget](docs/tutorial-06-parallel_wget.md)
  * 几个重要的话题
    * [关于错误处理](docs/about-error.md)
    * [关于超时](docs/about-timeout.md)
    * [关于DNS](docs/about-dns.md)
    * [关于程序退出](docs/about-exit.md)
  * 计算任务
    * [使用内置算法工厂：sort_task](docs/tutorial-07-sort_task.md)
    * [自定义计算任务：matrix_multiply](docs/tutorial-08-matrix_multiply.md)
  * 文件异步IO任务
    * [异步IO的http server：http_file_server](docs/tutorial-09-http_file_server.md)
  * 用户定义协议基础
    * [简单的用户自定义协议client/server](docs/tutorial-10-user_defined_protocol.md)
  * 定时与计数任务
    * [关于定时器](docs/about-timer.md)
    * [关于计数器](docs/about-counter.md)
  * 服务治理
    * [关于服务治理](docs/about-service-management.md)
    * [Upstream更多文档](docs/about-upstream.md)
  * 连接上下文的使用
    * [关于连接上下文](docs/about-connection-context.md)
  * 内置协议用法
    * [异步MySQL客户端：mysql_cli](docs/tutorial-12-mysql_cli.md)


#### 系统设计特点
我们认为，一个典型的后端程序由一些三个部分组成，并且完全独立开发。
* 协议
  * 大多数情况下，用户使用的是内置的通用网络协议，例如http，redis或各种rpc。
  * 用户可以方便的自定义网络协议，只需提供序列化和反序列化函数，就可以定义出自己的client/server。
* 算法
  * 在我们的设计里，算法是与协议对称的概念。如果说协议的调用是rpc，算法的调用就是一次apc（Async Procedure Call）。
  * 我们提供了一些通用算法，例如sort，merge，psort，reduce，可以直接使用。
  * 与自定义协议相比，自定义算法的使用要常见的多。任何一次边界清晰的复杂计算，都应该包装成算法。
* 任务流
  * 任务流就是实际的业务逻辑，就是把开发好的协议与算法放在流程图里使用起来。
  * 典型的任务流是一个闭合的串并联图（相信成一个闭合串并联电路）。复杂的业务逻辑，可能是一个非闭合的DAG。
  * 任务流图可以直接构建，也可以根据每一步的结果动态生成。所有任务都是异步执行的，通过callback返回。
基础任务，复合任务与任务工厂
* 我们系统中包含六种基础任务：通讯，文件IO，CPU，GPU，定时器，计数器。
* 一切任务都由任务工厂产生，并且在callback之后自动回收。
  * server任务是一种特殊的通讯任务，由框架调用任务工厂产生，通过process函数交给用户。
* 
