#include"http_conn.h"

/*����http��Ӧ��һЩ״̬��Ϣ*/
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

/*��վ�ĸ�Ŀ¼*/
const char* doc_root = "/home";


/*��̬��Ա���������ʼ��*/
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

int setnonblocking(int fd)
{
	/*��fd����Ϊ������ģʽ�ľ������*/
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
	setnonblocking(fd);//����setnonblocking��������fd����Ϊ������ģʽ
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

void http_conn::close_conn(bool real_close)//real_close����ʱĬ��Ϊtrue
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
	/*�������н����ڵ���*/
	/*int reuse = 1;
	setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));*/

	addfd(m_epollfd, sockfd, true);
	m_user_count++;
	init();//���ó�ʼ������
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

/*��ʼ������main������102�п�ʼ����read��������read�����������ݣ���process�������뵽�̳߳��д���
��process�����У��ȴ�����¼���
1,������״̬��process_read() 
2,��process_read()��ѭ������Ƿ����һ����������( ���ô�״̬��parse_line() )���Լ����е�����(��������or����ͷ��or��������),Ȼ����������Ե��ö�Ӧ�Ĵ�����
3,Ȼ����process_read()�е��ö�Ӧ�Ĵ�����������ret�����䷵�ص�HTTP_CODE(�ظ���ʱ����Ҫ���ݲ�ͬ��code����ͬ�Ļ�Ӧ)
��ret���յ�����get_request,�����do_request()����( �ú���������Ҫ�����ļ�ӳ�䵽�ڴ棬������ַ����m_file_adddress )
����bad �� no_request��internal_error ��������״̬����

�����������������������������״̬�����ص�process�У����Ŵ�����¼�:
4,����process_write()
5,����ret��ֵ���з������(�����͵�������ӵ����ͻ�������)
�������Ƿֱ��װ�������Ӧ�к����������Ӧͷ����, ���ǻὫ��Ӧ�к���Ӧͷ����ṹ������iovec �� index 0 ��λ��
������Ҫ�����ļ����ļ������Ѿ���ӳ�䵽�ڴ棬λ����m_file_adddress ���棬����ֻ�轫m_file_address����index 1 ��λ�ü���
����process�Ĺ����Ѿ�����
6,�������ص�main����121�У�����write����������writev���м���д�Ĳ���(���ýṹ������iovec����������������ݼ���д��)����������

*/

/*process���������̳߳��еĹ����̵߳��ã����Ǵ���http�������ں���*/
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


/*��״̬��*/
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
	/*m_checked_idxָ��readbuffer�е�ǰ���ڷ������ֽڣ�
	m_read_idxָ��readbuffer�пͻ�����β������һ�ֽ�*/
	for (;m_checked_idx<m_read_idx;++m_checked_idx)
	{
		/*��ȡ��ǰҪ�������ֽ�*/
		temp = m_read_buf[m_checked_idx];
		/*
		�����ǰ�ֽ���"\r",���س�������˵���п��ܶ���һ����������
		��Ϊһ�н����Իس��ӻ��з���������"\r\n"
		�������ǻ���Ҫ����������
		1����\r�������һ���ַ���������һ���ַ��Ƿ�Ϊ\n��
		���ǣ���˵������һ���������У������ǣ���˵����http�������﷨����
		2����\r�����һ���ַ�����ô���ǲ���֪��������ֽ���ʲô���򷵻�line_open������״̬������������
		*/
		if (temp == '\r')
		{
			if ((m_checked_idx + 1 == m_read_idx))
			{
				//\r�����һ���ַ�
				return LINE_OPEN;
			}
			else if (m_read_buf[m_checked_idx + 1] == '\n')
			{
				//\r����һ���ַ�Ϊ\n
				m_read_buf[m_checked_idx++] = '\0';//��\r�ֽ�����Ϊ\0
				m_read_buf[m_checked_idx++] = '\0';//��\n�ֽ�����Ϊ\0
				return LINE_OK;
			}
			// \r�������һ���ַ�,��\r��һ���ֽ�Ҳ����\n,˵���﷨������
			return LINE_BAD;
		}
		else if (temp == '\n')//�����������һ���ж���������Ϊ\n�п��ܳ����ڵ�һ���ֽڵ�λ��
		{
			if ((m_checked_idx>1)&&m_read_buf[m_checked_idx-1]=='\r')
			{
				m_read_buf[m_checked_idx++] = '\0';//��\r�ֽ�����Ϊ\0
				m_read_buf[m_checked_idx++] = '\0';//��\n�ֽ�����Ϊ\0
				return LINE_OK;
			}
			else
			{
				return LINE_BAD;
			}
		}

	}
	/*���m_read_buf���������ݷ�����϶�û����\r����\n���򷵻�LINE_OPEN*/
	return LINE_OPEN;
}

