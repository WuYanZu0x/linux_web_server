#include"http_conn.h"

/*定义http响应的一些状态信息*/
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

/*网站的根目录*/
const char* doc_root = "/home";


/*静态成员变量类外初始化*/
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

int setnonblocking(int fd)
{
	/*将fd设置为非阻塞模式的具体操作*/
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void addfd(int epollfd, int fd, bool oneshot)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
	if (oneshot)
	{
		event.events |= EPOLLONESHOT;
	}
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblocking(fd);//调用setnonblocking函数，将fd设置为非阻塞模式
}


void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
	epoll_ctl(epollfd,EPOLL_CTL_MOD, fd, &event);
}

void http_conn::close_conn(bool real_close)//real_close声明时默认为true
{
	if (real_close && (m_sockfd != -1))
	{
		//modfd( m_epollfd, m_sockfd, EPOLLIN );
		removefd(m_epollfd, m_sockfd);
		m_sockfd = -1;
		m_user_count--;
	}
}


void http_conn::init(int sockfd,const sockaddr_in & addr)
{
	m_sockfd = sockfd;
	m_address = addr;
	/*下面两行仅用于调试*/
	/*int reuse = 1;
	setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));*/

	addfd(m_epollfd, sockfd, true);
	m_user_count++;
	init();//调用初始化函数
}

void http_conn::init()
{
	m_check_state = CHECK_STATE_REQUESTLINE;
	m_linger = false;

	m_method = GET;
	m_url = 0;
	m_version = 0;
	m_content_length = 0;
	m_host = 0;
	m_start_line = 0;
	m_checked_idx = 0;
	m_read_idx = 0;
	m_write_idx = 0;
	memset(m_write_buf,'\0', WRITE_BUFFER_SIZE);
	memset(m_read_buf,'\0', READ_BUFFER_SIZE);
	memset(m_real_file,'\0',FILENAME_LEN);
}

bool http_conn::read()
{
	if (m_read_idx >= READ_BUFFER_SIZE)
	{
		return false;
	}

	int bytes_read = 0;
	while (true)
	{
		bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
		if (bytes_read == -1)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				break;
			}
			return false;
		}
		else if (bytes_read == 0)
		{
			return false;
		}

		m_read_idx += bytes_read;
	}
	
	return true;
}

/*初始化后，在main函数第102行开始调用read函数，若read函数中有内容，则将process函数加入到线程池中处理
在process函数中，先处理读事件：
1,调用主状态机process_read() 
2,在process_read()中循环检查是否读到一个完整的行( 利用从状态机parse_line() )，以及该行的属性(是请求行or请求头部or请求内容),然后根据其属性调用对应的处理函数
3,然后在process_read()中调用对应的处理函数，并用ret接受其返回的HTTP_CODE(回复的时候需要根据不同的code做不同的回应)
若ret接收到的是get_request,则调用do_request()函数( 该函数用来将要读的文件映射到内存，并将地址赋给m_file_adddress )
若是bad ， no_request，internal_error 则跳出主状态机，

以上这两种情况接下来都会跳出主状态机，回到process中，接着处理读事件:
4,调用process_write()
5,根据ret的值进行分类操作(将发送的内容添加到发送缓冲区中)
这里我们分别封装了添加响应行函数，添加响应头函数, 它们会将响应行和响应头放入结构体数组iovec 的 index 0 的位置
而若需要发送文件，文件内容已经被映射到内存，位置由m_file_adddress 保存，我们只需将m_file_address放入index 1 的位置即可
到此process的工作已经结束
6,接下来回到main函数121行，调用write函数，利用writev进行集中写的操作(利用结构体数组iovec，将数组的所有内容集中写出)，发送数据

*/

/*process函数，由线程池中的工作线程调用，这是处理http请求的入口函数*/
void http_conn::process()
{
	HTTP_CODE read_ret = process_read();
	
	if (read_ret == NO_REQUEST)
	{
		printf("norequest");
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		return;
	}

	bool write_ret = process_write(read_ret);
	if (!write_ret)
	{
		close_conn();
	}

	modfd(m_epollfd, m_sockfd, EPOLLOUT);
}


