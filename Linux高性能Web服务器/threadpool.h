#pragma once
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

/*线程池类，将其定义为模板类是为了代码复用。模板参数T代表任务类*/
template<class T>
class threadpool
{
public:
	/*参数thread_number是线程池中线程的数量，
	max_requests是请求队列中允 许的最大的等待处理的请求数量*/
	threadpool(int thread_number = 8, int max_requests = 10000);
	~threadpool();
	/*往请求队列中添加任务*/
	bool append(T* request);

private:
	/*C++中线程传入类内成员函数时，需要将该成员函数定义为静态成员函数，
	但我们需要访问线程池类中的动态成员变量消息队列m_workqueue,使用静态成员函数却只能访问静态成员变量和函数
	因此我们采用以下方法来实现在静态成员函数中调用类的动态成员变量和函数
	
	将类的对象作为参数传递给该静态成员函数，然后在静态函数中引用这个对象，然后利用该对象访问动态成员变量和函数

	在本项目中，worker()函数为静态函数，作为参数传递给pthreadcreate()中，
	然后在worker中引用我们创建的线程池对象，最后利用线程池对象调用run()函数读取工作队列
    */
	static void* worker(void* arg);
	/*工作线程运行的函数，它不断从工作队列中取出任务并执行*/
	void run();

private:
	int m_thread_number;/*线程池中的线程数*/
	int m_max_requests;/*请求队列中允许的最大的等待处理的请求数量*/
	pthread_t* m_threads;/*描述线程池的数组，大小为 m_thread_number*/
	std::list<T*> m_workqueue;/*请求队列*/
	locker m_queuelocker;/*保护请求队列的互斥锁*/
	sem m_queuestat;/*是否有任务需要处理*/
	bool m_stop;/*是否要结束线程*/

};


/*构造函数*/
template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) :
	/*采用初始化列表的形式对成员变量进行初始化，可参考博客：
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
		
		/*创建thrad_number个线程，此时把worker函数扔进去开始读工作队列*/
		//如果成功创建线程，pthread_create() 函数返回数字 0，反之返回非零值。
		if (pthread_create(m_threads + i, NULL, worker, this) != 0)
		/*第一个参数为存放新线程的id的地址，第二个参数为设置新线程的属性(一般设为NULL即可)
		第三个参数为要求线程执行的工作函数，第四个参数为传给工作函数的参数

		pthread_create用法可参考：
		https://c.biancheng.net/view/8607.html
		*/
		{
			delete[] m_threads;
			printf(" crate ");
			throw std::exception();
		}
		/*将这些线程设置为脱离线程(detach thread)*/
		printf("detached the %dth thread\n", i);
		if (pthread_detach(m_threads[i]))
		{
			delete[] m_threads;
			printf(" detach ");
			throw std::exception();
		}
		
	}
	


}

/*析构函数*/
template< typename T >
threadpool< T >::~threadpool()
{
	delete[] m_threads;
	m_stop =true;
}
/*append函数*/
template< typename T >
bool threadpool< T >::append(T* request)
{
	/*操作工作队列一定要加锁，因为该队列为共享资源*/
	m_queuelocker.lock();
	/*若当前请求数量大于最大请求数量，则不予执行。*/
	if (m_workqueue.size() > m_max_requests)
	{
		m_queuelocker.unlock();
		return false;
	}
	m_workqueue.push_back(request);
	m_queuelocker.unlock();
	m_queuestat.post();/*信号量加1*/
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