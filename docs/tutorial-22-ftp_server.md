# FTP服务器：ftp_server

## 示例代码
[tutorial-22-ftp_server.cc](/tutorial/tutorial-22-ftp_server.cc)

## 关于ftp_server
ftp_server把一个本地目录作为FTP服务的根目录，用户名和密码由命令行指定：
~~~sh
./ftp_server 2121 /data/ftp user password
~~~
之后可以用任意FTP客户端访问，比如[ftp_cli](/docs/tutorial-21-ftp_cli.md)或curl：
~~~sh
./ftp_cli ls ftp://user:password@127.0.0.1:2121/
curl -T a.txt ftp://user:password@127.0.0.1:2121/a.txt
~~~
按Ctrl-C结束服务。

## 创建FTP服务器
~~~cpp
WFFtpServer server("/data/ftp", [](const std::string& user,
                                   const std::string& password) {
    return user == "user" && password == "password";
});

if (server.start(2121) == 0)
{
    ...
    server.stop();
}
~~~
和其它server不同，FTP服务器不需要process函数，协议由框架处理。构造函数的参数是根目录和认证函数：所有用户看到的'/'都是这个根目录；认证函数在网络线程里调用，不要在里面阻塞。

服务器参数分为两部分。`WFServerParams`和其它server一样，FTP的默认值`FTP_SERVER_PARAMS_DEFAULT`把空闲控制连接的超时设为5分钟，一行命令最长8KB。数据连接的参数在`WFFtpDataParams`里：
~~~cpp
struct WFFtpDataParams
{
    size_t max_transfers;        /* 传输线程的数量 */
    int timeout;                 /* 等待数据连接，以及每次读写的超时 */
    unsigned short port_first;   /* 被动模式的端口范围，0表示任意端口 */
    unsigned short port_last;
};
~~~
需要修改时使用另一个构造函数：
~~~cpp
struct WFFtpDataParams params = FTP_DATA_PARAMS_DEFAULT;

params.port_first = 30000;
params.port_last = 30100;
WFFtpServer server(&FTP_SERVER_PARAMS_DEFAULT, &params, "/data/ftp", auth);
~~~

## 支持的命令
* 登录和会话：USER、PASS、QUIT、FEAT、SYST、NOOP、OPTS UTF8、TYPE、MODE、STRU。
* 目录和文件：PWD、CWD、CDUP、MKD、RMD、DELE、RNFR/RNTO、SIZE、MDTM。
* 数据传输：PASV、EPSV、LIST、NLST、RETR、STOR、APPE。

PORT、EPRT、REST、MLSD、AUTH等命令回复502，表示没有实现。

## 实现原理
* 欢迎消息：FTP连接建立后由服务器先发送220，而workflow的server收到请求后才回复。和`WFMySQLServer`发送握手包的做法一样，`WFFtpServer`在`new_connection()`里直接写出220。
* 会话状态：用户名、登录状态、当前目录、被动模式的监听socket等，保存在连接上下文里，只在process里访问。
* 两次回复：RETR等传输命令先回复150，传输结束后再回复226。150通过server任务的`push()`立即发出，数据传输作为后续任务放进任务的series，226是这个请求的回复。
* 数据连接：被动模式下，客户端连上数据端口后不发送任何数据，服务器要主动写，而workflow的server要收到请求才会产生任务。所以数据连接在服务器私有的线程池里完成，线程数就是同时进行的传输数量的上限，超过时排队等待，不占用计算线程。
* 根目录：路径先按当前目录规范化，'..'不会超出'/'；再检查真实路径，指向根目录以外的符号链接也不能访问。
* 数据连接只接受控制连接的同一个客户端地址，防止被其它主机抢先连接。

## 注意事项
* 关闭连接：连接后等待第一条命令的时间是`peer_response_timeout`，之后命令之间的空闲时间是`keep_alive_timeout`。超时或者服务器停止时，先回复421再关闭连接。QUIT和登录失败次数过多时，回复221或530之后直接关闭。
* workflow的server在一个请求回复之前收到新的请求时会关闭连接。所以传输过程中客户端发送ABOR，控制连接会被关闭，传输随之结束。常用客户端在一个命令回复之前不会发送下一个命令，不受影响。
* TYPE A和TYPE I一样按原样传输，不做换行符转换。
* 被动模式回复的是控制连接的本地地址，服务器在NAT后面时，客户端需要使用EPSV，或者像ftp_cli一样忽略PASV回复里的地址。
* 目前不支持主动模式、断点续传、MLSD、匿名登录开关、只读模式和FTPS。
