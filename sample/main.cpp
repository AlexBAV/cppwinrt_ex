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

#pragma comment(lib, "windowsapp")

#define SPECIALIZE(type) \
namespace winrt::ABI::Windows::Foundation \
{ \
	template <> struct __declspec(uuid("3a14233f-a037-4ac0-a0ad-c4bb0bbf0111")) __declspec(novtable) AsyncActionProgressHandler<type> : impl_AsyncActionProgressHandler<type> {}; \
	template <> struct __declspec(uuid("3a14233f-a037-4ac0-a0ad-c4bb0bbf0112")) __declspec(novtable) AsyncActionWithProgressCompletedHandler<type> : impl_AsyncActionWithProgressCompletedHandler<type> {}; \
	template <> struct __declspec(uuid("3a14233f-a037-4ac0-a0ad-c4bb0bbf0115")) __declspec(novtable) AsyncOperationCompletedHandler<type> : impl_AsyncOperationCompletedHandler<type> {}; \
	template <> struct __declspec(uuid("3a14233f-a037-4ac0-a0ad-c4bb0bbf0116")) __declspec(novtable) IAsyncActionWithProgress<type> : impl_IAsyncActionWithProgress<type> {}; \
	template <> struct __declspec(uuid("3a14233f-a037-4ac0-a0ad-c4bb0bbf0117")) __declspec(novtable) IAsyncOperation<type> : impl_IAsyncOperation<type> {}; \
} \
// end of macro

SPECIALIZE(bool)
SPECIALIZE(int)

using namespace winrt;
using namespace Windows::Foundation;
using namespace std::chrono_literals;

winrt_ex::async_action void_timer(TimeSpan duration)
{
	co_await duration;
}

winrt_ex::async_operation<bool> bool_timer(TimeSpan duration)
{
	co_await duration;
	co_return true;
}

winrt_ex::async_operation<int> int_timer(TimeSpan duration)
{
	co_await duration;
	co_return 10;
}

winrt_ex::async_action test_when_all_void()
{
	// Test when_all with awaitables
	co_await winrt_ex::when_all(std::experimental::suspend_never{}, std::experimental::suspend_never{});

	// Test when_all with IAsyncAction
	co_await winrt_ex::when_all(void_timer(3s), void_timer(8s));
}

winrt_ex::async_action test_when_all_mixed()
{
	std::tuple<winrt_ex::no_result, bool> result = co_await winrt_ex::when_all(void_timer(2s), bool_timer(3s));
	std::tuple<bool, int, winrt_ex::no_result> result2 = co_await winrt_ex::when_all(bool_timer(2s), int_timer(3s), winrt::resume_after{ 4s });
}

winrt_ex::async_action test_when_all_bool()
{
	// Test when_all with IAsyncOperation<T>
	co_await winrt_ex::when_all(bool_timer(3s), bool_timer(8s));
}

winrt_ex::async_action test_when_any_void()
{
	// Test when_any with awaitables
	co_await winrt_ex::when_any(std::experimental::suspend_never{}, std::experimental::suspend_never{});

	// Test when_any with IAsyncAction
	co_await winrt_ex::when_any(void_timer(3s), void_timer(8s));
}

winrt_ex::async_action test_when_any_bool()
{
	// Test when_any with IAsyncOperation<T>
	co_await winrt_ex::when_any(bool_timer(3s), bool_timer(8s));
}

winrt_ex::async_action test_async_timer()
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

winrt_ex::async_action test_execute_with_timeout()
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
