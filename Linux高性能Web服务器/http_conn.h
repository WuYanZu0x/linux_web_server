#pragma once
#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
class http_conn
{
public:
	static const int FILENAME_LEN = 200;
	/*读缓冲区的大小*/
	static const int READ_BUFFER_SIZE = 2048;
	/*写缓冲区的大小*/
    static const int WRITE_BUFFER_SIZE = 1024;
	/*enum关键字为枚举*/
	enum METHOD{GET=0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT,PATCH};
	enum CHECK_STATE{CHECK_STATE_REQUESTLINE=0,
	                 CHECK_STATE_HEADER,
		             CHECK_STATE_CONTENT};
	enum HTTP_CODE {
		NO_REQUEST,GET_REQUEST,BAD_REQUEST,
		NO_RESOURCE,FORBIDDEN_REQUEST,FILE_REQUEST,
		INTERNAL_ERROR,CLOSED_CONNECTION
	};
	enum LINE_STATUS {
		LINE_OK=0,
		LINE_BAD,
		LINE_OPEN
	};
public:
	http_conn(){}
	~http_conn(){}

public:
	/*初始化新接受的连接*/
	void init(int sockfd,const sockaddr_in& addr);
	/*关闭连接*/
	void close_conn(bool real_close=true);
	/*处理客户请求*/
	void process();
	/*非阻塞的读和写操作*/
	bool read();
	bool write();
private:
	/*初始化连接*/
	void init();
	/*解析http请求的入口函数*/
	HTTP_CODE process_read();
	/*下面几组函数被process_read所调用，各司其职，用来分析htpp请求*/
	HTTP_CODE parse_request_line(char* text);
	HTTP_CODE parse_headers(char* text);
	HTTP_CODE parse_content(char* text);
	HTTP_CODE do_request();
	char* getline() { return m_read_buf + m_start_line; }
	LINE_STATUS parse_line();
	
	/*填充http应答*/
	bool process_write(HTTP_CODE ret);
	/*下面几组函数被process_write所调用，各司其职，用来填充htpp应答*/
	void unmap();
	bool add_response(const char* format, ...);
	bool add_content(const char* content);
	bool add_status_line(int status, const char* title);
	bool add_headers(int content_length);
	bool add_content_length(int content_length);
	bool add_linger();
	bool add_blank_line();

	

public:
	static int m_epollfd;
	static int m_user_count;
private:
	/*该http连接的socket和对方的socket地址*/
	int m_sockfd;
	sockaddr_in m_address;

	/*读缓冲区*/
	char m_read_buf[READ_BUFFER_SIZE];
	/*标识读缓冲区中已经读入的客户数据的最后一个字节的下一个位置*/
	int m_read_idx;
	/*当前正在分析的字符在读缓冲区中的位置*/
	int m_checked_idx;
	/*当前正在解析的行的起始位置*/
	int m_start_line;

	/*写缓冲区*/
	char m_write_buf[WRITE_BUFFER_SIZE];
	/*写缓冲区中待发送的字节数*/
	int m_write_idx;

	/*主状态机当前所处的状态*/
	CHECK_STATE m_check_state;
	METHOD m_method;

	/*客户请求的目标文件的完整路径*/
	char m_real_file[FILENAME_LEN];
	/*客户请求目标文件的文件名*/
	char* m_url;
	/*http协议的版本号*/
	char* m_version;
	/*主机名*/
	char* m_host;
	/*http请求的消息体的长度*/
	int m_content_length;
	/*http请求是否要求保持连接*/
	bool m_linger;

	/*客户请求的模板文件被mmap到内存的起始位置*/
	char* m_file_address;
	/*目标文件的状态*/
	struct stat m_file_stat;

	/*
	我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量
	关于iovec,可参考博客：
	https://blog.csdn.net/baidu_15952103/article/details/109888362
	*/
	struct iovec m_iv[2];
	int m_iv_count;

};

#endif