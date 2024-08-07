/**********************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  CPL worker thread pool
 * Author:   Even Rouault, <even dot rouault at spatialys dot com>
 *
 **********************************************************************
 * Copyright (c) 2015, Even Rouault, <even dot rouault at spatialys dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_worker_thread_pool.h"

#include <iostream>

#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_vsi.h"

/************************************************************************/
/*                         CPLWorkerThreadPool()                        */
/************************************************************************/

/** Instantiate a new pool of worker threads.
 *
 * The pool is in an uninitialized state after this call. The Setup() method
 * must be called.
 */
CPLWorkerThreadPool::CPLWorkerThreadPool() : jobQueue{}
{
}

/** Instantiate a new pool of worker threads.
 *
 * \param nThreads  Number of threads in the pool.
 */
CPLWorkerThreadPool::CPLWorkerThreadPool(int nThreads) : jobQueue{}
{
    Setup(nThreads, nullptr, nullptr);
}

/************************************************************************/
/*                          ~CPLWorkerThreadPool()                      */
/************************************************************************/

/** Destroys a pool of worker threads.
 *
 * Any still pending job will be completed before the destructor returns.
 */
CPLWorkerThreadPool::~CPLWorkerThreadPool()
{
    Stop();
}

/************************************************************************/
/*                        GetThreadCount()                              */
/************************************************************************/

int CPLWorkerThreadPool::GetThreadCount() const
{
    std::unique_lock<std::mutex> oGuard(m_mutex);
    return static_cast<int>(m_workers.size());
}

/************************************************************************/
/*                       WorkerThreadFunction()                         */
/************************************************************************/

void CPLWorkerThreadPool::WorkerThreadFunction()
{
    size_t threadNum;
    {
        std::lock_guard<std::mutex> oGuard(m_mutex);
        threadNum = nNumThreadsStarted++;
    }

    // There is a race condition on threadNum (the init function for thread 2 could
    // run before the init function for thread 1), but it doesn't really matter since
    // it's just used as a unique index into the init data and that is also arbitrary.

    if (m_initFunc)
        m_initFunc(m_initData[threadNum]);

    while (true)
    {
        std::function<void()> task = GetNextJob();
        if (!task)
            break;
        task();
        DeclareJobFinished();
    }
}

/************************************************************************/
/*                             SubmitJob()                              */
/************************************************************************/

/** Queue a new job.
 *
 * @param pfnFunc Function to run for the job.
 * @param pData User data to pass to the job function.
 * @return true in case of success.
 */
bool CPLWorkerThreadPool::SubmitJob(CPLThreadFunc pfnFunc, void *pData)
{
    return SubmitJob([=] { pfnFunc(pData); });
}

/** Queue a new job.
 *
 * @param task  Void function to execute.
 * @return true in case of success.
 */
bool CPLWorkerThreadPool::SubmitJob(std::function<void()> task)
{
    std::unique_lock<std::mutex> oGuard(m_mutex);

    if (eState == CPLWTS_STOP)
        return false;

    jobQueue.emplace(task);
    nPendingJobs++;
    oGuard.unlock();

    m_cv.notify_all();

    return true;
}

/************************************************************************/
/*                             SubmitJobs()                              */
/************************************************************************/

/** Queue several jobs
 *
 * @param pfnFunc Function to run for the job.
 * @param apData User data instances to pass to the job function.
 * @return true in case of success.
 */
bool CPLWorkerThreadPool::SubmitJobs(CPLThreadFunc pfnFunc,
                                     const std::vector<void *> &apData)
{
    std::unique_lock<std::mutex> oGuard(m_mutex);

    if (eState == CPLWTS_STOP)
        return false;

    for (void *pData : apData)
    {
        jobQueue.emplace([=] { pfnFunc(pData); });
        nPendingJobs++;
    }
    oGuard.unlock();

    m_cv.notify_all();

    return true;
}

/************************************************************************/
/*                            WaitCompletion()                          */
/************************************************************************/

/** Wait for completion of part or whole jobs.
 *
 * @param nMaxRemainingJobs Maximum number of pendings jobs that are allowed
 *                          in the queue after this method has completed. Might
 * be 0 to wait for all jobs.
 */