/*主状态机*/
http_conn::HTTP_CODE http_conn::process_read()
{
	LINE_STATUS line_status = LINE_OK;
	HTTP_CODE ret = NO_REQUEST;
	char* text = 0;
	while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
		|| ((line_status = parse_line()) == LINE_OK))
	{
		text = getline();
		m_start_line = m_checked_idx;
		printf("get one http line:%s\n", text);

		switch (m_check_state)
		{
		case CHECK_STATE_REQUESTLINE:
		{
			ret = parse_request_line(text);
			if (ret == BAD_REQUEST)
			{
				return BAD_REQUEST;
			}
			break;
		}
		case CHECK_STATE_HEADER:
		{
			ret = parse_headers(text);
			if (ret == BAD_REQUEST)
			{
				return BAD_REQUEST;
			}
			else if (ret == GET_REQUEST)
			{
				return do_request();
			}
			break;
			
		}
		case CHECK_STATE_CONTENT:
		{
			ret = parse_content(text);
			if (ret == GET_REQUEST)
			{
				return do_request();
			}
			line_status = LINE_OPEN;
			break;
		}
		default:
			return INTERNAL_ERROR;
		}
	}
}

http_conn::LINE_STATUS http_conn::parse_line()
{
	char temp;
	/*m_checked_idx指向readbuffer中当前正在分析的字节，
	m_read_idx指向readbuffer中客户数据尾部的下一字节*/
	for (;m_checked_idx<m_read_idx;++m_checked_idx)
	{
		/*获取当前要分析的字节*/
		temp = m_read_buf[m_checked_idx];
		/*
		如果当前字节是"\r",即回车符，则说明有可能读到一个完整的行
		因为一行结束以回车加换行符结束，即"\r\n"
		所以我们还需要检查两种情况
		1，若\r不是最后一个字符，则检查下一个字符是否为\n，
		若是，则说明读到一个完整的行，若不是，则说明该http请求有语法错误
		2，若\r是最后一个字符，那么我们并不知道后面的字节是什么，则返回line_open，让主状态机继续读数据
		*/
		if (temp == '\r')
		{
			if ((m_checked_idx + 1 == m_read_idx))
			{
				//\r是最后一个字符
				return LINE_OPEN;
			}
			else if (m_read_buf[m_checked_idx + 1] == '\n')
			{
				//\r的下一个字符为\n
				m_read_buf[m_checked_idx++] = '\0';//将\r字节设置为\0
				m_read_buf[m_checked_idx++] = '\0';//将\n字节设置为\0
				return LINE_OK;
			}
			// \r不是最后一个字符,且\r下一个字节也不是\n,说明语法有问题
			return LINE_BAD;
		}
		else if (temp == '\n')//这里必须设置一个判断条件，因为\n有可能出现在第一个字节的位置
		{
			if ((m_checked_idx>1)&&m_read_buf[m_checked_idx-1]=='\r')
			{
				m_read_buf[m_checked_idx++] = '\0';//将\r字节设置为\0
				m_read_buf[m_checked_idx++] = '\0';//将\n字节设置为\0
				return LINE_OK;
			}
			else
			{
				return LINE_BAD;
			}
		}

	}
	/*如果m_read_buf中所有内容分析完毕都没遇到\r或者\n，则返回LINE_OPEN*/
	return LINE_OPEN;
}

