#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#if JUCE_INTEL
 #include <immintrin.h>
#endif

// A few worker threads that help the audio thread with a job that falls into independent parts (the tracks of one block). run() hands out the parts,
// the calling thread works on them too, and it returns when all are done. Nothing is allocated while running. The workers spin for a very short
// time after a job, so the next block (a millisecond or so later) finds them awake, and then sleep so they cost no CPU when the audio is idle.
class ParallelRunner
{
public:
    ParallelRunner() = default;
    ~ParallelRunner() { setThreads (0); }

    // Message thread; not while run() is active (the engine calls it with its lock held). 0 = everything on the calling thread.
    void setThreads (int numWorkers)
    {
        numWorkers = juce::jlimit (0, 32, numWorkers);
        if (numWorkers == (int) workers.size())
            return;
        stopAll();
        for (int i = 0; i < numWorkers; ++i)
        {
            workers.push_back (std::make_unique<Worker> (*this, i));
            workers.back()->startThread (juce::Thread::Priority::highest);
        }
    }

    int getThreads() const { return (int) workers.size(); }

    template <typename Fn>
    void run (int count, Fn&& fn)
    {
        if (count <= 0)
            return;
        if (workers.empty() || count == 1)
        {
            for (int i = 0; i < count; ++i)
                fn (i);
            return;
        }

        Task<Fn> task { &fn };
        nextIndex.store (1 << 30);   // (a worker that is late with the last job finds nothing to do while the new one is set up)
        jobCount = count;
        job = &Task<Fn>::call;
        jobContext = &task;
        doneCount.store (0);
        nextIndex.store (0);
        generation.fetch_add (1);

        const int wake = juce::jmin ((int) workers.size(), count - 1);   // (no need to wake more than there are parts to do)
        for (int i = 0; i < wake; ++i)
            workers[(size_t) i]->wakeUp();

        work();   // the calling thread helps
        while (doneCount.load() < count)   // the last parts are with the workers
            pause();
    }

private:
    template <typename Fn> struct Task
    {
        Fn* fn;
        static void call (void* c, int i) { (*static_cast<Task*> (c)->fn) (i); }
    };

    static void pause()
    {
       #if JUCE_INTEL
        _mm_pause();
       #else
        std::this_thread::yield();
       #endif
    }

    void work()
    {
        for (;;)
        {
            const int i = nextIndex.fetch_add (1);
            if (i >= jobCount)
                return;
            job (jobContext, i);
            doneCount.fetch_add (1);
        }
    }

    struct Worker : juce::Thread
    {
        Worker (ParallelRunner& o, int index) : juce::Thread ("UFODAW audio worker " + juce::String (index + 1)), owner (o) {}
        ~Worker() override { stopThread (2000); }

        void wakeUp() { event.signal(); }

        void run() override
        {
            unsigned seen = owner.generation.load();
            while (! threadShouldExit())
            {
                // spin for a short while (the next block follows soon), then sleep until woken
                bool got = false;
                for (int spin = 0; spin < 4000 && ! got; ++spin)
                {
                    if (owner.generation.load() != seen)
                        got = true;
                    else
                        ParallelRunner::pause();
                }
                if (! got)
                {
                    while (owner.generation.load() == seen && ! threadShouldExit())
                        event.wait (50);
                    if (threadShouldExit())
                        return;
                }
                seen = owner.generation.load();
                const juce::ScopedNoDenormals noDenormals;
                owner.work();
            }
        }

        ParallelRunner& owner;
        juce::WaitableEvent event;
    };

    void stopAll()
    {
        for (auto& w : workers)
            w->signalThreadShouldExit();
        for (auto& w : workers)
            w->wakeUp();
        workers.clear();   // (joins the threads)
    }

    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<unsigned> generation { 0 };
    std::atomic<int> nextIndex { 0 }, doneCount { 0 };
    int jobCount = 0;
    void (*job) (void*, int) = nullptr;
    void* jobContext = nullptr;
};
