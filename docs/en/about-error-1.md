# About error processing

In any software system, error processing is an important and complex issue. Within this framework, error processing is ubiquitous and extremely cumbersome.

In the interfaces expose to users, we try to make these as simple as possible. While users still inevitably need to understand some error messages.

# Disable C++ exceptions

We do not use C++ exceptions in the framework. When users are compiling code, it’s recommended to add -fno-exceptions to reduce the code size.
Based on the common practice in the industry, we will ignore the possibility of new operation failure, and internally avoid using new to allocate large blocks of memory. The C language style memory allocation is error-checking.

# About factory function

As we can see in the previous examples, all tasks and series are generated from two factory classes: WFTaskFactory or Workflow.

These factory classes, as well as more factory class interfaces that we may encounter in the future, aim to ensure success. In other words, it will ensure no NULL is returned. The user does not need to check the return value.

To achieve this goal, even though URL is invalid, the factory can still generate tasks normally, and get the error again in the callback of the task.

# Task status and error codes

In the previous examples, we often see this code in the callback:

```cpp
void callback(WFXxxTask *task)
{
    int state = task->get_state();
    int error = task->get_error();
    ...
}
```

Among all, state represents the end status of the task. You can refer to [WFTask.h](../src/factory/WFTask.h) for all possible state values:

```cpp
enum
{
    WFT_STATE_UNDEFINED = -1,
    WFT_STATE_SUCCESS = CS_STATE_SUCCESS,
    WFT_STATE_TOREPLY = CS_STATE_TOREPLY,        /* for server task only */
    WFT_STATE_NOREPLY = CS_STATE_TOREPLY + 1,    /* for server task only */
    WFT_STATE_SYS_ERROR = CS_STATE_ERROR,
    WFT_STATE_SSL_ERROR = 65,
    WFT_STATE_DNS_ERROR = 66,                    /* for client task only */
    WFT_STATE_TASK_ERROR = 67,
    WFT_STATE_ABORTED = CS_STATE_STOPPED         /* main process terminated */
};
```

The states deserving our attention:

  * SUCCESS: The task is successful. The client receives the complete reply, or the server writes the reply completely into the sending buffer (but not guarantees that the other party will receive it).
  * SYS_ERROR: System error. In this case, what task->get_error() gets is the system error code errno.
    * When get_error() gets ETIMEDOUT, you can call task->get_timeout_reason() to further receive the timeout reason.
  * DNS_ERROR: DNS resolution error. What get_error() gets is the return code of getaddrinfo() call. For instructions of DNS, see the document [about-dns.md](./about-dns.md).
    * The server task will never has DNS_ERROR.
  * SSL_ERROR: SSL error. What get_error() gets is the return value of SSL_get_error().
    * For now SSL error information is not complete, and the value of ERR_get_error() cannot be obtained. Therefore, basically there are three possible get_error() returns values:
      * SSL_ERROR_ZERO_RETURN, SSL_ERROR_X509_LOOKUP, SSL_ERROR_SSL。
    * We will consider adding more detailed SSL error information in the updated version.
  * TASK_ERROR: Task error. Common task errors include invalid URL, login failure. For return value of get_error() see [WFTaskError.h](../src/factory/WFTaskError.h).

States that users generally don't have to pay attention:

- UNDEFINED: The client task that has just been created but has not yet operate is in UNDEFINED state.
- TOREPLY: Prior to server task reply, and task->noreply() that has not been called, all are in TOREPLY status.
- NOREPLY: The server task after task->noreply() is called will remain in NOREPLY state. The same status in callback. The connection will be closed.

# Other error processing requirements

In addition to the error processing of the task, there is also a need to judge errors on the message interfaces of various specific protocols. Generally, these interfaces indicate an error by returning false, and pass the cause of the error through errno.

In addition, some more complex usages may require access to more complex error messages. We will further describe it in relevant documents.
