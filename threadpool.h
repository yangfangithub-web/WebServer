#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <execinfo.h>
#include <list>
#include <pthread.h>
#include <semaphore.h>
#include <cstdio>
#include "locker.h"

//线程池类，将其定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    //线程池中线程数量
    //请求队列中最多允许的、等待处理的请求的数量
    threadpool(int m_thread_number=8,int m_max_requests = 1000);
    ~threadpool();
    //将请求加入到队列中
    bool append(T* request);

private:
    //工作线程运行的函数，不断从工作队列中取出任务并执行
    static void* worker(void *arg);
    void run();

private:
    //线程数量
    int m_thread_number;

    //描述线程池的数组，大小为m_thread_number
    pthread_t *m_threads;

    //请求队列中最多允许的，等待处理的请求的数量
    int m_max_requests;

    //请求队列
    std::list<T*> m_workqueue;

    //保护请求队列的互斥锁
    locker m_queuelocker;

    //是否由任务需要处理
    sem m_queuestat; 

    //是否结束线程
    bool m_stop;

};



//类成员函数的实现
//构造函数:创建线程池（数组），并设置成脱离线程
template<typename T>
threadpool<T> :: threadpool(int thread_number,int max_requests) :
    m_thread_number(thread_number),m_max_requests(max_requests),
    m_stop(false),m_threads(NULL) {

        if ((thread_number <= 0)||(max_requests<=0) ) {
            throw std:: exception();
        }

        m_threads = new pthread_t[m_thread_number];
        if (!m_threads) {
            throw std:: exception();
        }

        //创建 thread_number个线程，并将它们设置成脱离线程
        for (int i=0;i<thread_number; ++i) {
            printf("create the %dth thread\n",i);
            if (pthread_create(m_threads+i,NULL,worker,this)!=0) {
                delete[] m_threads;
                throw std::exception();
            }

            if (pthread_detach(m_threads[i])) {
                delete[] m_threads;
                throw std::exception();
            }
        }
    }

//析构函数
template <typename T>
threadpool<T> :: ~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

//将请求加入到工作队列
template<typename T >
bool threadpool<T > :: append(T* request) {
    //操作工作队列时加锁，因为它被所有线程共享
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//请求队列的信号量+1
    return true;
}

//工作线程的运行函数
template <typename T > 
void* threadpool<T > :: worker(void* arg) {
    threadpool* pool = (threadpool*) arg;
    pool->run();
    return pool;
}

template <typename T >
void threadpool<T > :: run() {
    while(!m_stop) {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (! request) {
            continue;
        }
        request->process();
    }
}

#endif
