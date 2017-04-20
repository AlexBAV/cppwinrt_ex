#include <experimental/coroutine>
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
using V = std::tuple<bool, bool, bool>;
SPECIALIZE(V)
SPECIALIZE(int)

using namespace winrt;
using namespace Windows::Foundation;
using namespace std::chrono_literals;

HANDLE h1, h2;

IAsyncOperation<int> timer(TimeSpan duration)
{
	co_await duration;
	co_return 10;
}

IAsyncOperation<int> timer2(TimeSpan duration)
{
	co_await duration;
	SetEvent(h1);
	SetEvent(h2);
	co_return 10;
}

template<class... Awaitables>
IAsyncAction test(const Awaitables &...awaitables)
{
	co_await winrt_ex::when_any(awaitables...);
}

IAsyncOperation<bool> wait(HANDLE h)
{
	co_return co_await winrt::resume_on_signal{ h };
}

template<class... Awaitables>
IAsyncOperation<V> test2(const Awaitables &...awaitables)
{
	co_return co_await winrt_ex::when_all(awaitables...);
}

IAsyncAction void_timer(TimeSpan duration)
{
	co_await duration;
}

IAsyncOperation<bool> bool_timer(TimeSpan duration)
{
	co_await duration;
	co_return true;
}

IAsyncAction test_when_all_void()
{
	co_await winrt_ex::when_all(void_timer(3s), void_timer(8s));
}

IAsyncAction test_when_all_bool()
{
	co_await winrt_ex::when_all(bool_timer(3s), bool_timer(8s));
}

IAsyncAction test_when_any_void()
{
	co_await winrt_ex::when_any(void_timer(3s), void_timer(8s));
}

IAsyncAction test_when_any_bool()
{
	co_await winrt_ex::when_any(bool_timer(3s), bool_timer(8s));
}

int main()
{
	winrt::init_apartment();
	{
		test_when_all_void().get();
		test_when_all_bool().get();
		test_when_any_void().get();
		test_when_any_bool().get();

		Sleep(INFINITE);
	}
	winrt::uninit_apartment();
}
