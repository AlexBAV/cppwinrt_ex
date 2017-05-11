# Overview of `cppwinrt_ex` Library

This library is a small (and I hope growing) collection of utilities that complement the functionality of [`cppwinrt`](https://github.com/Microsoft/cppwinrt) library.

To start using this library, clone or download it and then `#include` the single header "cppwinrt_ex/core.h". Library has no external dependencies, except `cppwinrt` itself.

## Code Structure

This repository consists of the following subdirectories:

* **include**
  * Contains `core.h` header .
* **sample**
  * Contains an example project that illustrates the library usage.

## Compiler Support

The library has been tested on Microsoft Visual Studio 2017 Version 15.2 (26430.4).

## What's New

### Version 0.2

`async_action` and `async_operation<T>` classes have been removed. `future<T>`, a light-weight awaitable class is introduced instead. It is to be used in all coroutines that do not need to be resumed on the same thread. Coroutines that return future<T> may also be used starting with Windows Vista, which extends the range of supported OSes.

## Documentation

`cppwinrt_ex` library has all its classes and functions defined in `winrt_ex` namespace.

## TOC

* [`future<T>` Light-Weight Awaitable Class](#futuret-light-weight-awaitable-class)
* [`start` and `start_async` Functions](#start-and-start_async-functions)
* [`async_timer` Class](#async_timer-class)
* [`resumable_io_timeout` Class](#resumable_io_timeout-class)
* [`when_all` Function](#when_all-function)
* [`when_any` Function](#when_any-function)
* [`execute_with_timeout` Function](#execute_with_timeout-function)

### `future<T>` Light-Weight Awaitable Class

`cppwinrt` introduces a light-weight awaitable class `future<T>`. It may be used as a return type for any coroutine. `T` should be `void` for coroutines returning `void`. `T` cannot be a reference.

Continuation is not guaranteed to execute on the same thread.

`future<T>` provides a blocking `get()` method.

#### Notes

1. Coroutines in Windows Runtime application directly called from UI thread should use `IAsyncAction` and `IAsyncOperation<T>` because these types guarantee continuation to be executed on UI thread.
2. In the current version, when `await_resume` is called as part of execution of `co_await` expression, future's value is _moved_ to the caller.

### `start` and `start_async` Functions

`cppwinrt` provides a number of convenient utility classes to initiate asynchronous waits and I/O, among other things. The only problem with those classes is that the operation does not start until the caller begins _awaiting_ its result. Consider the following:

```C++
IAsyncAction coroutine1()
{
    // ...
    co_await 3s;
    // ...
}
```

In this code snippet, `co_await 3s;` is translated to `co_await resume_after{3s};`. `resume_after` is an _awaitable_ that starts a timer on thread pool and schedules a continuation. The problem is that you cannot *start* a timer and continue your work.

The same problem exists with `resumeable_io` class:

```C++
resumable_io io{handle};
/// ...
IAsyncAction coroutine2()
{
    co_await io.start([](OVERLAPPED &o)
    {
       check(::ReadFile(handle,...));
    });
    // ...
}
```

And again, you cannot start an I/O and do other stuff before you _await_ for operation result.

`winrt_ex` provides simple wrapper function that starts an asynchronous operation for you:

```C++
resumable_io io{handle};
/// ...
IAsyncAction coroutine3()
{
    auto running_io_operation = winrt_ex::start(io.start([](OVERLAPPED &o)
    {
       check(::ReadFile(handle,...));
    });
    // do other stuff
    // here we finally need to wait for operation to complete:
    auto result = co_await running_io_operation;
    // ...
}
```

`winrt_ex::start` supports awaitables that produce no result or awaitables that produce some result. Therefore, it behaves either as it has `IAsyncAction` return type or `IAsyncOperation<T>` return type.

Lirary also has `winrt_ex::start_async` version that has `future<T>` as its return type.

### `async_timer` Class

This is an awaitable cancellable timer. Its usage is very simple:

```C++
winrt_ex::async_timer timer;
// ...
IAsyncAction coroutine4()
{
    try
    {
        co_await timer.wait(20min);
    } catch(hresult_canceled)
    {
        // the wait has been cancelled
    }
}
// ...
void cancel_wait()
{
    timer.cancel();
}
```

**Note that current version runs the timer continuation inside the call to the `cancel` method. This might be changed in the future.**

### `resumable_io_timeout` Class

This is a version of `cppwinrt`'s `resumable_io` class that supports timeout for I/O operations. Its `start` method requires an additional parameter that specifies the I/O operation's timeout. If operation does not finish within a given time, it is cancelled and `hresult_canceled` exception is propagated to the continuation:

```C++
winrt_ex::resumable_io_timeout io{ handle_to_serial_port };
// ...
IAsyncAction coroutine5()
{
    try
    {
        auto bytes_received = co_await io.start([](OVERLAPPED &o)
        {
            check(::ReadFile(handle_to_serial_port, ... ));
        }, 10s);
        // Operation succeeded, continue processing
    } catch(hresult_canceled)
    {
        // operation timeout, data not ready
    } catch(hresult_error)
    {
        // other I/O errors
    }
}
```

### `when_all` Function

`when_all` function accepts any number of awaitables and produces an awaitable that is completed only when all input tasks are completed. If at least one of the tasks throws, the first thrown exception is rethrown by `when_all`.

Every input parameter must either be `IAsyncAction`, `IAsyncOperation<T>` or an awaitable that implements `await_resume` member function (or has a free function `await_resume`).

If all input tasks produce `void`, `when_all` also produces `void`, otherwise, it produces an `std::tuple<>` of all input parameter types. For `void` tasks, an empty type `winrt_ex::no_result` is used in the tuple.

```C++
winrt_ex::future<void> void_timer(TimeSpan duration)
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

IAsyncAction coroutine6()
{
    // The following operation will complete in 30 seconds and produce void
    co_await winrt_ex::when_all(void_timer(10s), void_timer(20s), void_timer(30s));

    // The following operation will complete in 30 seconds and produce std::tuple<bool, bool, int>
    std::tuple<bool, bool, int> result = co_await winrt_ex::when_all(bool_timer(10s), bool_timer(20s), int_timer(30s));

    // The following operation will complete in 30 seconds and produce std::tuple<bool, no_result, int>
    std::tuple<bool, no_result, int> result = co_await winrt_ex::when_all(bool_timer(10s), winrt::resume_after{20s}, int_timer(30s));
}
```

### `when_any` Function

`when_any` function accepts any number of awaitables and produces an awaitable that is completed when at least one of the input tasks is completed. If the first completed task throws, the thrown exception is rethrown by `when_any`.

All input parameters must be `IAsyncAction`, `IAsyncOperation<T>` or an awaitable type that implements `await_resume` member function (or has a free function `await_resume`) and **must all be of the same type**.

`when_any` **does not cancel any non-completed tasks.** When other tasks complete, their results are silently discarded. `when_any` makes sure the control block does not get destroyed until all tasks complete.

If all input tasks produce no result, `when_any` produces the index to the first completed task. Otherwise, it produces `std::pair<T, size_t>`, where the first result is the result of completed task and second is an index of completed task:

```C++
IAsyncAction coroutine7()
{
    // The following operation will complete in 10 seconds and produce 0
    size_t index = co_await winrt_ex::when_any(void_timer(10s), void_timer(20s), void_timer(30s));

    // The following operation will complete in 10 seconds and produce std::pair<bool, size_t> { true, 0 }
    std::pair<bool, size_t> result = co_await winrt_ex::when_any(bool_timer(10s), bool_timer(20s), bool_timer(30s));
}
```

### `execute_with_timeout` Function

This function takes an awaitable (and supports the same awaitable types as `when_all` function) and a time duration and returns an awaitable. When it is awaited, it either produces the result of the original awaitable or throws `hresult_canceled` exception if timeout elapses.

It does not cancel the input task if timeout elapses.

```C++
IAsyncAction coroutine8()
{
    try
    {
        co_await winrt_ex::execute_with_timeout(std::experimental::suspend_always{}, 3s);
    }
    catch (winrt::hresult_canceled)
    {
        // Timeout has elapsed
    }
}
```
