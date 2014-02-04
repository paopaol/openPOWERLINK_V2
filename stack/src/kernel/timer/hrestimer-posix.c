/**
********************************************************************************
\file   hrestimer-posix.c

\brief  High-resolution timer module for Linux using Posix timer functions

This module is the target specific implementation of the high-resolution
timer module for Linux userspace. It uses Posix timer functions for its
implementation.

\ingroup module_hrestimer
*******************************************************************************/

/*------------------------------------------------------------------------------
Copyright (c) 2012, SYSTEC electronic GmbH
Copyright (c) 2014, Bernecker+Rainer Industrie-Elektronik Ges.m.b.H. (B&R)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holders nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL COPYRIGHT HOLDERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
------------------------------------------------------------------------------*/

//------------------------------------------------------------------------------
// includes
//------------------------------------------------------------------------------
#include <oplk/EplInc.h>
#include <kernel/hrestimer.h>
#include <oplk/benchmark.h>

#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>

//============================================================================//
//            G L O B A L   D E F I N I T I O N S                             //
//============================================================================//

//------------------------------------------------------------------------------
// const defines
//------------------------------------------------------------------------------

#define TIMER_COUNT           2            ///< number of high-resolution timers
#define TIMER_MIN_VAL_SINGLE  20000        ///< minimum timer intervall for single timeouts
#define TIMER_MIN_VAL_CYCLE   100000       ///< minimum timer intervall for continuous timeouts

#define SIGHIGHRES           SIGRTMIN + 1

/* macros for timer handles */
#define TIMERHDL_MASK         0x0FFFFFFF
#define TIMERHDL_SHIFT        28
#define HDL_TO_IDX(Hdl)       ((Hdl >> TIMERHDL_SHIFT) - 1)
#define HDL_INIT(Idx)         ((Idx + 1) << TIMERHDL_SHIFT)
#define HDL_INC(Hdl)          (((Hdl + 1) & TIMERHDL_MASK) | (Hdl & ~TIMERHDL_MASK))

//------------------------------------------------------------------------------
// module global vars
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// global function prototypes
//------------------------------------------------------------------------------

//============================================================================//
//          P R I V A T E   D E F I N I T I O N S                             //
//============================================================================//

//------------------------------------------------------------------------------
// const defines
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// local types
//------------------------------------------------------------------------------

/**
\brief  High-resolution timer information structure

The structure contains all necessary information for a high-resolution timer.
*/
typedef struct
{
    tEplTimerEventArg   eventArg;       ///< Event argument
    tEplTimerkCallback  pfnCallback;    ///< Pointer to timer callback function
    timer_t             timer;          ///< timer_t struct of this timer
} tHresTimerInfo;

/**
\brief  High-resolution timer instance

The structure defines a high-resolution timer module instance.
*/
typedef struct
{
    tHresTimerInfo      aTimerInfo[TIMER_COUNT];    ///< Array with timer information for a set of timers
    pthread_t           threadId;                   ///< Timer thread Id
} tHresTimerInstance;

//------------------------------------------------------------------------------
// module local vars
//------------------------------------------------------------------------------
static tHresTimerInstance       hresTimerInstance_l;

//------------------------------------------------------------------------------
// local function prototypes
//------------------------------------------------------------------------------
static void * timerThread(void *pParm_p);

//============================================================================//
//            P U B L I C   F U N C T I O N S                                 //
//============================================================================//

//------------------------------------------------------------------------------
/**
\brief    Initialize high-resolution timer module

The function initializes the high-resolution timer module

\return Returns a tEplKernel error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tEplKernel hrestimer_init(void)
{
    return hrestimer_addInstance();
}

//------------------------------------------------------------------------------
/**
\brief    Add instance of high-resolution timer module

The function adds an instance of the high-resolution timer module.

\return Returns a tEplKernel error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tEplKernel hrestimer_addInstance(void)
{
    tEplKernel              ret = kEplSuccessful;
    UINT                    index;
    struct sched_param      schedParam;
    tHresTimerInfo*         pTimerInfo;
    struct sigevent         sev;

    EPL_MEMSET(&hresTimerInstance_l, 0, sizeof (hresTimerInstance_l));

    /* Initialize timer threads for all usable timers. */
    for (index = 0; index < TIMER_COUNT; index++)
    {
        pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];

        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo = SIGHIGHRES;
        sev.sigev_value.sival_ptr = pTimerInfo;

        if (timer_create(CLOCK_MONOTONIC, &sev, &pTimerInfo->timer) != 0)
        {
            return kEplNoResource;
        }
    }

    if (pthread_create(&hresTimerInstance_l.threadId, NULL,
                       timerThread, NULL) != 0)
    {
        return kEplNoResource;
    }

    schedParam.__sched_priority = EPL_THREAD_PRIORITY_HIGH;
    if (pthread_setschedparam(hresTimerInstance_l.threadId, SCHED_FIFO, &schedParam) != 0)
    {
        EPL_DBGLVL_ERROR_TRACE("%s() Couldn't set thread scheduling parameters!\n", __func__);
        pthread_cancel(hresTimerInstance_l.threadId);
        return kEplNoResource;
    }

    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Delete instance of high-resolution timer module

