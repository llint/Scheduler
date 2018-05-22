//
//  main.cpp
//  Scheduler
//
//  Created by Lin Luo on 4/22/15.
//  Copyright (c) 2015 Parallel Interactive LLC. All rights reserved.
//

#include <iostream>

#include <boost/context/all.hpp>

#include "Scheduler.h"

boost::context::fcontext_t fcm,fc1,fc2;

void f1(intptr_t)
{
    std::cout<<"f1: entered"<<std::endl;
    boost::context::jump_fcontext(&fc1,fcm,0);
    std::cout<<"f1: return"<<std::endl;
    boost::context::jump_fcontext(&fc1,fcm,1);
    BOOST_ASSERT(false&&!"f1: never returns");
}

void f2(intptr_t)
{
    std::cout<<"f2: entered"<<std::endl;
    boost::context::jump_fcontext(&fc2,fcm,0);
    std::cout<<"f2: return"<<std::endl;
    boost::context::jump_fcontext(&fc2,fcm,1);
    BOOST_ASSERT(false&&!"f2: never returns");
}

int main(int argc, const char * argv[])
{
    std::size_t size(8192);
    void* sp1(std::malloc(size));
    void* sp2(std::malloc(size));

    fc1=boost::context::make_fcontext((char*)sp1+size,size,f1); // NB: the stack is growing downwards!
    fc2=boost::context::make_fcontext((char*)sp2+size,size,f2); // NB: the stack is growing downwards!

    std::cout<<"main: call jump_fcontext( & fcm, fc1, 0)"<<std::endl;
    std::cout << boost::context::jump_fcontext(&fcm,fc1,0) << std::endl;
    std::cout << boost::context::jump_fcontext(&fcm,fc2,0) << std::endl;
    std::cout << boost::context::jump_fcontext(&fcm,fc1,0) << std::endl;
    std::cout << boost::context::jump_fcontext(&fcm,fc2,0) << std::endl;

    auto event = pi::IScheduler::Singleton().CreateEvent();
    //pi::IScheduler::Singleton().SetEvent(event);

    bool isFinishedTask1 = false;
    pi::IScheduler::Singleton().CreateTask([event, &isFinishedTask1]
    {
        std::cout << "Task1: I'm sleeping for 1 second ..." << std::endl;
        pi::IScheduler::Singleton().Sleep( std::chrono::milliseconds(1000) );
        std::cout << "Task1: I'm setting the event ..." << std::endl;
        pi::IScheduler::Singleton().SetEvent(event);
        isFinishedTask1 = true;
    });

    bool isFinishedTask2 = false;
    pi::IScheduler::Singleton().CreateTask([event, &isFinishedTask2]
    {
        std::cout << "Task2: I'm waiting for the event ..." << std::endl;
        bool signaled = pi::IScheduler::Singleton().WaitForEvent(event, std::chrono::milliseconds(5000));
        std::cout << "Task2: The event was signaled or not: " << (signaled ? "YES" : "NO") << std::endl;
        isFinishedTask2 = true;
    });

    while (!isFinishedTask1 || !isFinishedTask2)
    {
        pi::IScheduler::Singleton().Tick();
    }

    // insert code here...
    std::cout << "Hello, World!\n";
    return 0;
}