void CPLWorkerThreadPool::WaitCompletion(int nMaxRemainingJobs)
{
    if (nMaxRemainingJobs < 0)
        nMaxRemainingJobs = 0;
    std::unique_lock<std::mutex> oGuard(m_mutex);

    // Wait until the number of pending jobs is at or under the max remaining jobs.
    m_cv.wait(oGuard, [this, nMaxRemainingJobs]
              { return nPendingJobs <= nMaxRemainingJobs; });
}

/************************************************************************/
/*                            Stop()                               */
/************************************************************************/

/** Wait for threads to complete and stop the pool.
 */
void CPLWorkerThreadPool::Stop()
{
    {
        std::lock_guard<std::mutex> oGuard(m_mutex);
        if (eState == CPLWTS_STOP)
            return;
    }

    // Wait until the actual work is done.
    WaitCompletion();

    // Tell all the worker threads to stop.
    {
        std::lock_guard<std::mutex> oGuard(m_mutex);
        eState = CPLWTS_STOP;
    }
    m_cv.notify_all();

    for (std::thread &thread : m_workers)
        thread.join();
    m_workers.clear();
}

/************************************************************************/
/*                            WaitEvent()                               */
/************************************************************************/

/** Wait for completion of at least one job, if there are any remaining
 */
void CPLWorkerThreadPool::WaitEvent()
{
    std::unique_lock<std::mutex> oGuard(m_mutex);

    if (nPendingJobs == 0)
        return;

    m_cv.wait(oGuard, [this, nPreviousPending = nPendingJobs]
              { return nPendingJobs < nPreviousPending; });
}

/************************************************************************/
/*                                Setup()                               */
/************************************************************************/

/** Setup the pool.
 *
 * @param nThreads Number of threads to launch
 * @param pfnInitFunc Initialization function to run in each thread. May be NULL
 * @param pasInitData Array of initialization data. Its length must be nThreads,
 *                    or it should be NULL.
 * @return true if initialization was successful.
 */
bool CPLWorkerThreadPool::Setup(int nThreads, CPLThreadFunc pfnInitFunc,
                                void **pasInitData)
{
    return Setup(nThreads, pfnInitFunc, pasInitData, true);
}

/** Setup the pool.
 *
 * @param nThreads Number of threads to launch
 * @param pfnInitFunc Initialization function to run in each thread. May be NULL
 * @param pasInitData Array of initialization data. Its length must be nThreads,
 *                    or it should be NULL.
 * @param bWaitallStarted Whether to wait for all threads to be fully started.
 * @return true if initialization was successful.
 */
bool CPLWorkerThreadPool::Setup(int nThreads, CPLThreadFunc pfnInitFunc,
                                void **pasInitData, bool bWaitallStarted)
{
    std::unique_lock<std::mutex> oGuard(m_mutex);

    if (nThreads <= 0 || eState == CPLWTS_STOP)
        return false;

    m_initData = pasInitData;
    m_initFunc = pfnInitFunc;
    for (int i = 0; i < nThreads; i++)
        m_workers.emplace_back([this] { WorkerThreadFunction(); });
    /**
    {
        auto wt = std::make_unique<CPLWorkerThread>();
        wt->pfnInitFunc = pfnInitFunc;
        wt->pInitData = pasInitData ? pasInitData[i] : nullptr;
        wt->poTP = this;
        wt->hThread = CPLCreateJoinableThread(WorkerThreadFunction, wt.get());
        if (wt->hThread == nullptr)
            return false;
        aWT.emplace_back(std::move(wt));
    }
    **/

    // NOTE: There is no notification expected. We just use this to periodically check the
    //   numThreadsStarted under lock.
    if (bWaitallStarted)
        m_cv.wait_for(oGuard, std::chrono::milliseconds(10),
                      [this] {
                          return eState == CPLWTS_STOP ||
                                 nNumThreadsStarted == m_workers.size();
                      });

    return eState == CPLWTS_OK;
}

/************************************************************************/
/*                          DeclareJobFinished()                        */
/************************************************************************/

void CPLWorkerThreadPool::DeclareJobFinished()
{
    {
        std::lock_guard<std::mutex> oGuard(m_mutex);
        nPendingJobs--;
    }

    m_cv.notify_all();
}

/************************************************************************/
/*                             GetNextJob()                             */
/************************************************************************/