The function deletes an instance of the high-resolution timer module.

\return Returns a tEplKernel error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tEplKernel hrestimer_delInstance(void)
{
    tHresTimerInfo*         pTimerInfo;
    tEplKernel              ret = kEplSuccessful;
    UINT                    index;

    for (index = 0; index < TIMER_COUNT; index++)
    {
        pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];
        timer_delete(pTimerInfo->timer);
        pTimerInfo->eventArg.m_TimerHdl = 0;
        pTimerInfo->pfnCallback = NULL;
    }

    /* send exit signal to thread */
    pthread_cancel(hresTimerInstance_l.threadId);
    /* wait until thread terminates */
    EPL_DBGLVL_TIMERH_TRACE("%s() Waiting for thread to exit...\n", __func__);

    pthread_join(hresTimerInstance_l.threadId, NULL);
    EPL_DBGLVL_TIMERH_TRACE("%s() Thread exited!\n", __func__);

    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Modify a high-resolution timer

The function modifies the timeout of the timer with the specified handle.
If the handle, the pointer points to, is zero, the timer must be created first.
If it is not possible to stop the old timer, this function always assures that
the old timer does not trigger the callback function with the same handle as
the new timer. That means the callback function must check the passed handle
with the one returned by this function. If these are unequal, the call can be
discarded.

\param  pTimerHdl_p     Pointer to timer handle.
\param  time_p          Relative timeout in [ns].
\param  pfnCallback_p   Callback function, which is called when timer expires.
                        (The function is called mutual exclusive with the Edrv
                        callback functions (Rx and Tx)).
\param  argument_p      User-specific argument
\param  fContinue_p     If TRUE, callback function will be called continuously.
                        Otherwise, it is a one-shot timer.

\return Returns a tEplKernel error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tEplKernel hrestimer_modifyTimer(tEplTimerHdl* pTimerHdl_p, ULONGLONG time_p,
                                 tEplTimerkCallback pfnCallback_p, ULONG argument_p,
                                 BOOL fContinue_p)
{
    tEplKernel              ret = kEplSuccessful;
    UINT                    index;
    tHresTimerInfo*         pTimerInfo;
    struct itimerspec       RelTime;

    // check pointer to handle
    if(pTimerHdl_p == NULL)
    {
        EPL_DBGLVL_ERROR_TRACE("%s() Invalid timer handle\n", __func__);
        return kEplTimerInvalidHandle;
    }

    if (*pTimerHdl_p == 0)
    {   // no timer created yet -> search free timer info structure
        pTimerInfo = &hresTimerInstance_l.aTimerInfo[0];
        for (index = 0; index < TIMER_COUNT; index++, pTimerInfo++)
        {
            if (pTimerInfo->eventArg.m_TimerHdl == 0)
            {   // free structure found
                break;
            }
        }
        if (index >= TIMER_COUNT)
        {   // no free structure found
            EPL_DBGLVL_ERROR_TRACE("%s() Invalid timer index:%d\n", __func__, index);
            return kEplTimerNoTimerCreated;
        }
        pTimerInfo->eventArg.m_TimerHdl = HDL_INIT(index);
    }
    else
    {
        index = HDL_TO_IDX(*pTimerHdl_p);
        if (index >= TIMER_COUNT)
        {   // invalid handle
            EPL_DBGLVL_ERROR_TRACE("%s() Invalid timer index:%d\n", __func__, index);
            return kEplTimerInvalidHandle;
        }
        pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];
    }

    // increase too small time values
    if (fContinue_p != FALSE)
    {
        if (time_p < TIMER_MIN_VAL_CYCLE)
            time_p = TIMER_MIN_VAL_CYCLE;
    }
    else
    {
        if (time_p < TIMER_MIN_VAL_SINGLE)
            time_p = TIMER_MIN_VAL_SINGLE;
    }

    /* increment timer handle
     * (if timer expires right after this statement, the user
     * would detect an unknown timer handle and discard it) */
    pTimerInfo->eventArg.m_TimerHdl = HDL_INC(pTimerInfo->eventArg.m_TimerHdl);
    *pTimerHdl_p = pTimerInfo->eventArg.m_TimerHdl;

    /* initialize timer info */
    pTimerInfo->eventArg.m_Arg.m_dwVal = argument_p;
    pTimerInfo->pfnCallback      = pfnCallback_p;

    if (time_p >= 1000000000L)
    {
        RelTime.it_value.tv_sec = (time_p / 1000000000L);
        RelTime.it_value.tv_nsec = (time_p % 1000000000) ;
    }
    else
    {
        RelTime.it_value.tv_sec = 0;
        RelTime.it_value.tv_nsec = time_p;
    }

    if (fContinue_p)
    {
        RelTime.it_interval.tv_nsec = RelTime.it_value.tv_nsec;
        RelTime.it_interval.tv_sec = RelTime.it_value.tv_sec;
    }
    else
    {
        RelTime.it_interval.tv_nsec = 0;
        RelTime.it_interval.tv_sec = 0;
    }

    EPL_DBGLVL_TIMERH_TRACE("%s() timer:%lx timeout=%ld:%ld\n", __func__,
                            pTimerInfo->eventArg.m_TimerHdl,
                            RelTime.it_value.tv_sec, RelTime.it_value.tv_nsec);

    timer_settime(pTimerInfo->timer, 0, &RelTime, NULL);

    return ret;
}