/*����http�����У���ȡmethod��URL���Լ�http�汾��*/
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
	////�����ַ��� str1 ���� str2 ��ָ�����κ��ַ�ƥ��ĵ�һ���ַ����ⲻ������ֹ null �ַ��������ǲ��� �ո��\t
	m_url = strpbrk(text," /t");//����ʵ��ûŪ��Ϊʲô�ǲ�\t������\r\n
	if (!m_url)
	{
		//�����п϶��пո����\t,����˵�������⡣
		return BAD_REQUEST;
	}
	/*���ｫ�ո���滻��0������m_url++��ָ����һ��λ��*/
	*m_url++ = '\0';
	char* method = text;//ע��ǰ�潫�ո���滻Ϊ��\0����ֵ����������\0�����������Դ�ʱtext��������Ҫ�ҵķ���
	if (strcasecmp(method, "GET") == 0)
	//strcasecmp(s1, s2)�ȶ�s1��s2�ַ�������ͬ����0,s1����s2���ش���0��ֵ��s1С��s2����С��0��ֵ��
	{
		m_method = GET;
	}
	else
	{
		//Ŀǰ��֧��GET����
		return BAD_REQUEST;
	}
	//�����൱�ڷ�ֹ�м��ж��\t��ֱ�Ӱ�pointָ����һ�����ǿո��\t�ĵط�
	//point += strspn(point, "\t");
	m_url+= strspn(m_url, " \t");

	//������URL�ķ�������ȡ�汾��,��Ϊ����汾�Ų��ԾͿ���ֱ��returnbad�ˣ��Ͳ����ڷ���url

    //�����൱�ڷ�ֹ�м��ж��\t��ֱ�Ӱ�versionָ����һ�����ǿո��\t�ĵط�
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

	//��URL�ķ���
	  //���URL�Ƿ�Ϸ�,url�ǲ�����http://��ͷ
	if (strncasecmp(m_url, "http://", 7) == 0) {
/*
int strncasecmp(const char *s1, const char *s2, size_t n);
strncasecmp()�����Ƚϲ���s1 ��s2 �ַ���ǰn���ַ����Ƚ�ʱ���Զ����Դ�Сд�Ĳ��졣
������s1 ��s2 �ַ�����ͬ�򷵻�0��s1 ������s2 �򷵻ش���0 ��ֵ��s1 ��С��s2 �򷵻�С��0 ��ֵ��
*/
		//������ͷhttp://
		m_url += 7;
		//char *strchr(char *s1,char c1):���ַ���s1��Ѱ���ַ�c1��һ�γ��ֵ�λ�á��ҵ�����ָ�룬û�ҵ�����NULL
		m_url = strchr(m_url, '/');
	}
	if (!m_url || m_url[0] != '/') {
		return BAD_REQUEST;
	}
	printf("The request URL id: %s\n", m_url);
	//HTTP�����д�����ϣ�״̬ת�Ƶ�ͷ���ֶεķ�����
	m_check_state = CHECK_STATE_HEADER;
	//��Ϊֻ�����������У�����Ҫ����ͷ��
	return NO_REQUEST;

}

