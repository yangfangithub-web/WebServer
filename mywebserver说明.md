

# mywebserver

## 一、主要内容

### 1. 项目流程图



- 通过传参获取端口号

- 设置PIPE信号捕捉和忽略处理

  ```
  当往一个写段关闭管道，或者socket连接中连续写入数据时会触发SIGPIPE信号，或者TCP通信四次挥手半关闭状态时，服务器仍旧发送两次数据，系统会发出SIGPIPE信号。
  SIGPIPE信号默认是结束进程。
  我们不希望服务器因为写错误而结束进程，因此对该信号设置忽略。
  	signal(SIGPIPE,SIG_IGN);
  ```

- 创建socket套接字
- 设置端口复用setsockopt
- 绑定服务器IP+端口，监听

- 创建epoll对象，检测读/写/新连接事件，添加监听文件描述符

- 创建管道，用于对SIGALARM和SIGTERM信号捕捉通知，epoll检测管道读端

- 初始化线程池

- 初始化客户端数组users

- 设定定时器

  ```
  定时产生SIGALARM信号，信号捕捉到会将该信号写入管道
  epoll检测到会进行处理
  ```

- while(!stop_server)循环epoll_wait阻塞等待事件发生

```
遍历事件数组，处理逻辑
a.有新连接进来
	-accept()接受客户端连接
	-判断客户端数量是否已满
		-满了，则通知关闭与客户端通信的文件描述符，continue
		-没满，新客户加入客户端数组，初始化init(sockfd,客户端地址)，添加到epoll对象中，创建定时器
b.客户端读事件EPOLLIN
	-read(),一次性读完所有数据
	-加入线程池请求队列
c.客户端写事件EPOLLOUT
	-write(),一次性写完所有数据
	-重置为EPOLLIN事件检测
d.客户端异常
	-EPOLLRDHUP|EPOLLHUP|EPOLLERR
	-关闭连接
e.管道读端的EPOLLIN事件
	-SIGALARM信号，设置timeout标志位为true
	-SIGTERM信号，设置stop_server标志位为true
```

- timeout检测定时器链表超时情况，并断开非活跃连接

```
	-调用tick()函数
	-重新计时alarm
	-重置timeout为false
```

- 关闭文件描述符，释放资源



### 2. 线程池工作原理

- sem_wait()等待信号量>0，可消费

- 请求队列上锁，取出队列头的任务，解锁

- process()执行用户请求任务

  ```
  1. process_read()：根据主状态机，解析HTTP请求，
  	-解析请求首行
  	-解析请求头
  	-解析请求体
  获得完整请求则返回状态机GET_REQUEST
  2. do_request(),执行用户文件操作
  	-分析目标文件的属性如果目标文件存在，对所有用户可读，且不是目录
  	-则使用mmap将其映射到内存地址m_file_address处，
  	-并告诉调用者获取文件成功，返回状态机FILE_REQUEST
  3. process_write(read_ret),生成根据服务器处理HTTP请求的结果生成响应
  	-根据read_ret结果对应的状态码往写缓冲区m_write_buf写待发送的数据，包括响应行，响应头，响应体,第一块写缓冲区都要写m_iv[0]
  	-如果read_ret为FILE_REQUEST，第二块写缓冲区写客户请求的目标文件
  	-写缓冲区里的数据会在main()调用的write()里一次性写完
  4. 重置sockfd
  	-如果第二布返回NO_REQUEST,则重置sockfd的EPOLLIN|EPOLLONESHOT事件继续监听
  	-如果第第三部执行完了，重置sockfd的EPOLLOUT|EPOLLONESHOT事件
  ```

### 3. 超时检测模块工作原理

- 监听套接字连接前设置定时器，定时产生SIGALRAM信号

  ```
  -初始化新接收的连接时，创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
  -当某个定时任务发生变化时，调整对应的定时器在链表中的位置。
  ```

- 信号捕捉SIGALARM信号，并写入管道

- epoll检测到读管道的EPOLLIN事件表示有SIGALARM信号

- 设置timeout标志位

- 执行一次 tick() 函数，以处理链表上到期任务。

  ```
  -从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
  -当前未超时，则后面节点也不超时
  -否则调用定时器的回调函数，以执行定时任务，关闭连接
  ```

  

## 二、技术难点

1. 采用TCP协议，使用 socket（） 实现服务器和浏览器客户端的通信；
2. 使用epoll事件检测技术实现IO多路复用，提高运行效率
3. 采用Proactor的事件处理模式，利用线程池实现多线程机制，实现高并发通信，减少频繁创建和销毁线程带来的开销。
4. 主进程负责事件的读写，子进程负责业务逻辑——用有限状态机解析HTTP请求报文；生成响应报文。
5. 利用链表数据结构实现超时检测处理。

## 三、压力测试

1. 测试1

   终端输入

   ```
   cd test_presure/webbench-1.5/
   ./webbench -c 1000 -t 60 http://192.168.136.128:10000/index.html
   ```

   终端输出

   ```
   Benchmarking: GET http://192.168.136.128:10000/index.html
   1000 clients, running 60 sec.
   
   Speed=339613 pages/min, 899974 bytes/sec.
   Requests: 339613 susceed, 0 failed.
   ```

   

2. 测试2

​		终端输入

```
cd test_presure/webbench-1.5/
./webbench -c 7000 -t 5 http://192.168.136.128:10000/index.html
```

​		终端输出

```
Benchmarking: GET http://192.168.136.128:10000/index.html
7000 clients, running 5 sec.

Speed=530160 pages/min, 1404828 bytes/sec.
Requests: 44180 susceed, 0 failed.
```