//------------------------------------------------------------------------------
/**
\brief    Delete a high-resolution timer

The function deletes an created high-resolution timer. The timer is specified
by its timer handle. After deleting the handle is reset to zero.

\param  pTimerHdl_p     Pointer to timer handle.

\return Returns a tEplKernel error code.

\ingroup module_hrestimer
*/
//------------------------------------------------------------------------------
tEplKernel hrestimer_deleteTimer(tEplTimerHdl* pTimerHdl_p)
{
    tEplKernel                  ret = kEplSuccessful;
    UINT                        index;
    tHresTimerInfo*             pTimerInfo;
    struct itimerspec           relTime;

    EPL_DBGLVL_TIMERH_TRACE("%s() Deleting timer:%lx\n", __func__, *pTimerHdl_p);

    if(pTimerHdl_p == NULL)
        return kEplTimerInvalidHandle;

    if (*pTimerHdl_p == 0)
    {   // no timer created yet
        return ret;
    }
    else
    {
        index = HDL_TO_IDX(*pTimerHdl_p);
        if (index >= TIMER_COUNT)
        {   // invalid handle
            return kEplTimerInvalidHandle;
        }
        pTimerInfo = &hresTimerInstance_l.aTimerInfo[index];
        if (pTimerInfo->eventArg.m_TimerHdl != *pTimerHdl_p)
        {   // invalid handle
            return ret;
        }
    }

    // values of 0 disarms the timer
    relTime.it_value.tv_sec = 0;
    relTime.it_value.tv_nsec = 0;
    timer_settime(pTimerInfo->timer, 0, &relTime, NULL);

    *pTimerHdl_p = 0;
    pTimerInfo->eventArg.m_TimerHdl = 0;
    pTimerInfo->pfnCallback = NULL;

    return ret;
}

//============================================================================//
//            P R I V A T E   F U N C T I O N S                               //
//============================================================================//
/// \name Private Functions
/// \{

//------------------------------------------------------------------------------
/**
\brief    Timer thread function

The function provides the main function of the timer thread.

\param  pParm_p     Thread parameter (unused!)

\return Returns a void* as specified by the pthread interface but it is not used!
*/
//------------------------------------------------------------------------------
static void* timerThread(void *pParm_p)
{
    INT                                 iRet;
    tHresTimerInfo                      *pTimerInfo;
    sigset_t                            awaitedSignal;
    siginfo_t                           signalInfo;

    UNUSED_PARAMETER(pParm_p);

    EPL_DBGLVL_TIMERH_TRACE("%s(): ThreadId:%ld\n", __func__, syscall(SYS_gettid));

    sigemptyset(&awaitedSignal);
    sigaddset(&awaitedSignal, SIGHIGHRES);
    pthread_sigmask(SIG_BLOCK, &awaitedSignal, NULL);

    /* loop forever until thread will be canceled */
    while (1)
    {
        if ((iRet = sigwaitinfo(&awaitedSignal, &signalInfo)) > 0)
        {
            pTimerInfo = (tHresTimerInfo *)signalInfo.si_value.sival_ptr;
            /* call callback function */
            if (pTimerInfo->pfnCallback != NULL)
            {
                pTimerInfo->pfnCallback(&pTimerInfo->eventArg);
            }
        }
    }

    EPL_DBGLVL_TIMERH_TRACE("%s() Exiting!\n", __func__);
    return NULL;
}

/// \}