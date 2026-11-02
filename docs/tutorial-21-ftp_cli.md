# 异步FTP客户端：ftp_cli

## 示例代码
[tutorial-21-ftp_cli.cc](/tutorial/tutorial-21-ftp_cli.cc)

## 关于ftp_cli
ftp_cli是一个命令行FTP客户端，可以列目录、下载、上传、重命名，或者发送任意FTP命令：
~~~sh
./ftp_cli ls ftp://user:password@127.0.0.1:21/pub
./ftp_cli get ftp://public.nlm.nih.gov/nlmdata/.dailymed/sitemap2.xml.gz > sitemap2.xml.gz
./ftp_cli put ftp://user:password@127.0.0.1:21/pub/b.txt < b.txt
./ftp_cli mv ftp://user:password@127.0.0.1:21/pub/b.txt /pub/c.txt
./ftp_cli cmd ftp://user:password@127.0.0.1:21 MKD /pub/dir
~~~
服务器的回复打印在stderr，ls和get得到的数据打印在stdout。下载的文件边接收边写到stdout，不会整个放在内存里。

## 创建FTP任务
和http任务一样，FTP任务可以通过url直接创建：
~~~cpp
class WFFtpTaskFactory
{
public:
    static WFFtpTask *create_get_task(const std::string& url,
                                      ftp_callback_t callback);

    static WFFtpTask *create_list_task(const std::string& url,
                                       ftp_callback_t callback);

    static WFFtpTask *create_put_task(const std::string& url,
                                      const void *buf, size_t size,
                                      ftp_callback_t callback);

    static WFFtpTask *create_rename_task(const std::string& url,
                                         const std::string& to,
                                         ftp_callback_t callback);

    static WFFtpTask *create_command_task(const std::string& url,
                                          const std::string& command,
                                          const std::string& argument,
                                          ftp_callback_t callback);

    static void set_max_connections(size_t max);
};
~~~
* create_get_task和create_put_task：分别对应RETR和STOR，上传时也可以在任务启动前通过`task->get_req()->set_command()`换成APPE进行追加。
* create_list_task：默认使用LIST命令，也可以换成NLST或MLSD。
* create_rename_task：依次发送RNFR和RNTO，url是原文件，`to`是新的路径。
* create_command_task：发送一个不需要数据连接的命令，例如PWD、MKD、RMD、DELE、SIZE、MDTM、NOOP等，url里的路径不使用。由客户端管理的命令，例如USER、PASV、RETR、TYPE等，会返回WFT_ERR_FTP_COMMAND_DISALLOWED错误。

url的格式是`ftp://[user[:password]@]host[:port]/path`。没有用户名时使用anonymous登录，端口默认为21。按照RFC 1738，url里的路径是相对于登录目录的，比如`ftp://host/pub/a.txt`下载的是登录目录下的`pub/a.txt`。如果要使用绝对路径，开头写成`%2F`，比如`ftp://host/%2Fpub/a.txt`。路径里的其它特殊字符也需要url编码，比如空格写成`%20`。

FTP的控制连接是有状态的：一条连接需要先登录，一次文件传输还需要先在控制连接上协商数据连接的端口，所以一条控制连接同时只能执行一个任务。框架为每个服务器和用户维护最多4条控制连接，任务拿到空闲的连接才开始执行，否则排队等待。连接只在第一次使用时登录，之后保持长连接。这个上限可以通过`set_max_connections()`修改，对之后第一次访问的服务器生效。

## 在同一条连接上执行任务
通过url创建的任务可能使用不同的连接，所以不能依赖CWD这样的连接状态。如果需要在一条连接上依次执行一组任务，可以使用`WFFtpClient`：
~~~cpp
WFFtpClient client(1);

if (client.init("ftp://user:password@127.0.0.1:21") < 0)
{
    perror("init");
    exit(1);
}

WFFtpTask *task = client.create_get_task("pub/a.txt", callback);
~~~
和`WFMySQLConnection`一样，构造函数的参数是连接的id，id和url都相同的客户端共享同一条连接，所以每个客户端请使用不同的id。客户端的任务接口和`WFFtpTaskFactory`类似，参数是路径而不是url，路径不需要url编码。另外还有`create_quit_task()`，用于发送QUIT并关闭连接，之后的任务会重新建立连接并登录。