std::function<void()> CPLWorkerThreadPool::GetNextJob()
{
    std::unique_lock<std::mutex> oGuard(m_mutex);

    m_cv.wait(oGuard,
              [this] { return eState == CPLWTS_STOP || jobQueue.size(); });
    if (eState == CPLWTS_STOP)
        return std::function<void()>();
    auto task = std::move(jobQueue.front());
    jobQueue.pop();
    return task;
}

/************************************************************************/
/*                         CreateJobQueue()                             */
/************************************************************************/

/** Create a new job queue based on this worker thread pool.
 *
 * The worker thread pool must remain alive while the returned object is
 * itself alive.
 *
 * @since GDAL 3.2
 */
std::unique_ptr<CPLJobQueue> CPLWorkerThreadPool::CreateJobQueue()
{
    return std::unique_ptr<CPLJobQueue>(new CPLJobQueue(this));
}

/************************************************************************/
/*                            CPLJobQueue()                             */
/************************************************************************/

//! @cond Doxygen_Suppress
CPLJobQueue::CPLJobQueue(CPLWorkerThreadPool *poPool) : m_poPool(poPool)
{
}

//! @endcond

/************************************************************************/
/*                           ~CPLJobQueue()                             */
/************************************************************************/

CPLJobQueue::~CPLJobQueue()
{
    WaitCompletion();
}

/************************************************************************/
/*                           JobQueueJob                                */
/************************************************************************/

struct JobQueueJob
{
    std::function<void()> task;
    CPLJobQueue *poQueue = nullptr;
};

/************************************************************************/
/*                          JobQueueFunction()                          */
/************************************************************************/

void CPLJobQueue::JobQueueFunction(void *pData)
{
    JobQueueJob *poJob = static_cast<JobQueueJob *>(pData);
    if (poJob->task)
        poJob->task();
    poJob->poQueue->DeclareJobFinished();
    delete poJob;
}

/************************************************************************/
/*                          DeclareJobFinished()                        */
/************************************************************************/

void CPLJobQueue::DeclareJobFinished()
{
    {
        std::lock_guard<std::mutex> oGuard(m_mutex);
        m_nPendingJobs--;
    }
    m_cv.notify_one();
}

/************************************************************************/
/*                             SubmitJob()                              */
/************************************************************************/

/** Queue a new job.
 *
 * @param task Function to run for the job.
 * @return true in case of success.
 */
bool CPLJobQueue::SubmitJob(std::function<void()> task)
{
    JobQueueJob *poJob = new JobQueueJob;
    poJob->poQueue = this;
    poJob->task = task;
    {
        std::lock_guard<std::mutex> oGuard(m_mutex);
        m_nPendingJobs++;
    }
    bool submitted = m_poPool->SubmitJob(JobQueueFunction, poJob);
    if (!submitted)
        DeclareJobFinished();
    return submitted;
}

/** Queue a new job.
 *
 * @param pfnFunc Function to run for the job.
 * @param pData User data to pass to the job function.
 * @return true in case of success.
 */
bool CPLJobQueue::SubmitJob(CPLThreadFunc pfnFunc, void *pData)
{
    return SubmitJob([=] { pfnFunc(pData); });
}

/************************************************************************/
/*                            WaitCompletion()                          */
/************************************************************************/

/** Wait for completion of part or whole jobs.
 *
 * @param nMaxRemainingJobs Maximum number of pendings jobs that are allowed
 *                          in the queue after this method has completed. Might
 * be 0 to wait for all jobs.
 */
void CPLJobQueue::WaitCompletion(int nMaxRemainingJobs)
{
    std::unique_lock<std::mutex> oGuard(m_mutex);
    m_cv.wait(oGuard, [this, nMaxRemainingJobs]
              { return m_nPendingJobs <= nMaxRemainingJobs; });
}

/************************************************************************/
/*                             WaitEvent()                              */
/************************************************************************/

/** Wait for completion for at least one job.
 *
 * @return true if there are remaining jobs.
 */
bool CPLJobQueue::WaitEvent()
{
    std::unique_lock<std::mutex> oGuard(m_mutex);

    if (m_nPendingJobs == 0)
        return false;

    m_cv.wait(oGuard, [this, nPreviousPending = m_nPendingJobs]
              { return m_nPendingJobs < nPreviousPending; });
    return m_nPendingJobs > 0;
}
