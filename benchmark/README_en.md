# Performance test

Sogou C++ Workflow is a network framework with excellent performance. This article demonstrates our performance tests, including the plans, the codes, and the results as well as the comparison with similar products.

This article will be continuously updated with more experiments in additional scenarios.

## HTTP Server

HTTP Client/Server is a common application scenarios in Sogou C++ Workflow.  Let’s begin with the experiment on the servide side. 

### Environments

We deploy two identical machines as the Server and the Client. Their software and hardware configuration are listed as follows:

| Software & hardware | Configuration
|:----------:|:----------
| CPU| 40 Cores, x86\_64, Intel(R) Xeon(R) CPU E5-2630 v4 @ 2.20GHz
| Memory| 192GB
| NIC| 25000Mbps
| Operating system| CentOS 7.8.2003
| Kernel| Linux version 3.10.0-1127.el7.x86\_64
| GCC| 4.8.5

The RTT between the two machines, measured using `ping`, is around 0.1ms. 

### Control group

We use nginx and brpc as the control group. 
We select nginx because it is widely deployed in the production environments, with outstanding performance;
for brpc, in this experiment we focus only on its performance as a HTTP Server, and its other features are tested in more details in [separate experiments][Sogou RPC Benchmark]

In fact, in addition to these two frameworks, we have also experimented with other frameworks at the same time, but the results are widely different, so they are not shown in this article.

In the future, we will include more suitable frameworks for comparison.

### Client tools

In this experiment, the tools for stress test are [wrk][wrk] and [wrk2][wrk2]. 
The former is suitable for testing QPS limits and latency under specific concurrency scenarios, 
and the latter is suitable for testing latency distribution under specific QPS.

We have tried other testing tools, such as [ab][ab], but the stress was not sufficiently strong.  In view of this, we are also developing our own benchmark tools based on Sogou C++ Workflow.

### Variables and indicators

Generally speaking, there are many ways to test the performance of a network framework. We can explore the adaptability of the framework in different scenarios by using different variables and observing the resulting indicators.

This experiment uses the most common variables and indicators: 
we use different degrees of concurrency and different load from the Client to test the QPS and latency changes. 
In addition, we also tested the latency distribution of normal requests when they are sent together with slow requests.

The following paragraphs show two test scenarios.

### Testing methods

#### Starting an HTTP server

1. Compile benchmark
2. Enter the benchmark directory and run

```
./http_server 12 9000 11
```

Note: The starting parameters include: number of threads, port number and the length of the random string in the response.

### Wrk test

```
wrk --latency -d10 -c200 --timeout 8 -t 6 http://127.0.0.1:9000
```

**Parameters:**

-c200: start 200 connections

-t6: start 6 threads for stress test

-d10: set the duration of the stress test to 10 seconds

--timeout 8: set the connection timeout to 8 seconds

### QPS and latency under different degrees of concurrency and different data length

#### Code and configuration

We built an extremely simple HTTP server, 
ignoring all business logic 
and focusing only on the performance of the network framework.

You can see the code snippets below. For the complete code, please visit [benchmark.][benchmark-01 Code]

```cpp
// ...

auto * resp = task->get_resp();
resp->add_header_pair("Date", timestamp);
resp->add_header_pair("Content-Type", "text/plain; charset=UTF-8");
resp->append_output_body_nocopy(content.data(), content.size());

// ...
```

In the above code snippet, 
for each incoming HTTP request, 
the server returns a fixed piece of content as the Body, 
and set the necessary Header, 
including the `content-type` and `date` specified in the code 
and the automatically populated `connection` and `content-length`.

The fixed content in the HTTP Body is an ASCII string generated randomly at the start of the Server, 
whose length can be configured in the startup parameters. 
You can also configure the number of poller threads used and the listened port number at the same time. 
In the experiment, we fixed the number of poller threads to 16. 
Thus, Sogou C++ Workflow used 16 poller threads and 20 handler threads (default configuration).

For nginx and brps, 
we built the same response content; 
and set 40 processes for nginx and 40 threads for brpc.

