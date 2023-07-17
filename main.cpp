#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <error.h>
#include <fcntl.h>      
#include <sys/epoll.h>
#include <signal.h>
#include <assert.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"
#include "log.h"

#define MAX_FD 65535 //最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 //监听的最大事件数量

static int pipefd[2]; // 管道文件描述符 0为读，1为写

//添加文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
//删除文件描述符
extern void removefd(int epollfd, int fd);
//修改文件描述符
extern void modfd(int epollfd,int fd, int ev);
// 文件描述符设置非阻塞操作
extern void setnonblocking(int fd);

//添加信号捕捉
void addsig(int sig,void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_flags = 0;
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL) != -1);
}

//向管道写数据的信号捕捉回调函数
void sig_to_pipe(int sig){
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

int main(int argc, char* argv[]) {


    if (argc<=1) {  // 形参个数，第一个为执行命令的名称
        printf("按照如下格式运行: %s port_number\n",basename(argv[0]));
        return 1;
    }
    //获取端口号
    int port = atoi(argv[1]);//字符串转整型
    //对SIGPIE信号进行处理
    addsig(SIGPIPE,SIG_IGN);

    //初始化线程池
    threadpool <http_conn>* pool = NULL;//request是http_conn类的
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        return 1;
    }
    //创建数组，保存所有用户端的信息
    http_conn* users=new http_conn[MAX_FD];


    //创建套接字
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert( listenfd >= 0 );                            // ...判断是否创建成功
    
    int ret = 0;
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr=INADDR_ANY;
    address.sin_port = htons(port);

    //设置端口复用
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    //绑定
    ret = bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    assert( ret != -1 );    // ...判断是否成功

    //监听
    ret = listen(listenfd,5);
    assert( ret != -1 );    // ...判断是否成功

    //创建epoll对象
    //事件数组
    //添加监听的文件描述符
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    //将监听的文件描述符添加到epoll对象中
    addfd (epollfd,listenfd,false);
    http_conn::m_epollfd = epollfd;  // 静态成员，类共享

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );               // 写管道非阻塞
    addfd(epollfd, pipefd[0], false); // epoll检测读管道

    // 设置信号处理函数
    addsig(SIGALRM, sig_to_pipe);   // 定时器信号
    addsig(SIGTERM, sig_to_pipe);   // SIGTERM 关闭服务器
    bool stop_server = false;       // 关闭服务器标志位

    bool timeout = false;   // 定时器周期已到
    alarm(TIMESLOT);        // 定时产生SIGALRM信号

    //主线程不断取循环检测事件的发生
    while (!stop_server) {
        int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if ((number<=0) && (errno != EINTR)) {
            EMlog(LOGLEVEL_ERROR,"EPOLL failed.\n");
            break;
        }

        for (int i=0;i<number;i++) {
            int sockfd= events[i].data.fd;
            if (sockfd == listenfd) {
                //有新的连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd,(struct sockaddr*)&client_address, &client_addrlength);

                if (connfd <0) {
                    printf("errno is: %d\n",errno);
                    continue;
                }

                if (http_conn ::m_user_count >= MAX_FD) {
                    //目前连接数满了
                    close(connfd);
                    continue;
                }
                //将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd,client_address);  
                   
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP |EPOLLERR)) {
                //异常，关闭连接
                EMlog(LOGLEVEL_DEBUG,"-------EPOLLRDHUP | EPOLLHUP | EPOLLERR--------\n");
                users[sockfd].close_conn();
                http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
            } else if (events[i].events & EPOLLIN) {
                //读数据
                EMlog(LOGLEVEL_DEBUG,"-------EPOLLIN-------\n\n");
                if (users[sockfd].read()) {//一次性读完所有数据
                    pool->append(users + sockfd);//读完之后要解析请求，所以放到线程池的请求队列里去
                } else {
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            } else if (events[i].events&EPOLLOUT) {
                //写数据
                EMlog(LOGLEVEL_DEBUG, "-------EPOLLOUT--------\n\n");
                if (!users[sockfd].write()) { //一次性写完所有数据
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            } else if (sockfd == pipefd[0] && (events[i].events & EPOLLIN)) {
                // 读管道有数据，SIGALRM 或 SIGTERM信号触发
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){
                    continue;
                } else if(ret == 0){
                    continue;
                } else{
                    for(int i = 0; i < ret; ++i){
                        switch (signals[i]) // 字符ASCII码
                        {
                        case SIGALRM:
                        // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                        // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                            timeout = true;
                            break;
                        case SIGTERM:
                            stop_server = true;
                        }
                    }
                }

            }

        }
    
        if (timeout) {
            // 定时处理任务，实际上就是调用tick()函数
            http_conn::m_timer_lst.tick();
             // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
            alarm(TIMESLOT);
            timeout = false;    // 重置timeout
        }
    }


    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete pool;
    return 0;
}