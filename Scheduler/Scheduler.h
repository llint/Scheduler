//
//  Scheduler.h
//  Scheduler
//
//  Created by Lin Luo on 4/22/15.
//  Copyright (c) 2015 Parallel Interactive LLC. All rights reserved.
//

#ifndef Scheduler_Scheduler_h
#define Scheduler_Scheduler_h

#include "Utilities/Types.h"
#include "Utilities/WeakPtr.h"

namespace pi
{
    struct IScheduler
    {
        static IScheduler& Singleton();

        typedef intptr_t EventID;
        
        /**
         * Creates a new task and puts it to the end of the ready queue
         * This function can be called from either the main context or a task
         */
        typedef std::function<void ()> TaskFunc;
        virtual void CreateTask(TaskFunc&& f) = 0;

        /**
         * Creates a non-signaled and auto-reset event
         * This function can be invoked from the main context or a task context
         */
        virtual EventID CreateEvent() = 0;

        /**
         * Waits for an event to be set, the current task will be put to the event's waiting queue
         * This function can only be invoked from a task context
         * returns true if the wait is fulfilled via the actual signal
         * returns false if the wait timed out
         * @timeoutMs: timeout value in milliseconds - note that the timeout is not exact, since the wait is cooperative
         * -1 means to wait forever until the event is signaled, 0 means timeout immediately if the event is not signaled (returns false)
         * if the event is signaled, and a task invokes this function, the task will not be blocked, but the event will be reset automatically
         */
        virtual bool WaitForEvent( EventID event, std::chrono::milliseconds timeout = std::chrono::milliseconds(-1) ) = 0;

        /**
         * Sets an event to be signaled; if there are any waiting tasks, puts the first waiting task to the ready queue
         * the event will be auto reset after releasing one waiting task, and it remains signaled if there are no waiting tasks
         */
        virtual void SetEvent(EventID event) = 0;

        /**
         * Deletes the event, and releases all the waiting tasks and puts them to the ready queue
         * WaitForEvent will return true, as if the event is signaled
         */
        virtual void DeleteEvent(EventID event) = 0;

        /**
         * Yields the execution of the current task, and puts it to the end of the ready queue
         * This function can only be invoked from within a task - do not invoke this from the main context
         */
        virtual void Yield() = 0;

        /**
         * Puts the current task to sleep for at least durationMs in milliseconds
         * This function can only be invoked from within a task context
         * Sleep(0) is semantically the same as Yield()
         */
        virtual void Sleep(std::chrono::milliseconds duration) = 0;

        /**
         * Ticks the scheduler so the next ready task can be executed, until it finishes, yields, or blocks
         * This function can only be invoked from the main context - never invoke this from within any task
         */
        virtual void Tick() = 0;
    };
}

#endif
