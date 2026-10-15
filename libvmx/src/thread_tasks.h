/*
* MIT License
* 
* Copyright (c) 2025 Open Media Transport Contributors
* 
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
* 
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
* 
*/

#pragma once
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>

// Build marker for QA (skill opencamera-omt-android, rule 4: "verify the native lib was really
// rebuilt"). `strings libvmx.so | grep vmx-thread-task-predicate-wait` is how the APK build proves
// the NDK compiled this header into the shipped library rather than reusing a stale object file.
// File scope, not a struct member: an in-class `static` with an initialiser is a declaration.
__attribute__((used)) static const char VMX_ThreadTaskBuildTag[] = "vmx-thread-task-predicate-wait-p0-11";

struct ThreadTask
{
	std::thread thread;
	std::queue<std::function<void()>> queue;
	std::mutex mtx;
	std::condition_variable cv;
	std::condition_variable complete;
	bool running = false;

	void Join()
	{
		std::unique_lock<std::mutex> lock(mtx);
		// P0 #11 (ported from the desktop vendored copy): wait on the CONDITION, not on the
		// notification. The old body was
		//     if (queue.size() > 0) complete.wait(lock);
		// and std::condition_variable::wait is explicitly allowed to return spuriously — without
		// any notify at all. Join() then reported "the slice work is finished" while tasks were
		// still queued. Its callers are VMX_Decode/VMX_Encode (vmxcodec.cpp:930 and :1708), which
		// return straight into the slice streams, so VMX_Destroy — and the resolution-change path
		// in VMX_SetDimensions that recreates the pool — could free streams a worker was still
		// writing to. Use-after-free with no diagnostic, on the wake that nobody schedules.
		//
		// queue.empty() is the right predicate: a task runs with mtx held (below), so the worker
		// only reaches cv.wait — the one place it releases mtx and lets Join in — after the queue
		// has actually drained.
		complete.wait(lock, [this] { return queue.empty(); });
	}

	void TaskLoop()
	{
		std::unique_lock<std::mutex> lock(mtx);
		for (;;)
		{
			// P0 #11: `running` is read only under mtx. The old loop tested `while (running)`
			// OUTSIDE the lock, so Destroy() could clear running and fire its notify_all() in the
			// window between that test and the cv.wait() further down: the worker went to sleep
			// after the only wakeup had already been delivered, and thread.join() inside Destroy()
			// blocked forever. A hung encoder thread, not a crash — which is why it took a read of
			// the desktop copy to find it. The predicate re-checks running under the mutex it is
			// waiting on, so no wakeup can be lost.
			cv.wait(lock, [this] { return !running || !queue.empty(); });
			if (!running)
			{
				complete.notify_all();
				break;
			}
			std::function<void()> func = std::move(queue.front());
			queue.pop();
			func();  // executed while holding mtx (serialized), exactly as before
			if (queue.empty())
				complete.notify_all();
		}
	}

	void Initialize()
	{
		running = true;
		queue = std::queue<std::function<void()>>();
		thread = std::thread(&ThreadTask::TaskLoop, this);
	}
	void Push(std::function<void()> task)
	{
		{
			std::lock_guard<std::mutex> lock(mtx);
			queue.push(task);
		}
		cv.notify_all();
	}
	void Destroy()
	{
		{
			std::lock_guard<std::mutex> lock(mtx);
			running = false;
		}
		cv.notify_all();
		thread.join();
	}
};

struct ThreadTasks
{
	int numThreads;
	ThreadTask** tasks;
};

static ThreadTasks* CreateTasks(int numThreads)
{
	ThreadTasks* th = new ThreadTasks();
	th->numThreads = numThreads;
	th->tasks = new ThreadTask*[numThreads];
	for (int i = 0; i < numThreads; i++)
	{
		ThreadTask* task = new ThreadTask();
		th->tasks[i] = task;
		task->Initialize();
	}
	return th;
}

static void DestroyTasks(ThreadTasks* tasks)
{
	if (tasks)
	{
		for (int i = 0; i < tasks->numThreads; i++)
		{
			tasks->tasks[i]->Destroy();
			ThreadTask* task = tasks->tasks[i];
			delete task;
		}
		// CreateTasks allocates three things: the struct, the ThreadTask*[] array, and each task.
		// This function freed two of them, so the array leaked — 16 bytes per VMX_Create, and
		// Sender::createEncoder() calls VMX_Create again on every resolution change. ASan caught it
		// in qa/build-host.sh; the desktop vendored copy of this same file (nodos-yded-linux
		// plugins/yd.net.ompout/vendor/libvmx/src/thread_tasks.h:123) already had this line.
		delete[] tasks->tasks;
		tasks->tasks = nullptr;
		delete tasks;
	}
}

