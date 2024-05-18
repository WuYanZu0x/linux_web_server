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
	/*���������Ĵ�С*/
	static const int READ_BUFFER_SIZE = 2048;
	/*д�������Ĵ�С*/
    static const int WRITE_BUFFER_SIZE = 1024;
	/*enum�ؼ���Ϊö��*/
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
	/*��ʼ���½��ܵ�����*/
	void init(int sockfd,const sockaddr_in& addr);
	/*�ر�����*/
	void close_conn(bool real_close=true);
	/*����ͻ�����*/
	void process();
	/*�������Ķ���д����*/
	bool read();
	bool write();
private:
	/*��ʼ������*/
	void init();
	/*����http�������ں���*/
	HTTP_CODE process_read();
	/*���漸�麯����process_read�����ã���˾��ְ����������htpp����*/
	HTTP_CODE parse_request_line(char* text);
	HTTP_CODE parse_headers(char* text);
	HTTP_CODE parse_content(char* text);
	HTTP_CODE do_request();
	char* getline() { return m_read_buf + m_start_line; }
	LINE_STATUS parse_line();
	
	/*���httpӦ��*/
	bool process_write(HTTP_CODE ret);
	/*���漸�麯����process_write�����ã���˾��ְ���������htppӦ��*/
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
	/*��http���ӵ�socket�ͶԷ���socket��ַ*/
	int m_sockfd;
	sockaddr_in m_address;

	/*��������*/
	char m_read_buf[READ_BUFFER_SIZE];
	/*��ʶ�����������Ѿ�����Ŀͻ����ݵ����һ���ֽڵ���һ��λ��*/
	int m_read_idx;
	/*��ǰ���ڷ������ַ��ڶ��������е�λ��*/
	int m_checked_idx;
	/*��ǰ���ڽ������е���ʼλ��*/
	int m_start_line;

	/*д������*/
	char m_write_buf[WRITE_BUFFER_SIZE];
	/*д�������д����͵��ֽ���*/
	int m_write_idx;

	/*��״̬����ǰ������״̬*/
	CHECK_STATE m_check_state;
	METHOD m_method;

	/*�ͻ������Ŀ���ļ�������·��*/
	char m_real_file[FILENAME_LEN];
	/*�ͻ�����Ŀ���ļ����ļ���*/
	char* m_url;
	/*httpЭ��İ汾��*/
	char* m_version;
	/*������*/
	char* m_host;
	/*http�������Ϣ��ĳ���*/
	int m_content_length;
	/*http�����Ƿ�Ҫ�󱣳�����*/
	bool m_linger;

	/*�ͻ������ģ���ļ���mmap���ڴ����ʼλ��*/
	char* m_file_address;
	/*Ŀ���ļ���״̬*/
	struct stat m_file_stat;

	/*
	���ǽ�����writev��ִ��д���������Զ�������������Ա������m_iv_count��ʾ��д�ڴ�������
	����iovec,�ɲο����ͣ�
	https://blog.csdn.net/baidu_15952103/article/details/109888362
	*/
	struct iovec m_iv[2];
	int m_iv_count;

};

#endif