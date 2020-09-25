# A simple user-defined protocol client/server

# Sample code

[message.h](../tutorial/tutorial-10-user_defined_protocol/message.h)  
[message.cc](../tutorial/tutorial-10-user_defined_protocol/message.cc)  
[server.cc](../tutorial/tutorial-10-user_defined_protocol/server.cc)  
[client.cc](../tutorial/tutorial-10-user_defined_protocol/client.cc)

# About user_defined_protocol

This example designs a simple communication protocol, and constructs server and client on the protocol. The server converts the message sent by the client into uppercase and then returns.

# Protocol format

The protocol message contains a 4-byte head and a message body. Head is an integer in network order, indicating the length of the body.

The request and response messages are in the same format.

# Implementation of protocol

The user-defined protocol needs to provide the serialization and deserialization methods of the protocol, both of which are virtual functions of ProtocolMeessage class.

In addition, for ease of use, we strongly recommend users to implement the move construction and move assignment of messages (used for std::move()). In [ProtocolMessage.h](../src/protocol/ProtocolMessage.h), the serialization and deserialization interfaces are as follows:

~~~cpp
namespace protocol
{

class ProtocolMessage : public CommMessageOut, public CommMessageIn
{
private:
    virtual int encode(struct iovec vectors[], int max);

    /* You have to implement one of the 'append' functions, and the first one
     * with arguement 'size_t *size' is recommmended. */
    virtual int append(const void *buf, size_t *size)；
    virtual int append(const void *buf, size_t size);

    ...
};

}
~~~

# Serialization function encode

  * The encode function is called before the message is sent, and it is called only once for each message.
  * In encode function, user needs to serialize the message to a vector array, and the number of array elements does not exceed max. The current max value is 8192.
  * For the definition of structure structiovec, please reference to the system to call readv and writev.
  * When the encode function is correct, the return value ranges between 0 and max, indicating how many vectors are used in the message.
    * In case of UDP protocol, please note that the total length does not exceed 64k, and use no more than 1024 vectors (In Linux, maximum 1024 vectors are allowed for each writev).
      * The UDP protocol can only be used for client and cannot implement UDP server.
  * Encode returns -1 indicates an error. When returns to -1, errno needs to be set. If return value is >max, you will get an EOVERFLOW error. All errors are obtained in the callback.
  * For the sake of performance, the content that the iov_base pointer of vector points will not be copied. So generally it points to the members of message class.
  
# Deserialization function append

  * The append function is called every time a data block is received. Therefore, each message may be called for several times.
  * Buf and size are the content and length of data block received respectively. User needs to copy the data content.
    * If append(const void *buf, size_t *size) interface is implemented, you can modify *size to tell the framework the length consumed this time. Received size-consumed size = remaining size, the remaining part of buf will be received again when append is called up next time. This function is more convenient for protocol analysis. Of course, users can copy them all for self management, in such a case, no need to *size.
    * In case of UDP protocol, one append must be a complete data packet.
  * If append function returns 0, it means that the message is not complete and the transmission continues. If it returns 1, it means the message end. -1 means an error, in such a case, errno needs to be set.
  * In short, the role of append is to tell the frame whether the message transmission is ended or not. Don't perform complex, unnecessary protocol analysis in append.
  
# Errno settings

  * If encode or append returns -1 or other negative numbers, it will mean a failure, in such a case, the reason for the error needs to be transmitted through errno. The user will get this error in the callback.
  * In case of a failure in system call or library function such as libc (such as malloc), libc will definitely set errno well, and the user does not need to set it again.
  * Some errors with illegal messages are very common. For example, EBADMSG and EMSGSIZE represent wrong message content and too large message respectively.
  * User can select a value beyond the system-defined errno range to indicate some custom errors. Generally, values larger than 256 are allowed.
  * Do not use negative errno. Because the framework internally uses negative numbers to represent SSL errors.
  
In our example, both serialization and deserialization of messages are very simple. 

The header file [message.h](../tutorial/tutorial-10-user_defined_protocol/message.h) clarifies request and response classes:

~~~cpp
namespace protocol
{

class TutorialMessage : public ProtocolMessage
{
private:
    virtual int encode(struct iovec vectors[], int max);
    virtual int append(const void *buf, size_t size);
    ...
};

using TutorialRequest = TutorialMessage;
using TutorialResponse = TutorialMessage;

}
~~~

Request and response classes are the same type of message. You can use it directly.

Note that request and response must be able to be constructed without parameters, that is to say, no-argument constructor or no constructor is required.

[message.cc](../tutorial/tutorial-10-user_defined_protocol/message.cc) contains the instructions on the implementation of encode and append:

~~~cpp
namespace protocol
{

int TutorialMessage::encode(struct iovec vectors[], int max/*max==8192*/)
{
    uint32_t n = htonl(this->body_size);

    memcpy(this->head, &n, 4);
    vectors[0].iov_base = this->head;
    vectors[0].iov_len = 4;
    vectors[1].iov_base = this->body;
    vectors[1].iov_len = this->body_size;

    return 2;    /* return the number of vectors used, no more then max. */
}

int TutorialMessage::append(const void *buf, size_t size)
{
    if (this->head_received < 4)
    {
        size_t head_left;
        void *p;

        p = &this->head[this->head_received];
        head_left = 4 - this->head_received;
        if (size < 4 - this->head_received)
        {
            memcpy(p, buf, size);
            this->head_received += size;
            return 0;
        }

        memcpy(p, buf, head_left);
        size -= head_left;
        buf = (const char *)buf + head_left;

        p = this->head;
        this->body_size = ntohl(*(uint32_t *)p);
        if (this->body_size > this->size_limit)
        {
            errno = EMSGSIZE;
            return -1;
        }

        this->body = (char *)malloc(this->body_size);
        if (!this->body)
            return -1;

        this->body_received = 0;
    }

    size_t body_left = this->body_size - this->body_received;

    if (size > body_left)
    {
        errno = EBADMSG;
        return -1;
    }

    memcpy(this->body, buf, body_left);
    if (size < body_left)
        return 0;

    return 1;
}

}
~~~