FTP任务可以和其它任务一样放进任务流，也可以同时启动多个任务，它们会在控制连接上排队依次执行。

## 处理结果
~~~cpp
void ftp_callback(WFFtpTask *task)
{
    protocol::FtpResponse *resp = task->get_resp();

    if (task->get_state() != WFT_STATE_SUCCESS)
    {
        // 网络错误，或者登录失败等。登录失败时resp里仍然是服务器的回复。
        ...
    }
    else if (resp->get_reply_code() / 100 == 2)
    {
        // 成功。ls和get的数据在resp->get_data()里。
        ...
    }
    else
    {
        // 服务器的否定回复，比如550文件不存在。
        ...
    }
}
~~~
与http任务收到404时任务状态依然是成功类似，FTP服务器的否定回复也不是任务错误，需要检查回复码。`get_reply_code()`和`get_reply_text()`是最终的回复，例如下载时的226；如果之前有150这样的预备回复，可以通过`get_preliminary_code()`和`get_preliminary_text()`得到。多行回复的文本以'\n'连接，不包含回复码。

任务错误码在[WFTaskError.h](/src/factory/WFTaskError.h)中。url不合法时是WFT_ERR_URI_PARSE_FAILED，协议不是ftp时是WFT_ERR_URI_SCHEME_INVALID。FTP相关的有：
* WFT_ERR_FTP_SERVICE_UNAVAILABLE：连接后服务器的欢迎消息不是2xx。
* WFT_ERR_FTP_LOGIN_FAILED：USER或PASS被拒绝。
* WFT_ERR_FTP_TYPE_FAILED：服务器不接受二进制传输模式TYPE I。
* WFT_ERR_FTP_PASSIVE_FAILED：EPSV和PASV都失败，或者回复无法解析。
* WFT_ERR_FTP_COMMAND_DISALLOWED：命令由客户端管理，不允许直接发送。

## 流式下载
默认情况下，下载和列目录得到的数据保存在`resp->get_data()`里，可以通过`task->get_resp()->set_size_limit()`限制数据的大小。下载大文件时，可以在任务启动前设置extract函数，每收到一块数据就调用一次，数据不再保存在resp里：
~~~cpp
WFFtpTask *task = WFFtpTaskFactory::create_get_task(url, callback);

task->set_extract([fp](WFFtpTask *task, const void *buf, size_t size) {
    fwrite(buf, 1, size, fp);
});
task->start();
~~~
extract函数在网络线程里调用，不要在里面做阻塞或耗时的操作。设置了extract之后，数据的大小不受size limit的限制。数据是否完整，仍然以callback里的任务状态和226等回复为准。

## 实现原理
* 控制连接：新连接上先接收服务器的220欢迎消息，然后依次发送USER、PASS和TYPE I。登录状态保存在连接上下文里，所以长连接上之后的任务不会重复登录。
* 连接管理：每条控制连接对应一个会话，同一个服务器和用户的会话放在一个资源池（`WFResourcePool`）里。任务先从资源池取得一个会话，再在这个会话的连接上执行，结束后归还。`WFFtpClient`相当于只有一个会话的服务器，两种接口使用同样的流程。
* 数据连接：只使用被动模式。客户端优先发送EPSV，服务器不支持时改用PASV。不论使用哪个命令，数据连接都连向控制连接的服务器地址，避免服务器在NAT后面时回复的是内网地址。
* 传输顺序：数据连接在RETR、STOR、LIST等命令发出之后才建立，这样服务器总是先收到命令。下载时以服务器关闭数据连接作为数据的结束，上传时客户端发完数据后关闭连接，传输是否成功以控制连接上的226等回复为准。
* 中止传输：如果传输命令没有收到1xx预备回复而直接失败，服务器不会使用这条数据连接，客户端会关闭控制连接，让服务器释放被动端口，数据连接随之结束。

## 注意事项
* 连接被服务器关闭后会自动重连并重新登录，但CWD设置的工作目录会丢失，建议使用`WFFtpClient`时也使用绝对路径。
* 上传的数据需要全部放在内存里。
* 目前不支持FTPS和主动模式（PORT/EPRT）。