/*解析http请求行，获取method，URL，以及http版本号*/
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
	////查找字符串 str1 中与 str2 中指定的任何字符匹配的第一个字符。这不包括终止 null 字符。这里是查找 空格和\t
	m_url = strpbrk(text," /t");//这里实在没弄懂为什么是查\t而不是\r\n
	if (!m_url)
	{
		//请求行肯定有空格或者\t,否则说明有问题。
		return BAD_REQUEST;
	}
	/*这里将空格符替换成0，再让m_url++，指向下一个位置*/
	*m_url++ = '\0';
	char* method = text;//注意前面将空格符替换为了\0，赋值过程中遇到\0便会结束，所以此时text就是我们要找的方法
	if (strcasecmp(method, "GET") == 0)
	//strcasecmp(s1, s2)比对s1和s2字符串，相同返回0,s1大于s2返回大于0的值，s1小于s2返回小于0的值。
	{
		m_method = GET;
	}
	else
	{
		//目前仅支持GET方法
		return BAD_REQUEST;
	}
	//这里相当于防止中间有多个\t，直接把point指到第一个不是空格或\t的地方
	//point += strspn(point, "\t");
	m_url+= strspn(m_url, " \t");

	//先跳过URL的分析，获取版本号,因为如果版本号不对就可以直接returnbad了，就不用在分析url

    //这里相当于防止中间有多个\t，直接把version指到第一个不是空格或\t的地方
	/*version += strspn(point, "\t");*/
	m_version = strpbrk(m_url, " \t");
	if(! m_version)
	{
		return BAD_REQUEST;
	}
	*m_version++ = '\0';
	if (strcasecmp(m_version, "HTTP/1.1") != 0)
	{
		return BAD_REQUEST;
	}

	//对URL的分析
	  //检查URL是否合法,url是不是以http://起头
	if (strncasecmp(m_url, "http://", 7) == 0) {
/*
int strncasecmp(const char *s1, const char *s2, size_t n);
strncasecmp()用来比较参数s1 和s2 字符串前n个字符，比较时会自动忽略大小写的差异。
若参数s1 和s2 字符串相同则返回0。s1 若大于s2 则返回大于0 的值，s1 若小于s2 则返回小于0 的值。
*/
		//跳过开头http://
		m_url += 7;
		//char *strchr(char *s1,char c1):从字符串s1中寻找字符c1第一次出现的位置。找到返回指针，没找到返回NULL
		m_url = strchr(m_url, '/');
	}
	if (!m_url || m_url[0] != '/') {
		return BAD_REQUEST;
	}
	printf("The request URL id: %s\n", m_url);
	//HTTP请求行处理完毕，状态转移到头部字段的分析。
	m_check_state = CHECK_STATE_HEADER;
	//因为只分析了请求行，还需要分析头部
	return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
	/*遇到 /0 ，表示头部字段解析完毕*/
	if (text[0] == '\0')
	{
		/*
		如果http请求有消息体，则还需要解析消息体，所以返回CHECK_STATE_CONTENT
		*/
		if (m_content_length != 0)
		{
			m_check_state = CHECK_STATE_CONTENT;
			return NO_REQUEST;
		}
		else
		{
			/*若没有，说明我们已经得到一个完整的http请求了*/
			return GET_REQUEST;
		}
	}
	/*处理connection字段*/
      else if (strncasecmp(text, "Connection:", 11) == 0)
    {
      text += 11;
      text += strspn(text, " \t");
      if (strcasecmp(text, "keep-alive") == 0)
		{
			m_linger = true;
	    }
	  
    }
	/*处理Content-Length字段*/
	  else if (strncasecmp(text, "Content-Length:", 15) == 0)
	{
		text += 15;
		text += strspn(text, " \t");
		m_content_length = atol(text);
		//C 库函数 long int atol(const char *str) 把参数 str 所指向的字符串转换为一个长整数（类型为 long int 型）。
	}
	/*处理Host字段*/
	  else if (strncasecmp(text, "Host:", 5) == 0)
	{
		text += 5;
		text += strspn(text, " \t");
		m_host = text;
		//C 库函数 long int atol(const char *str) 把参数 str 所指向的字符串转换为一个长整数（类型为 long int 型）。
	}
	  else
	{
		printf("oop! unknow head%s\n", text);
	}
	return NO_REQUEST;
}

/*我们没有真正的解析消息体，只是判断它是否被完整读入了*/

http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
	if (m_read_idx >= (m_content_length + m_checked_idx))//还没弄懂这个判断6
	{
		text[m_content_length] = '\0';
		return GET_REQUEST;
	}
	else
	{
		return NO_REQUEST;
	}
}

/*当得到一个完整，正确的http请求后，我们就开始分析目标文件的属性。如果目标文件存在，具有others可读权限，且不是目录，
则使用mmap将其映射到内存地址m_file_address处，告诉调用者文件获取成功*/
http_conn::HTTP_CODE http_conn::do_request()
{
	strcpy(m_real_file, doc_root);
	int len = strlen(doc_root);
	/* C 库函数 char *strncpy(char *dest, const char *src, size_t n) 把 src 所指向的字符串复制到 dest，
	最多复制 n 个字符。当 src 的长度小于 n 时，dest 的剩余部分将用空字节填充。*/
	strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
	if (stat(m_real_file, &m_file_stat) < 0)
	{
		/*如果目标文件不存在*/
		return NO_RESOURCE;
	}

	if (!(m_file_stat.st_mode & S_IROTH))
	{
		/*如果目标文件不具有others可读的性质，则返回forbidden*/
		return FORBIDDEN_REQUEST;
	}

	if (S_ISDIR(m_file_stat.st_mode))
	{
		/*如果目标文件是一个目录，则返回bad*/
		return BAD_REQUEST;
	}
	int fd = open(m_real_file, O_RDONLY);
	/*
	进行mmap
	关于mmap，可参考博客：
    https://zhuanlan.zhihu.com/p/477641987
	*/
	m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	return FILE_REQUEST;
}
/*
对内存映射区执行mumap操作
关于mmap，可参考博客：
https://zhuanlan.zhihu.com/p/477641987
*/
void http_conn::unmap()
{
	if (m_file_address)
	{
		munmap(m_file_address, m_file_stat.st_size);
		m_file_address = 0;
	}
}