The implementation of encode is very simple. Two fixed vectors are used, which point to head and body respectively. Note that the iov_base pointer must point to a member of the message class.

Append needs to ensure that the 4-byte head is received completely, and then read the message body. In addition, we cannot guarantee that the first append will definitely include the complete head, so the process is a little cumbersome.

Append implements size_limit function, if the size_limit is exceeded, an EMSGSIZE error will be returned. If users do not need to limit the message size, they can ignore the size_limit field.

Since we require the communication protocol to be back and forth, the so-called "TCP sticky packet" problem does not need to take into account, instead it is directly handled as an error message.

Now, as we know the message definition and implementation, we can build server and client.

# Definition of server and client

With request and response class, we can build servers and clients based on this protocol. In the previous example, we described the type definitions related to the Http protocol:

~~~cpp
using WFHttpTask = WFNetworkTask<protocol::HttpRequest,
                                 protocol::HttpResponse>;
using http_callback_t = std::function<void (WFHttpTask *)>;

using WFHttpServer = WFServer<protocol::HttpRequest,
                              protocol::HttpResponse>;
using http_process_t = std::function<void (WFHttpTask *)>;

Similarly, for this Tutorial protocol, there is no difference in the definition of data types:
using WFTutorialTask = WFNetworkTask<protocol::TutorialRequest,
                                     protocol::TutorialResponse>;
using tutorial_callback_t = std::function<void (WFTutorialTask *)>;

using WFTutorialServer = WFServer<protocol::TutorialRequest,
                                  protocol::TutorialResponse>;
using tutorial_process_t = std::function<void (WFTutorialTask *)>;
~~~

# Server

The server has no difference from http server. Prioritize IPv6 start, which does not affect IPv4 client requests. In addition, the request is limited to 4KB.

Refer to [server.cc](../tutorial/tutorial-10-user_defined_protocol/server.cc) for the codes.

# Client

The client logic is to receive user input from standard IO, construct a request and send it to the server for the result.

The reading Standard input process is completed in the callback, so we will first issue an empty request. Also for the sake of safety, we limit the server reply package to 4KB.

The only thing we should learn about client is how to generate a client task with user-defined protocol, there are three interfaces to choice in [WFTaskFactory.h](../src/factory/WFTaskFactory.h):

~~~cpp
template<class REQ, class RESP>
class WFNetworkTaskFactory
{
private:
    using T = WFNetworkTask<REQ, RESP>;

public:
    static T *create_client_task(TransportType type,
                                 const std::string& host,
                                 unsigned short port,
                                 int retry_max,
                                 std::function<void (T *)> callback);

    static T *create_client_task(TransportType type,
                                 const std::string& url,
                                 int retry_max,
                                 std::function<void (T *)> callback);

    static T *create_client_task(TransportType type,
                                 const URI& uri,
                                 int retry_max,
                                 std::function<void (T *)> callback);
    ...
};
~~~

Among all, TransportType specifies the transport layer protocol. The currently available values include TT_TCP, TT_UDP, TT_SCTP and TT_TCP_SSL.

There is little difference between the three interfaces. In this example, URL is not needed for the time being. We use domain name and port to create tasks.

The actual calling codes are as follows. We derive the WFTaskFactory class, but this derivation is not essential.

~~~cpp
using namespace protocol;

class MyFactory : public WFTaskFactory
{
public:
    static WFTutorialTask *create_tutorial_task(const std::string& host,
                                                unsigned short port,
                                                int retry_max,
                                                tutorial_callback_t callback)
    {
        using NTF = WFNetworkTaskFactory<TutorialRequest, TutorialResponse>;
        WFTutorialTask *task = NTF::create_client_task(TT_TCP, host, port,
                                                       retry_max,
                                                       std::move(callback));
        task->set_keep_alive(30 * 1000);
        return task;
    }
};
~~~

As shown above, we used WFNetworkTaskFactory<TutorialRequest, TutorialResponse> class to create client tasks.

Next, maintain the connection for 30 seconds after the communication is completed through the set_keep_alive() interface of the task, otherwise, short connection will be used by default.

The knowledge about other codes of client has been provided in the previous examples. Please refer to [client.cc](../tutorial/tutorial-10-user_defined_protocol/client.cc).

# How to generate the request of built-in protocol

Now the system embeds four built-in protocols: http, redis, mysql, and kafka. Can we generate an http or redis task by the same way? For example:
~~~cpp
WFHttpTask *task = WFNetworkTaskFactory<protocol::HttpRequest, protocol::HttpResponse>::create_client_task(...);
~~~

It should be noted that the http task generated in this way will lose a lot of functions, for example, it is impossible to identify whether a persistent connection is used according to header, or to identify and redirect, etc.

Similarly, if a MySQL task is generated in this way, it may not be able to run at all. Because login authentication process is absent.

A Kafka request may require a complex process of interaction with multiple brokers, obviously the request created cannot complete this process.

It’s clear that the generation process of each kind of built-in protocol message is far more complicated than this example. Similarly, if user needs to implement a more functional communication protocol, they need to write extra codes.
