//-------------------------------------------------------------------------------------------------------
// Copyright (C) 2016 HHD Software Ltd.
// Written by Alexander Bessonov
//
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------

#include <chrono>
#include <iostream>

#include <experimental/resumable>
#include <cppwinrt_ex/core.h>

#include <future>

#pragma comment(lib, "windowsapp")

using namespace winrt;
using namespace Windows::Foundation;
using namespace std::chrono_literals;

winrt_ex::future<void> void_timer(TimeSpan duration)
{
	co_await duration;
}

winrt_ex::future<bool> bool_timer(TimeSpan duration)
{
	co_await duration;
	co_return true;
}

winrt_ex::future<int> int_timer(TimeSpan duration)
{
	co_await duration;
	co_return 10;
}

winrt_ex::future<void> test_when_all_void()
{
	// Test when_all with awaitables
	co_await winrt_ex::when_all(std::experimental::suspend_never{}, std::experimental::suspend_never{});

	// Test when_all with IAsyncAction
	co_await winrt_ex::when_all(void_timer(3s), void_timer(8s));
}

winrt_ex::future<void> test_when_all_mixed()
{
	std::tuple<winrt_ex::no_result, bool> result = co_await winrt_ex::when_all(void_timer(2s), bool_timer(3s));
	std::tuple<bool, int, winrt_ex::no_result> result2 = co_await winrt_ex::when_all(bool_timer(2s), int_timer(3s), winrt::resume_after{ 4s });
}

winrt_ex::future<void> test_when_all_bool()
{
	// Test when_all with IAsyncOperation<T>
	std::promise<bool> promise;
	promise.set_value(true);
	co_await winrt_ex::when_all(bool_timer(3s), bool_timer(8s), promise.get_future());
}

winrt_ex::future<void> test_when_any_void()
{
	// Test when_any with awaitables
	co_await winrt_ex::when_any(std::experimental::suspend_never{}, std::experimental::suspend_never{});

	auto timer1 = void_timer(3s);
	// Test when_any with IAsyncAction
	co_await winrt_ex::when_any(timer1, void_timer(8s));
}

winrt_ex::future<void> test_when_any_bool()
{
	// Test when_any with IAsyncOperation<T>
	co_await winrt_ex::when_any(bool_timer(3s), bool_timer(8s));
}

winrt_ex::future<void> test_async_timer()
{
	// Test cancellable async_timer. Start a timer for 20 minutes and cancel it after 2 seconds
	winrt_ex::async_timer atimer;

	auto timer_task = winrt_ex::start_async(atimer.wait(20min));
	co_await 2s;
	atimer.cancel();
	try
	{
		co_await timer_task;
	}
	catch (winrt::hresult_canceled)
	{
		std::wcout << L"Timer cancelled. ";
	}
}

winrt_ex::future<void> test_execute_with_timeout()
{
	// Test execute_with_timeout
	try
	{
		co_await winrt_ex::execute_with_timeout(std::experimental::suspend_always{}, 3s);
	}
	catch (winrt::hresult_canceled)
	{
		std::wcout << L"Operation cancelled. ";
	}
	try
	{
		co_await winrt_ex::execute_with_timeout(bool_timer(20s), 3s);
	}
	catch (winrt::hresult_canceled)
	{
		std::wcout << L"Operation cancelled. ";
	}
}

template<class F>
void measure(const wchar_t *name, const F &f)
{
	std::wcout << L"Starting operation " << name << L" ... ";
	auto start = std::chrono::high_resolution_clock::now();
	f();
	auto stop = std::chrono::high_resolution_clock::now();

	auto seconds = std::chrono::duration_cast<std::chrono::duration<double>>(stop - start);
	std::wcout << seconds.count() << L" seconds\r\n";
}

int main()
{
	winrt::init_apartment();
	{
		measure(L"test_execute_with_timeout", [] { test_execute_with_timeout().get(); });
		measure(L"test_async_timer", [] { test_async_timer().get(); });
		measure(L"test_when_all_void", [] { test_when_all_void().get(); });
		measure(L"test_when_all_bool", [] { test_when_all_bool().get(); });
		measure(L"test_when_all_mixed", [] { test_when_all_mixed().get(); });
		measure(L"test_when_any_void", [] { test_when_any_void().get(); });
		measure(L"test_when_any_bool", [] {test_when_any_bool().get(); });

		Sleep(5000);
	}
	winrt::uninit_apartment();
}
