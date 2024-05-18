#pragma once
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

/*�̳߳��࣬���䶨��Ϊģ������Ϊ�˴��븴�á�ģ�����T����������*/
template<class T>
class threadpool
{
public:
	/*����thread_number���̳߳����̵߳�������
	max_requests������������� ������ĵȴ��������������*/
	threadpool(int thread_number = 8, int max_requests = 10000);
	~threadpool();
	/*������������������*/
	bool append(T* request);

private:
	/*C++���̴߳������ڳ�Ա����ʱ����Ҫ���ó�Ա��������Ϊ��̬��Ա������
	��������Ҫ�����̳߳����еĶ�̬��Ա������Ϣ����m_workqueue,ʹ�þ�̬��Ա����ȴֻ�ܷ��ʾ�̬��Ա�����ͺ���
	������ǲ������·�����ʵ���ھ�̬��Ա�����е�����Ķ�̬��Ա�����ͺ���
	
	����Ķ�����Ϊ�������ݸ��þ�̬��Ա������Ȼ���ھ�̬�����������������Ȼ�����øö�����ʶ�̬��Ա�����ͺ���

	�ڱ���Ŀ�У�worker()����Ϊ��̬��������Ϊ�������ݸ�pthreadcreate()�У�
	Ȼ����worker���������Ǵ������̳߳ض�����������̳߳ض������run()������ȡ��������
    */
	static void* worker(void* arg);
	/*�����߳����еĺ����������ϴӹ���������ȡ������ִ��*/
	void run();

private:
	int m_thread_number;/*�̳߳��е��߳���*/
	int m_max_requests;/*�����������������ĵȴ��������������*/
	pthread_t* m_threads;/*�����̳߳ص����飬��СΪ m_thread_number*/
	std::list<T*> m_workqueue;/*�������*/
	locker m_queuelocker;/*����������еĻ�����*/
	sem m_queuestat;/*�Ƿ���������Ҫ����*/
	bool m_stop;/*�Ƿ�Ҫ�����߳�*/

};


/*���캯��*/
template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) :
	/*���ó�ʼ���б����ʽ�Գ�Ա�������г�ʼ�����ɲο����ͣ�
	https://blog.csdn.net/weixin_45031801/article/details/134170844
	*/
	m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL)
{
	if ((thread_number <= 0) || (max_requests <= 0))
	{
		throw std::exception();
	}

	m_threads = new pthread_t[m_thread_number];
	if (!m_threads)
	{
		throw std::exception();
	}

	
	for (int i = 0; i < thread_number; ++i)
	{
		printf("create the %dth thread\n", i);
		
		/*����thrad_number���̣߳���ʱ��worker�����ӽ�ȥ��ʼ����������*/
		//����ɹ������̣߳�pthread_create() ������������ 0����֮���ط���ֵ��
		if (pthread_create(m_threads + i, NULL, worker, this) != 0)
		/*��һ������Ϊ������̵߳�id�ĵ�ַ���ڶ�������Ϊ�������̵߳�����(һ����ΪNULL����)
		����������ΪҪ���߳�ִ�еĹ������������ĸ�����Ϊ�������������Ĳ���

		pthread_create�÷��ɲο���
		https://c.biancheng.net/view/8607.html
		*/
		{
			delete[] m_threads;
			printf(" crate ");
			throw std::exception();
		}
		/*����Щ�߳�����Ϊ�����߳�(detach thread)*/
		printf("detached the %dth thread\n", i);
		if (pthread_detach(m_threads[i]))
		{
			delete[] m_threads;
			printf(" detach ");
			throw std::exception();
		}
		
	}
	


}

/*��������*/
template< typename T >
threadpool< T >::~threadpool()
{
	delete[] m_threads;
	m_stop =true;
}
/*append����*/
template< typename T >
bool threadpool< T >::append(T* request)
{
	/*������������һ��Ҫ��������Ϊ�ö���Ϊ������Դ*/
	m_queuelocker.lock();
	/*����ǰ�����������������������������ִ�С�*/
	if (m_workqueue.size() > m_max_requests)
	{
		m_queuelocker.unlock();
		return false;
	}
	m_workqueue.push_back(request);
	m_queuelocker.unlock();
	m_queuestat.post();/*�ź�����1*/
	printf("append");
	return true;
}

template< typename T >
void* threadpool< T >::worker(void* arg)
{
	threadpool* pool = (threadpool*)arg;
	pool->run();
	return pool;
}

template< typename T >
void threadpool< T >::run()
{
	while (!m_stop)
	{
		m_queuestat.wait();
		m_queuelocker.lock();
		if (m_workqueue.empty())
		{
			m_queuelocker.unlock();
			continue;
		}
		T* request = m_workqueue.front();
		m_workqueue.pop_front();
		m_queuelocker.unlock();
		if (!request)
		{
			continue;
		}
		request->process();
	}
}

#endif