bool http_conn::write()
{
	int temp = 0;
	int bytes_have_send = 0;
	int bytes_to_send = m_write_idx;
	if (bytes_to_send == 0)
	{
		modfd(m_epollfd, m_sockfd, EPOLLIN);
		init();
		return true;
	}

	while (1)
	{
		temp = writev(m_sockfd, m_iv, m_iv_count);
		if (temp <= -1)
		{
			if (errno == EAGAIN)
			{
				modfd(m_epollfd, m_sockfd, EPOLLOUT);
				return true;
			}
			unmap();
			return false;
		}

		bytes_to_send -= temp;
		bytes_have_send += temp;
		if (bytes_to_send <= bytes_have_send)
		{
			unmap();
			if (m_linger)
			{
				init();
				modfd(m_epollfd, m_sockfd, EPOLLIN);
				return true;
			}
			else
			{
				modfd(m_epollfd, m_sockfd, EPOLLIN);
				return false;
			}
		}
	}
}


bool http_conn::add_response(const char* format, ...)//三个点代表可变参数
{
	if (m_write_idx >= WRITE_BUFFER_SIZE)
	{
		return false;
	}
	/*这里的va_list是可变参数列表的意思，因为在add_reponse的参数列表中，
	我们采用了可变参数的形式，于是需要利用va_list来获取刚刚参数
	具体使用可以查看博客：
	https://blog.csdn.net/tangg555/article/details/62038404
	*/
	va_list(arg_list);
	va_start(arg_list, format);
	/*
	vsnprintf() 可以利用va_list，将参数打印到缓存区中
	具体使用可参考:
	https://blog.csdn.net/luliplus/article/details/124123219
	*/
	int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
	if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
	{
		return false;
	}
	m_write_idx += len;
	va_end(arg_list);
	return true;
}

bool http_conn::add_status_line(int status, const char* title)
{
	return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
	add_content_length(content_len);
	add_linger();
	add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
	return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
	return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
	return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content)
{
	return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
	printf("write");
	switch (ret)
	{
	case INTERNAL_ERROR:
	{
		add_status_line(500, error_500_title);
		add_headers(strlen(error_500_form));
		
		if (!add_content(error_500_form))
		{
			return false;
		}
		break;
	}
	case BAD_REQUEST:
	{
		add_status_line(400, error_400_title);
		add_headers(strlen(error_400_form));
		if (!add_content(error_400_form))
		{
			return false;
		}
		break;
	}
	case NO_RESOURCE:
	{
		add_status_line(404, error_404_title);
		add_headers(strlen(error_404_form));
		if (!add_content(error_404_form))
		{
			return false;
		}
		break;
	}
	case FORBIDDEN_REQUEST:
	{
		add_status_line(403, error_403_title);
		add_headers(strlen(error_403_form));
		printf("ffffffffff");
		if (!add_content(error_403_form))
		{
			return false;
		}
		break;
	}
	case FILE_REQUEST:
	{
		add_status_line(200, ok_200_title);
		printf("filreply");
		if (m_file_stat.st_size != 0)
		{
			add_headers(m_file_stat.st_size);
			m_iv[0].iov_base = m_write_buf;
			m_iv[0].iov_len = m_write_idx;
			m_iv[1].iov_base = m_file_address;
			m_iv[1].iov_len = m_file_stat.st_size;
			m_iv_count = 2;
			return true;
		}
		else
		{
			const char* ok_string = "<html><body></body></html>";
			add_headers(strlen(ok_string));
			if (!add_content(ok_string))
			{
				return false;
			}
		}
	}
	default:
	{
		return false;
	}
	}

	m_iv[0].iov_base = m_write_buf;
	m_iv[0].iov_len = m_write_idx;
	m_iv_count = 1;
	return true;
}