#### Variables

In the experiment, the degree of concurrency always doubles the previous value in the range of `[1, 2K]`, and the data length is also doubled in the range of `[16B, 64KB]`. They are orthogonally combined,

#### Indicators

As the number of the combination of concurrency and data length is large, we selected part of the data and drawn a curve.

##### Relationship between QPS and degree of concurrency at fixed data length

![Concurrency and QPS][Con-QPS]

The above figure shows that when the data length remains unchanged, 
QPS increases with the increase of concurrency degree, and then becomes stable. 
In this process, Sogou C++ Workflow is obviously better than brpc and nginx. 
In particular, the curves for the data length at 64 or 512 show that an QPS of 500K can be maintained when the concurrency is sufficient.

Note that the curves for nginx-64 and nginx-512 in the above figure are difficult to identify as they overlap very much.

##### Relationship between QPS and data length (fixed degree of concurrency)

![Body Length and QPS][Len-QPS]

The above figure shows that when the concurrency remains unchanged, 
with the increase of data length, 
QPS keeps stable and then drops when the data length exceeds 4K. 
In this process, Sogou C++ Workflow also performs better.

##### Relationship between latency and degree of concurrency at fixed data length

![Concurrency and Latency][Con-Lat]

The above figure shows that when the data length remains unchanged, 
the latency increases with the increase in the degree of concurrency. 
In this process, Sogou C++ Workflow is slightly better than nginx and brpc.

##### Relationship between latency and data length (fixed degree of concurrency)

![Body Length and Latency][Len-Lat]

The above figure shows that when the concurrency remains unchanged, 
with the increase of data length, the latency increases. 
In this process, Sogou C++ Workflow performs better than nginx and brpc.

### Latency distribution with additional slow requests

#### Code

Based on the above tests, we simply added the logic for a slow request to simulate the special circumstances that may appear in the business scenario.

You can see the code snippet below. For the complete code, please visit [benchmark.][benchmark-02 Code]

```cpp
// ...

if (std::strcmp(uri, "/long_req/") == 0)
{
    auto timer_task = WFTaskFactory::create_timer_task(microseconds, nullptr);
    series_of(task)->push_back(timer_task);
}
// ...
```

We performed checks in the process of Server. 
If the request visited a specific path, 
we add one `WFTimerTask` to the end of Series 
to simulate an asynchronous time-consuming process. Similarly, we use `bthread_usleep()` to realize asynchronous sleep in brpc.

#### Configuration

This experiment uses a fixed concurrency of 1024, the data length is 1024 bytes, QPS is 20K, 100K and 200K, respectively for testing normal requests, 
and then draw the delay; 
at the same time, there is another route to send slow requests, whose QPS is 1% of the above QPS. 
The data of the slow requests are not included in the statistics. 
The duration of a slow request is 5ms.

#### Latency CDF 

![Latency CDF][Lat CDF]

The above figure shows that Sogou C++ Workflow is slightly inferior to brpc when QPS is 20K; 
when QPS is 100K, their performance are almost equal;
when QPS is 200K, Sogou C++ Workflow is slightly better than brpc. 
In a word, their performance gap is negligible in this scenario.

[Sogou RPC Benchmark]: https://github.com/holmes1412/sogou-rpc-benchmark ""
[wrk]: https://github.com/wg/wrk ""
[wrk2]: https://github.com/giltene/wrk2 ""
[ab]: https://httpd.apache.org/docs/2.4/programs/ab.html ""
[benchmark-01 Code]: benchmark-01-http_server.cc ""
[benchmark-02 Code]: benchmark-02-http_server_long_req.cc ""
[Con-QPS]: ../docs/img/benchmark-01.png ""
[Len-QPS]: ../docs/img/benchmark-02.png ""
[Con-Lat]: ../docs/img/benchmark-03.png ""
[Len-Lat]: ../docs/img/benchmark-04.png ""
[Lat CDF]: ../docs/img/benchmark-05.png ""