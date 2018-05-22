//
//  Scheduler.cpp
//  Scheduler
//
//  Created by Lin Luo on 4/22/15.
//  Copyright (c) 2015 Parallel Interactive LLC. All rights reserved.
//

#include <boost/context/all.hpp>

#include "Scheduler.h"

#define STACK_SIZE 8192

namespace pi
{
    class Task
    {
        friend class Scheduler;

    public:
        typedef std::unique_ptr<Task> ptr;
        typedef WeakPtr<Task> weak_ptr;

        Task( IScheduler::TaskFunc&& f )
        : m_f( std::move(f) )
        , m_fct( boost::context::make_fcontext(m_sp.data()+m_sp.size(), m_sp.size(), static_entry) ) // NB: the stack is growing downwards!
        {
        }

        Task(const Task&) = delete;
        Task(Task&&) = delete;

        Task& operator=(const Task&) = delete;
        Task& operator=(Task&&) = delete;

        void Run()
        {
            m_f();
        }

    private:
        std::array<uint8_t, STACK_SIZE> m_sp;

        IScheduler::TaskFunc m_f;
        boost::context::fcontext_t m_fct;

    private:
        // this is the current task entry point
        static void static_entry(intptr_t p);
    };

    struct Event
    {
        typedef std::unique_ptr<Event> ptr;

        Event()
        : m_signaled(false)
        {
        }

        bool m_signaled;
        std::list< std::pair<Task::ptr, std::chrono::high_resolution_clock::time_point> > m_waiting; // NB: use list instead of deque for fast removal
        std::map< std::chrono::high_resolution_clock::time_point, std::unordered_map< Task::weak_ptr, decltype(m_waiting)::iterator > > m_timed;
    };

    // The Scheduler runs in the main thread
    class Scheduler : public IScheduler
    {
    public:
        static Scheduler& Singleton()
        {
            static Scheduler m_singleton;
            return m_singleton;
        }

        void CreateTask(TaskFunc&& f) override
        {
            // the new task is put to the end of the ready queue, and will be picked up later inside Tick()
            m_ready.emplace_back( Task::ptr( new Task( std::move(f) ) ), 0 );
        }

        EventID CreateEvent() override
        {
            Event::ptr event( new Event() );
            EventID eventId = reinterpret_cast<EventID>( event.get() );
            m_events.emplace( eventId, std::move(event) );
            return eventId;
        }

        bool WaitForEvent(EventID eventId, std::chrono::milliseconds timeout) override
        {
            if (m_current)
            {
                auto it = m_events.find(eventId);
                if ( it != m_events.end() )
                {
                    if (it->second->m_signaled)
                    {
                        it->second->m_signaled = false;
                        return true;
                    }

                    if ( timeout == std::chrono::milliseconds(0) )
                    {
                        // this is effectively a poll, so don't block
                        return false;
                    }

                    // put the current task to the waiting queue
                    auto tp = timeout != std::chrono::milliseconds(-1) ?
                        std::chrono::high_resolution_clock::now() + timeout : std::chrono::high_resolution_clock::time_point();
                    it->second->m_waiting.push_back( {std::move(m_current), tp} );
                    auto i = it->second->m_waiting.end();
                    --i; // i now points to the task that was just put to the end of the waiting list

                    // timeout of -1 indicates wait forever, so we don't track it in the timed waiting list
                    if ( timeout != std::chrono::milliseconds(-1) )
                    {
                        it->second->m_timed[tp].emplace(i->first, i);
                    }

                    // now jump back to main
                    intptr_t r = boost::context::jump_fcontext(&i->first->m_fct, m_fcm, 0); // 0 indicates that the task is not finished
                    return r == 1; // when control jumps back from main, either the wait timed out (0), or event was signaled (1)
                }
            }

            return true; // either not inside a task, or event is invalid, we just return true to pretend everything is fine
        }

        void SetEvent(EventID eventId) override
        {
            auto it = m_events.find(eventId);
            if ( it != m_events.end() )
            {
                if ( !it->second->m_waiting.empty() )
                {
                    // grab the first waiting task, and move it to the ready queue; also, cleanup the entry in the timed wait queue, if tracked
                    m_ready.emplace_back( std::move(it->second->m_waiting.front().first), 1 ); // 1 indicates that the event is signaled
                    auto tp = it->second->m_waiting.front().second;
                    it->second->m_waiting.pop_front();
                    auto x = it->second->m_timed.find(tp);
                    if ( x != it->second->m_timed.end() )
                    {
                        x->second.erase(m_ready.back().first);
                        if ( x->second.empty() )
                        {
                            it->second->m_timed.erase(x);
                        }
                    }
                }
                else
                {
                    it->second->m_signaled = true;
                }
            }
        }