http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
	/*���� /0 ����ʾͷ���ֶν������*/
	if (text[0] == '\0')
	{
		/*
		���http��������Ϣ�壬����Ҫ������Ϣ�壬���Է���CHECK_STATE_CONTENT
		*/
		if (m_content_length != 0)
		{
			m_check_state = CHECK_STATE_CONTENT;
			return NO_REQUEST;
		}
		else
		{
			/*��û�У�˵�������Ѿ��õ�һ��������http������*/
			return GET_REQUEST;
		}
	}
	/*����connection�ֶ�*/
      else if (strncasecmp(text, "Connection:", 11) == 0)
    {
      text += 11;
      text += strspn(text, " \t");
      if (strcasecmp(text, "keep-alive") == 0)
		{
			m_linger = true;
	    }
	  
    }
	/*����Content-Length�ֶ�*/
	  else if (strncasecmp(text, "Content-Length:", 15) == 0)
	{
		text += 15;
		text += strspn(text, " \t");
		m_content_length = atol(text);
		//C �⺯�� long int atol(const char *str) �Ѳ��� str ��ָ����ַ���ת��Ϊһ��������������Ϊ long int �ͣ���
	}
	/*����Host�ֶ�*/
	  else if (strncasecmp(text, "Host:", 5) == 0)
	{
		text += 5;
		text += strspn(text, " \t");
		m_host = text;
		//C �⺯�� long int atol(const char *str) �Ѳ��� str ��ָ����ַ���ת��Ϊһ��������������Ϊ long int �ͣ���
	}
	  else
	{
		printf("oop! unknow head%s\n", text);
	}
	return NO_REQUEST;
}

/*����û�������Ľ�����Ϣ�壬ֻ���ж����Ƿ�����������*/

http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
	if (m_read_idx >= (m_content_length + m_checked_idx))//��ûŪ������ж�6
	{
		text[m_content_length] = '\0';
		return GET_REQUEST;
	}
	else
	{
		return NO_REQUEST;
	}
}

/*���õ�һ����������ȷ��http��������ǾͿ�ʼ����Ŀ���ļ������ԡ����Ŀ���ļ����ڣ�����others�ɶ�Ȩ�ޣ��Ҳ���Ŀ¼��
��ʹ��mmap����ӳ�䵽�ڴ��ַm_file_address�������ߵ������ļ���ȡ�ɹ�*/
http_conn::HTTP_CODE http_conn::do_request()
{
	strcpy(m_real_file, doc_root);
	int len = strlen(doc_root);
	/* C �⺯�� char *strncpy(char *dest, const char *src, size_t n) �� src ��ָ����ַ������Ƶ� dest��
	��ิ�� n ���ַ����� src �ĳ���С�� n ʱ��dest ��ʣ�ಿ�ֽ��ÿ��ֽ���䡣*/
	strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
	if (stat(m_real_file, &m_file_stat) < 0)
	{
		/*���Ŀ���ļ�������*/
		return NO_RESOURCE;
	}

	if (!(m_file_stat.st_mode & S_IROTH))
	{
		/*���Ŀ���ļ�������others�ɶ������ʣ��򷵻�forbidden*/
		return FORBIDDEN_REQUEST;
	}

	if (S_ISDIR(m_file_stat.st_mode))
	{
		/*���Ŀ���ļ���һ��Ŀ¼���򷵻�bad*/
		return BAD_REQUEST;
	}
	int fd = open(m_real_file, O_RDONLY);
	/*
	����mmap
	����mmap���ɲο����ͣ�
    https://zhuanlan.zhihu.com/p/477641987
	*/
	m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	return FILE_REQUEST;
}
/*
���ڴ�ӳ����ִ��mumap����
����mmap���ɲο����ͣ�
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


bool http_conn::add_response(const char* format, ...)//���������ɱ����
{
	if (m_write_idx >= WRITE_BUFFER_SIZE)
	{
		return false;
	}
	/*�����va_list�ǿɱ�����б����˼����Ϊ��add_reponse�Ĳ����б��У�
	���ǲ����˿ɱ��������ʽ��������Ҫ����va_list����ȡ�ող���
	����ʹ�ÿ��Բ鿴���ͣ�
	https://blog.csdn.net/tangg555/article/details/62038404
	*/
	va_list(arg_list);
	va_start(arg_list, format);
	/*
	vsnprintf() ��������va_list����������ӡ����������
	����ʹ�ÿɲο�:
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
