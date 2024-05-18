#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

using namespace std;
#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
//使用extern调用http_conn.cpp中的函数,可以避免包含头文件
extern int addfd(int epollfd, int fd, bool if_oneshot);
extern int removefd(int epollfd, int fd);

void addsig(int sig, void(handler)(int), bool restart = true)
{
	struct sigaction sa;
	memset(&sa, '\0', sizeof(sa));
	sa.sa_handler = handler;
	if (restart)
	{
		sa.sa_flags |= SA_RESTART;
	}
	sigfillset(&sa.sa_mask);
	assert(sigaction(sig, &sa, NULL) != -1);
}

void show_error(int connfd, const char* info)
{
	printf("%s", info);
	send(connfd, info, strlen(info), 0);
	close(connfd);
}


int main(int argc, char* argv[])
{
	
	/**/
	/*if (argc <= 2)
	{
		printf("usage: %s ip_address port_number\n", basename(argv[0]));
		return 1;
	}
	const char* ip = argv[1];
	int port = atoi(argv[2]);*/
	const char* ip ="192.168.88.130";
	int port = 12345;

	
/*在网络编程中，SIGPIPE这个信号是很常见的。当往一个写端关闭的管道或socket连接中连续写入数据时会引发SIGPIPE信号,
引发SIGPIPE信号的写操作将设置errno为EPIPE。在TCP通信中，当通信的双方中的一方close一个连接时，
若另一方接着发数据，根据TCP协议的规定，会收到一个RST响应报文，若再往这个服务器发送数据时，
系统会发出一个SIGPIPE信号给进程，告诉进程这个连接已经断开了，不能再写入数据。*/
	addsig(SIGPIPE, SIG_IGN);//忽略sigpipe信号
	
	/*创建线程池*/
	threadpool<http_conn>* pool = NULL;
	try
	{
		pool = new threadpool<http_conn>;
	}
	catch (const std::exception&)
	{
		return 1;//代表非正常终止
	}

	
	/*预先为每一个可能的客户连接，分配一个http_conn对象，也就是事件处理的类*/
	http_conn* users = new http_conn[MAX_FD];

	assert(users);//断言，assert(condition)，condition若为false会中断程序，这里判断users是否成功分配到了内存空间

	int user_count = 0;


	int listenfd = socket(PF_INET, SOCK_STREAM, 0);
	assert(listenfd >= 0);//断言
	struct linger tmp = { 1,0 };//满足下一行setsockopt的第3，4个参数
	/*SO_LINGER，如果选择此选项, close或 shutdown将等到所有套接字里排队的消息成功发送或到达延迟时间后>才会返回. 否则, 调用将立即返回。*/
	setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

	struct sockaddr_in address;//存放ip地址和端口号的结构体
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &address.sin_addr);//转为网络字节序，并存入address的ip里面
	address.sin_port = htons(port);


	int ret = 0;
	ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
	assert(ret >= 0);

	ret = listen(listenfd, 5);//第二位参数为backlog，意为最大监听长度，一般设置为5
	assert(ret >= 0);

	epoll_event events[MAX_EVENT_NUMBER];
	int epollfd = epoll_create(5);
	assert(epollfd!=-1);
	addfd(epollfd, listenfd, false);
	http_conn::m_epollfd = epollfd;//写完httpconn在回头看

	while (true)
	{
		int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
		
		if ((number < 0) && (errno != EINTR))//在读写操作时，遇到中断会将errno置为0，这里若并非为0，说明处理错误
		{
			printf("epoll_failure\n");
			break;
		}

		for (int i = 0; i < number; i++)
		{
			int sockfd = events[i].data.fd;
			if (sockfd == listenfd)
			{
				struct sockaddr_in client_address;
				socklen_t client_addrlength = sizeof(client_address);
				int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
				if (connfd < 0)
				{
					printf("accept error");
					continue;
				}
				if (http_conn::m_user_count >= MAX_FD)
				{
					show_error(connfd, "Internal server busy");
					continue;
				}
				/*如果没有以上问题，则开始初始化*/
				users[connfd].init(connfd, client_address);
				printf("initialize\n");

			}
			else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
			{
				/*如果有异常，直接关闭客户连接*/
				users[sockfd].close_conn();
			}
			else if (events[i].events & EPOLLIN)
			{
				/*如果没有内容可读，关闭连接，反之加入线程池*/
				if (users[sockfd].read())//这里调用read函数
				{
					
					pool->append(users + sockfd);

				}
				else
				{
					users[sockfd].close_conn();
				}
			}
			else if (events[i].events & EPOLLOUT)
			{
				if (!users[sockfd].write())
				{
					users[sockfd].close_conn();
				}
			}
			else
			{

			}

		}

	}


	close(epollfd);//占用了文件描述符，需要关闭回收
	close(listenfd);
	delete[]users;
	delete pool;
	return 0;


}