        void DeleteEvent(EventID eventId) override
        {
            auto it = m_events.find(eventId);
            if ( it != m_events.end() )
            {
                for (auto& each : it->second->m_waiting)
                {
                    m_ready.emplace_back( std::move(each.first), 1 ); // acts as if the event is signaled!
                }

                m_events.erase(it);
            }
        }

        void Yield() override
        {
            if (m_current)
            {
                // puts the current task to the end of the ready queue, and jump back to the main context
                // we do the requeue before jumping, because the call site has most knowledge, otherwise we have to pass some extra
                // info back to the main context (heap allocation for the info converted to intptr_t), and then we have to branch there
                m_ready.emplace_back( std::move(m_current), 0 );
                intptr_t x = boost::context::jump_fcontext(&m_ready.back().first->m_fct, m_fcm, 0); // 0 indicates that the task is not finished

                // this is the place when this task will be resumed, later
                // x contains whatever value the main context will pass back, if any
                (void)x;
            }
        }

        void Sleep(std::chrono::milliseconds duration) override
        {
            if ( duration == std::chrono::milliseconds(0) )
            {
                Yield();
            }
            else if (m_current)
            {
                auto i = m_sleeping.emplace( std::chrono::high_resolution_clock::now() + duration, std::move(m_current) );
                intptr_t x = boost::context::jump_fcontext(&i->second->m_fct, m_fcm, 0); // 0 indicates that the task is not finished

                // this is the place when this task is resumed, when this task is woken up
                (void)x;
            }
        }

        void Tick() override
        {
            // NB: can only be invoked from the main context
            if (!m_current)
            {
                auto now = std::chrono::high_resolution_clock::now();

                // 1. check the sleeping tasks, and move the awaken ones to the end of the ready queue
                auto x = m_sleeping.upper_bound(now);
                auto n = m_sleeping.begin();
                while (n != x)
                {
                    auto i = n++;

                    m_ready.emplace_back( std::move(i->second), 0 );
                    m_sleeping.erase(i);
                }

                // 2. check all the events, and check for timed waiting tasks, if any one timed out, move them to the ready queue
                for (auto& e : m_events)
                {
                    auto x = e.second->m_timed.upper_bound(now);
                    auto n = e.second->m_timed.begin();
                    while (n != x)
                    {
                        auto i = n++;

                        // for each time point that expires, wake up the tasks underneath
                        for (auto& t : i->second)
                        {
                            m_ready.emplace_back( std::move(t.second->first), 0 ); // 0 indicates timeout; SetEvent will use 1 to indicate event signaled
                            e.second->m_waiting.erase(t.second); // remove the stale entry from the waiting list
                        }
                        e.second->m_timed.erase(i); // remove the expired time point
                    }
                }

                // 3. grab the first item from the ready queue, set it to m_current, and jump to it,
                // the return value of jump_context indicates if the task is finished or not
                if ( !m_ready.empty() )
                {
                    // NB: setup and cleanup everything before we jump!
                    m_current = std::move( m_ready.front().first );
                    intptr_t x = m_ready.front().second;
                    m_ready.pop_front();

                    intptr_t r = boost::context::jump_fcontext(&m_fcm, m_current->m_fct, x);
                    if (r == 1)
                    {
                        // if the task is finished, m_current.reset() to destroy the task
                        m_current = nullptr;
                    }

                    // otherwise, the task is not finished, m_current should have already been moved to either the end of the ready queue, timer queue, or
                    // the event queues, and m_current should be set to nullptr before the control "returns" from jump_context
                }
            }
        }

        void TaskEnter()
        {
            m_current->Run();
        }

        void TaskLeave()
        {
            // jump back to the main context, pass 1 as the return value to indicate that the task has finished its course
            // we'll delete the finished task (m_current) after the control is jumped back to main
            (void)boost::context::jump_fcontext(&m_current->m_fct, m_fcm, 1); // (void) to signify that the return value is not used - it won't "return"!

            // the control will never get here
        }

    protected:
        Scheduler()
        : m_fcm(nullptr)
        {
        }

        ~Scheduler()
        {
        }

    private:
        boost::context::fcontext_t m_fcm;

        Task::ptr m_current;
        std::deque< std::pair<Task::ptr, intptr_t> > m_ready;
        std::unordered_map<EventID, Event::ptr> m_events;

        std::multimap<std::chrono::high_resolution_clock::time_point, Task::ptr> m_sleeping;
    };

    IScheduler& IScheduler::Singleton()
    {
        return Scheduler::Singleton();
    }

    void Task::static_entry(intptr_t p)
    {
        // when control reaches here, Scheduler.m_current has been setup already
        Scheduler::Singleton().TaskEnter();
        Scheduler::Singleton().TaskLeave();
    }
}

