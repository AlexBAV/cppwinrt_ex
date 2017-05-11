//-------------------------------------------------------------------------------------------------------
// Copyright (C) 2016 HHD Software Ltd.
// Written by Alexander Bessonov
//
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
// Version 0.2
// async_action & async_operation<T> classes removed
// light-weight future<T> template added

#pragma once

#include <atomic>
#include <type_traits>
#include <tuple>
#include <utility>
#include <exception>
#include <memory>
#include <array>
#include <mutex>
#include <experimental/resumable>

#include <winrt/base.h>

namespace winrt_ex
{
	namespace details
	{
		// future
		// Windows SRW lock wrapped in shared_mutex-friendly class
		class srwlock
		{
			SRWLOCK m_lock{};
		public:
			srwlock(const srwlock &) = delete;
			srwlock & operator=(const srwlock &) = delete;
			srwlock() noexcept = default;

			void lock() noexcept
			{
				AcquireSRWLockExclusive(&m_lock);
			}

			void lock_shared() noexcept
			{
				AcquireSRWLockShared(&m_lock);
			}

			bool try_lock() noexcept
			{
				return 0 != TryAcquireSRWLockExclusive(&m_lock);
			}

			void unlock() noexcept
			{
				ReleaseSRWLockExclusive(&m_lock);
			}

			void unlock_shared() noexcept
			{
				ReleaseSRWLockShared(&m_lock);
			}
		};


		enum class status_t
		{
			running,
			ready,
			exception
		};

		struct promise_base0
		{
			srwlock lock;
			std::experimental::coroutine_handle<> resume{};
			std::exception_ptr exception;
			std::atomic<int> use_count{ 1 };

			status_t status{ status_t::running };

			bool is_ready() const noexcept
			{
				return status != status_t::running;
			}

			void check_resume(std::unique_lock<srwlock> &&l)
			{
				if (resume)
				{
					l.unlock();
					resume();
				}
			}

			void set_exception(std::exception_ptr exception_)
			{
				std::unique_lock<srwlock> l(lock);
				exception = exception_;
				status = status_t::exception;
				check_resume(std::move(l));
			}

			void check_exception()
			{
				assert(is_ready());
//				std::unique_lock<srwlock> l(lock);
				if (status == status_t::exception && exception)
					std::rethrow_exception(exception);
			}
		};

		template<class T>
		struct promise_base : promise_base0
		{
			T value;

			template<class V>
			void return_value(V &&v)
			{
				std::unique_lock<srwlock> l(lock);
				value = std::forward<V>(v);
				status = status_t::ready;
				check_resume(std::move(l));
			}

			T &get()
			{
				check_exception();
				return value;
			}
		};

		template<>
		struct promise_base<void> : promise_base0
		{
			void return_void()
			{
				std::unique_lock<srwlock> l(lock);
				status = status_t::ready;
				check_resume(std::move(l));
			}

			struct empty_type {};

			empty_type get()
			{
				check_exception();
				return {};
			}
		};

		template<class T>
		class future_base
		{
		protected:
			const T &iget(T &value) const &
			{
				return value;
			}

			T &&iget(T &value) &&
			{
				return std::move(value);
			}
		};

		template<>
		class future_base<void>
		{
		protected:
			template<class T>
			void iget(T &)
			{
			}
		};

		template<class T>
		class future : public future_base<T>
		{
			static_assert(!std::is_reference_v<T>, "future<T> is not allowed for reference types");
			struct promise_type_ : promise_base<T>
			{
				srwlock lock;
				std::experimental::coroutine_handle<> destroy_resume{};
				bool future_exists{ true };

				//
				bool start_async(std::experimental::coroutine_handle<> resume_)
				{
					const std::lock_guard<srwlock> l(lock);
					if (is_ready())
						return false;	// we already have a result
					resume = resume_;
					return true;
				}

				static std::experimental::suspend_never initial_suspend() noexcept
				{
					return {};
				}

				auto final_suspend() noexcept
				{
					struct awaiter
					{
						promise_type_ *pthis;

						bool await_ready() const noexcept
						{
							pthis->lock.lock();
							auto is_ready = !pthis->future_exists;
							if (is_ready)
								pthis->lock.unlock();
							return is_ready;
						}

						void await_suspend(std::experimental::coroutine_handle<> resume) noexcept
						{
							pthis->destroy_resume = resume;
							pthis->lock.unlock();
						}

						static void await_resume() noexcept
						{
						}
					};
					return awaiter{ this };
				}

				future<T> get_return_object() noexcept
				{
					return { this };
				}

				void add_ref()
				{
					use_count.fetch_add(1, std::memory_order_relaxed);
				}

				void release()
				{
					if (1 == use_count.fetch_sub(1, std::memory_order_relaxed))
						destroy();
				}

				void destroy()
				{
					std::unique_lock<srwlock> l(lock);
					future_exists = false;
					if (destroy_resume)
					{
						l.unlock();
						destroy_resume.destroy();
					}
				}
			};

			promise_type_ *promise;

			future(promise_type_ *promise) noexcept :
				promise{ promise }
			{}

			struct special_await
			{
				promise_type_ *promise;

				bool await_ready() const
				{
					return promise->is_ready();
				}

				bool await_suspend(std::experimental::coroutine_handle<> resume)
				{
					return promise->start_async(resume);
				}

				void await_resume()
				{
				}
			};

		public:
			using promise_type = promise_type_;

			~future()
			{
				if (promise)
					promise->release();
			}

			future(const future &o) noexcept :
			promise{ o.promise }
			{
				if (promise)
					promise->add_ref();
			}

			future &operator =(const future &o) noexcept
			{
				if (promise)
					promise->release();
				promise = o.promise;
				if (promise)
					promise->add_ref();
			}

			future(future &&o) noexcept :
				promise{ o.promise }
			{
				o.promise = nullptr;
			}

			future &operator =(future &&o) noexcept
			{
				using std::swap;
				swap(promise, o.promise);
				return *this;
			}

			void wait()
			{
				if (promise->status != status_t::running)
					return;

				winrt::impl::lock x;
				winrt::impl::condition_variable cv;
				bool completed = false;

				[&]()->winrt::fire_and_forget
				{
					co_await special_await{ promise };
					const winrt::impl::lock_guard guard(x);
					completed = true;
					cv.wake_one();
				}();

				const winrt::impl::lock_guard guard(x);
				cv.wait_while(x, [&] { return !completed; });
			}

			decltype(auto) get()
			{
				wait();
				return iget(promise->get());
			}

			// await
			bool await_ready() const
			{
				return promise->is_ready();
			}

			bool await_suspend(std::experimental::coroutine_handle<> resume)
			{
				return promise->start_async(resume);
			}

			void iawait_resume(std::true_type)
			{
				promise->get();
			}

			decltype(auto) iawait_resume(std::false_type)
			{
				return std::move(promise->get());
			}

			decltype(auto) await_resume()
			{
				return iawait_resume(std::is_same<T, void>{});
			}
		};
		
		// no_result will substitute 'void' in tuple
		struct no_result {};

		// get_result_type 
		// get the coroutine result type
		// supports IAsyncAction, IAsyncOperation<T> and awaitable object that implements await_resume
		// yields result_type<T> where T is a coroutine result type
		template<class T>
		struct result_type
		{
			using type = T;
		};

		constexpr result_type<void> get_result_type(const ::winrt::Windows::Foundation::IAsyncAction &)
		{
			return {};
		}

		template<class T>
		constexpr result_type<T> get_result_type(const ::winrt::Windows::Foundation::IAsyncOperation<T> &)
		{
			return {};
		}

		// primary template handles types that do not implement await_resume:
		template<class, class = std::void_t<>>
		struct has_await_resume : std::false_type {};

		// specialization recognizes types that do implement await_resume
		template<class T>
		struct has_await_resume<T, std::void_t<decltype(std::declval<T&>().await_resume())>> : std::true_type {};

		template<class T>
		constexpr bool has_await_resume_v = has_await_resume<T>::value;

		// "external" cae
		template<class, class = std::void_t<>>
		struct has_external_await_resume : std::false_type {};

		template<class T>
		struct has_external_await_resume<T, std::void_t<decltype(await_resume(std::declval<T &>()))>> : std::true_type {};

		template<class T>
		constexpr bool has_external_await_resume_v = has_external_await_resume<T>::value;

		//
		template<class T>
		constexpr result_type<decltype(std::declval<T &>().await_resume())> get_result_type(const T &, std::enable_if_t<has_await_resume_v<T>, void *> = nullptr)
		{
			return {};
		}

		template<class T>
		constexpr result_type<decltype(await_resume(std::declval<T &>()))> get_result_type(const T &, std::enable_if_t<has_external_await_resume_v<T>, void *> = nullptr)
		{
			return {};
		}

		// Helper to get a coroutine result type for a first item in variadic sequence
		template<class First, class...Rest>
		constexpr auto get_first_result_type(const First &first, const Rest &...)
		{
			return get_result_type(first);
		}

		// Get a first type in a variadic type list
		template<class...T>
		struct get_first;

		template<class F,class...R>
		struct get_first<F, R...>
		{
			using type = F;
		};

		template<class...T>
		using get_first_t = typename get_first<T...>::type;

		// when_all
		template<size_t Index, class Master, class Awaitable>
		inline auto when_all_helper_single(Master &master, Awaitable task) noexcept ->
			std::enable_if_t<
				std::is_same_v<
					result_type<void>,
					decltype(get_result_type(task))
				>,
				winrt::fire_and_forget
			>
		{
			try
			{
				co_await task;
				master.finished<Index>();
			}
			catch (...)
			{
				master.finished_exception();
			}
		}

		template<size_t Index, class Master, class Awaitable>
		inline auto when_all_helper_single(Master &master, Awaitable task) noexcept ->
			std::enable_if_t<
				!std::is_same_v<
					result_type<void>,
					decltype(get_result_type(task))
				>,
				winrt::fire_and_forget
			>
		{
			try
			{
				master.finished<Index>(co_await task);
			}
			catch (...)
			{
				master.finished_exception();
			};
		}

		template<class Master, class Tuple, size_t...I>
		inline void when_all_helper(Master &master, Tuple &&tuple, std::index_sequence<I...>) noexcept
		{
			[[maybe_unused]] auto x = { when_all_helper_single<I>(master, std::get<I>(std::move(tuple)))... };
		}

		template<class...Awaitables>
		struct when_all_awaitable_base
		{
			std::exception_ptr exception;
			std::atomic<int> counter;
			std::experimental::coroutine_handle<> resume;
			std::tuple<std::decay_t<Awaitables>...> awaitables;

			when_all_awaitable_base(Awaitables &&...awaitables) noexcept :
				awaitables{ std::forward<Awaitables>(awaitables)... },
				counter{ sizeof...(awaitables) }
			{}

			when_all_awaitable_base(when_all_awaitable_base &&o) noexcept :
				exception{ std::move(o.exception) },
				counter{ o.counter.load(std::memory_order_relaxed) },	// it is safe to "move" atomic this way because we don't "use" it until the final instance is allocated
				resume{ o.resume },
				awaitables{ std::move(o.awaitables) }
			{}

			void finished_exception() noexcept
			{
				if (!exception)
					exception = std::current_exception();
				check_resume();
			}

			void check_resume() noexcept
			{
				if (0 == counter.fetch_sub(1, std::memory_order_relaxed) - 1)
					resume();
			}

			bool await_ready() const noexcept
			{
				return false;
			}
		};

		// void case
		template<class...Awaitables>
		struct when_all_awaitable_void : when_all_awaitable_base<Awaitables...>
		{
			when_all_awaitable_void(Awaitables &&...awaitables) noexcept :
				when_all_awaitable_base{ std::forward<Awaitables>(awaitables)... }
			{}

			when_all_awaitable_void(when_all_awaitable_void &&o) noexcept :
				when_all_awaitable_base{ static_cast<when_all_awaitable_base &&>(o) }
			{}

			template<size_t, class T>
			void finished(const T &) noexcept
			{
				check_resume();
			}

			template<size_t>
			void finished() noexcept
			{
				check_resume();
			}

			void await_suspend(std::experimental::coroutine_handle<> handle) noexcept
			{
				resume = handle;
				using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
				when_all_helper(*this, std::move(awaitables), index_t{});
			}

			void await_resume() const
			{
				if (exception)
					std::rethrow_exception(exception);
			}
		};

		template<class...Awaitables>
		inline auto when_all_impl(std::true_type, Awaitables &&...awaitables)
		{
			return when_all_awaitable_void<Awaitables...> { std::forward<Awaitables>(awaitables)... };
		}

		// non-void case
		template<class...Awaitables>
		struct when_all_awaitable_value : when_all_awaitable_base<Awaitables...>
		{
			template<class T>
			struct transform
			{
				using type = std::conditional_t<std::is_same_v<result_type<void>, T>, no_result, std::decay_t<typename T::type>>;
			};

			template<class T>
			using transform_t = typename transform<T>::type;

			template<class...Ts>
			struct get_results_type
			{
				using type = std::tuple<transform_t<Ts>...>;
			};

			using results_t = typename get_results_type<decltype(get_result_type(std::declval<Awaitables>()))...>::type;
			results_t results;

			when_all_awaitable_value(Awaitables &&...awaitables) noexcept :
				when_all_awaitable_base{ std::forward<Awaitables>(awaitables)... }
			{}

			when_all_awaitable_value(when_all_awaitable_value &&o) noexcept :
				when_all_awaitables_base{ static_cast<when_all_awaitable_base &&>(o) }
			{}

			template<size_t index, class T>
			void finished(T &&result) noexcept
			{
				std::get<index>(results) = std::forward<T>(result);
				check_resume();
			}

			template<size_t>
			void finished() noexcept
			{
				check_resume();
			}

			void await_suspend(std::experimental::coroutine_handle<> handle) noexcept
			{
				resume = handle;
				using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
				when_all_helper(*this, awaitables, index_t{});
			}

			results_t await_resume() const
			{
				if (exception)
					std::rethrow_exception(exception);
				else
					return results;
			}
		};

		template<class...Awaitables>
		inline auto when_all_impl(std::false_type, Awaitables &&...awaitables)
		{
			return when_all_awaitable_value<Awaitables...> { std::forward<Awaitables>(awaitables)... };
		}

		template<class...Awaitables>
		inline auto when_all(Awaitables &&...awaitables)
		{
			static_assert(sizeof...(Awaitables) >= 2, "when_all must be passed at least two arguments");
			using first_type = decltype(get_first_result_type(awaitables...));

			return when_all_impl(
				std::conjunction<
					std::is_same<result_type<void>, first_type>,
					are_all_same_t<decltype(get_result_type(awaitables))...>
				>{}, std::forward<Awaitables>(awaitables)...);
		}

		///////////////////////////////////

		// when_any
		struct when_any_block_base
		{
			std::exception_ptr exception;
			std::atomic<std::experimental::coroutine_handle<>> resume{};

			void finished_exception() noexcept
			{
				if (resume.load(std::memory_order_relaxed))
				{
					auto value = resume.exchange(nullptr, std::memory_order_relaxed);
					if (value)
					{
						exception = std::current_exception();
						value();
					}
				}
			}
		};

		// void case
		struct when_any_block_void : when_any_block_base
		{
			size_t index;

			void finished(size_t index_) noexcept
			{
				if (resume.load(std::memory_order_relaxed))
				{
					auto value = resume.exchange(nullptr, std::memory_order_relaxed);
					if (value)
					{
						index = index_;
						value();
					}
				}
			}
		};

		template<class T>
		struct when_any_block_value : when_any_block_base
		{
			T result;
			size_t index;

			void finished(T &&result_, size_t index_) noexcept
			{
				if (resume.load(std::memory_order_relaxed))
				{
					auto value = resume.exchange(nullptr, std::memory_order_relaxed);
					if (value)
					{
						result = std::move(result_);
						index = index_;
						value();
					}
				}
			}
		};

		template<size_t Index, class Awaitable>
		inline winrt::fire_and_forget when_any_helper_single(std::shared_ptr<when_any_block_void> master, Awaitable task) noexcept
		{
			try
			{
				co_await task;
				master->finished(Index);
			}
			catch (...)
			{
				master->finished_exception();
			}
		}

		template<size_t N, class Tuple, size_t...I>
		inline void when_any_helper(std::array<std::shared_ptr<when_any_block_void>, N> &&master, Tuple &&tuple, std::index_sequence<I...>) noexcept
		{
			[[maybe_unused]] auto x = { when_any_helper_single<I>(std::get<I>(std::move(master)), std::get<I>(std::move(tuple)))... };
		}

		template<class...Awaitables>
		inline auto when_any_impl(result_type<void>, Awaitables &&...awaitables)
		{
			struct when_any_awaitable
			{
				std::shared_ptr<when_any_block_void> ptr;
				std::tuple<std::decay_t<Awaitables>...> awaitables;

				when_any_awaitable(Awaitables &&...awaitables) noexcept :
					awaitables{ std::forward<Awaitables>(awaitables)... },
					ptr{ std::make_shared<when_any_block_void>() }
				{
				}

				bool await_ready() const noexcept
				{
					return false;
				}

				void await_suspend(std::experimental::coroutine_handle<> handle)
				{
					ptr->resume.store(handle, std::memory_order_relaxed);
					std::array<std::shared_ptr<when_any_block_void>, sizeof...(Awaitables)> references;
					std::fill(references.begin(), references.end(), ptr);

					auto awaitables_local_copy = std::move(awaitables);

					using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
					when_any_helper(std::move(references), std::move(awaitables_local_copy), index_t{});
				}

				size_t await_resume() const
				{
					if (ptr->exception)
						std::rethrow_exception(ptr->exception);
					else
						return ptr->index;
				}
			};

			return when_any_awaitable{ std::forward<Awaitables>(awaitables)... };
		}

		//non-void case
		template<class T, class Awaitable>
		inline winrt::fire_and_forget when_any_helper_single_value(std::shared_ptr<when_any_block_value<T>> master, Awaitable task, size_t index) noexcept
		{
			try
			{
				master->finished(co_await task, index);
			}
			catch(...)
			{
				master->finished_exception();
			}
		}

		template<class T, size_t N, class Tuple, size_t...I>
		inline void when_any_helper_value(std::array<std::shared_ptr<when_any_block_value<T>>, N> &&master, Tuple &&tuple, std::index_sequence<I...>) noexcept
		{
			[[maybe_unused]] auto x = { when_any_helper_single_value<T>(std::get<I>(std::move(master)), std::get<I>(std::move(tuple)), I)... };
		}


		template<class T, class...Awaitables>
		inline auto when_any_impl(result_type<T>, Awaitables &&...awaitables)
		{
			using value_type = std::decay_t<T>;
			struct when_any_awaitable
			{
				std::shared_ptr<when_any_block_value<value_type>> ptr;
				std::tuple<std::decay_t<Awaitables>...> awaitables;

				when_any_awaitable(Awaitables &&...awaitables) noexcept :
					awaitables{ std::forward<Awaitables>(awaitables)... },
					ptr{ std::make_shared<when_any_block_value<value_type>>() }
				{}

				bool await_ready() const noexcept
				{
					return false;
				}

				void await_suspend(std::experimental::coroutine_handle<> handle)
				{
					ptr->resume.store(handle, std::memory_order_relaxed);
					std::array<std::shared_ptr<when_any_block_value<value_type>>, sizeof...(Awaitables)> references;
					std::fill(references.begin(), references.end(), ptr);

					auto awaitables_copy = std::move(awaitables);
					using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
					when_any_helper_value(std::move(references), std::move(awaitables_copy), index_t{});
				}

				std::pair<T, size_t> await_resume() const
				{
					if (ptr->exception)
						std::rethrow_exception(ptr->exception);
					else
						return { std::move(ptr->result), ptr->index };
				}
			};

			return when_any_awaitable{ std::forward<Awaitables>(awaitables)... };
		}

		template<class...Ts>
		struct are_all_same
		{
			using first_type = get_first_t<Ts...>;

			using type = std::conjunction<std::is_same<first_type, Ts>...>;
		};

		template<class...Ts>
		using are_all_same_t = typename are_all_same<Ts...>::type;

		template<class...Ts>
		constexpr bool are_all_same_v = typename are_all_same<Ts...>::type::value;

		template<class...Awaitables>
		inline auto when_any(Awaitables &&...awaitables)
		{
			static_assert(sizeof...(Awaitables) >= 2, "when_any must be passed at least two arguments");
			static_assert(are_all_same_v<decltype(get_result_type(awaitables))...>, "when_any requires all awaitables to produce the same type");

			return when_any_impl(get_first_result_type(awaitables...), std::forward<Awaitables>(awaitables)...);
		}

		//////////////////////////////
		// Simplified versions of IAsyncAction and IAsyncOperation that do not force return to original thread context
		template <typename Async>
		struct await_adapter
		{
			const Async & async;

			bool await_ready() const
			{
				return async.Status() == winrt::Windows::Foundation::AsyncStatus::Completed;
			}

			void await_suspend(std::experimental::coroutine_handle<> handle) const
			{
				async.Completed([handle](const auto &, winrt::Windows::Foundation::AsyncStatus)
				{
					handle();
				});
			}

			auto await_resume() const
			{
				return async.GetResults();
			}
		};

		// start & start_async
		// methods "starts" an asynchronous operation that only starts in await_suspend
		struct default_policy
		{
			template<class T>
			struct promise
			{
				template<class Awaitable>
				static ::winrt::Windows::Foundation::IAsyncOperation<T> start(Awaitable awaitable)
				{
					auto result = co_await awaitable;
					co_return result;
				}

				static ::winrt::Windows::Foundation::IAsyncOperation<T> wait(winrt::Windows::Foundation::TimeSpan timeout)
				{
					co_await ::winrt::resume_after{ timeout };
					throw ::winrt::hresult_canceled{};
				}
			};

			template<>
			struct promise<void>
			{
				template<class Awaitable>
				static ::winrt::Windows::Foundation::IAsyncAction start(Awaitable awaitable)
				{
					co_await awaitable;
				}

				static ::winrt::Windows::Foundation::IAsyncAction wait(::winrt::Windows::Foundation::TimeSpan timeout)
				{
					co_await ::winrt::resume_after{ timeout };
					throw ::winrt::hresult_canceled{};
				}
			};
		};

		struct ex_policy
		{
			template<class T>
			struct promise
			{
				template<class Awaitable>
				static future<T> start(Awaitable awaitable)
				{
					co_return co_await awaitable;
				}
			};

			template<>
			struct promise<void>
			{
				template<class Awaitable>
				static future<void> start(Awaitable awaitable)
				{
					co_await awaitable;
				}
			};
		};

		template<class Policy,class Awaitable>
		inline auto start(Awaitable &&awaitable)
		{
			using promise_wrapper_t = typename Policy::promise<decltype(std::declval<Awaitable &>().await_resume())>;
			return promise_wrapper_t::start(std::forward<Awaitable>(awaitable));
		}

		template<class Awaitable>
		inline auto start(const Awaitable &awaitable)
		{
			return start<default_policy>(awaitable);
		}

		template<class Awaitable>
		inline auto start_async(const Awaitable &awaitable)
		{
			return start<ex_policy>(awaitable);
		}

		// Cancellable timer
		class async_timer
		{
			struct timer_traits : winrt::impl::handle_traits<PTP_TIMER>
			{
				static void close(type value) noexcept
				{
					CloseThreadpoolTimer(value);
				}
			};

			winrt::impl::handle<timer_traits> timer
			{ 
				CreateThreadpoolTimer([](PTP_CALLBACK_INSTANCE, void * context, PTP_TIMER) noexcept
				{
					static_cast<async_timer *>(context)->resume();
				}, this, nullptr) 
			};

			std::atomic_flag resumed{ false };
			std::atomic<bool> cancelled{ false };
			std::experimental::coroutine_handle<> resume_location{ nullptr };

			//
			auto get() const noexcept
			{
				return winrt::get_abi(timer);
			}

			bool is_cancelled() const noexcept
			{
				return cancelled.load(std::memory_order_acquire);
			}

			void resume() noexcept
			{
				if (resume_location && !resumed.test_and_set())
					resume_location();
			}

			void set_handle(std::experimental::coroutine_handle<> handle)
			{
				resume_location = handle;
			}

		public:
			auto wait(winrt::Windows::Foundation::TimeSpan duration) noexcept
			{
				class awaiter
				{
					async_timer *timer;
					winrt::Windows::Foundation::TimeSpan duration;

				public:
					awaiter(async_timer *timer, winrt::Windows::Foundation::TimeSpan duration) noexcept :
						timer{ timer },
						duration{ duration }
					{}

					bool await_ready() const noexcept
					{
						return duration.count() <= 0;
					}

					void await_suspend(std::experimental::coroutine_handle<> handle) noexcept
					{
						timer->set_handle(handle);
						int64_t relative_count = -duration.count();
						SetThreadpoolTimer(timer->get(), reinterpret_cast<PFILETIME>(&relative_count), 0, 0);
					}

					void await_resume() const
					{
						timer->set_handle(nullptr);
						if (timer->is_cancelled())
							throw winrt::hresult_canceled();
					}
				};

				resumed.clear();
				return awaiter{ this,duration };
			}

			void cancel()
			{
				cancelled.store(true, std::memory_order_release);
				SetThreadpoolTimer(get(), nullptr, 0, 0);
				WaitForThreadpoolTimerCallbacks(get(), TRUE);
				resume();
			}
		};

		// resumeable I/O with timeout
		template<class D>
		class supports_timeout
		{
			struct timer_traits : winrt::impl::handle_traits<PTP_TIMER>
			{
				static void close(type value) noexcept
				{
					CloseThreadpoolTimer(value);
				}
			};

			winrt::impl::handle<timer_traits> m_timer
			{ 
				CreateThreadpoolTimer([](PTP_CALLBACK_INSTANCE, void * context, PTP_TIMER) noexcept
				{
					static_cast<D *>(context)->on_timeout();
				}, static_cast<D *>(this), nullptr)
			};
			winrt::Windows::Foundation::TimeSpan timeout;

		protected:
			using supports_timeout_base = supports_timeout;

			supports_timeout(winrt::Windows::Foundation::TimeSpan timeout) :
				timeout{ timeout }
			{}

			void set_timer() const noexcept
			{
				if (timeout.count())
				{
					int64_t relative_count = -timeout.count();
					SetThreadpoolTimer(winrt::get_abi(m_timer), reinterpret_cast<PFILETIME>(&relative_count), 0, 0);
				}
			}

			void reset_timer() const noexcept
			{
				if (timeout.count())
				{
					SetThreadpoolTimer(winrt::get_abi(m_timer), nullptr, 0, 0);
					WaitForThreadpoolTimerCallbacks(winrt::get_abi(m_timer), TRUE);
				}
			}
		};

		class resumable_io_timeout
		{
			struct io_traits : winrt::impl::handle_traits<PTP_IO>
			{
				static void close(type value) noexcept
				{
					CloseThreadpoolIo(value);
				}
			};

			class my_awaitable_base : public OVERLAPPED
			{
			protected:
				uint32_t m_result{};
				std::experimental::coroutine_handle<> m_resume{ nullptr };
				virtual void resume() = 0;

				my_awaitable_base() : OVERLAPPED{}
				{}

			public:
				static void __stdcall callback(PTP_CALLBACK_INSTANCE, void *, void * overlapped, ULONG result, ULONG_PTR, PTP_IO) noexcept
				{
					auto context = static_cast<my_awaitable_base *>(static_cast<OVERLAPPED *>(overlapped));
					context->m_result = result;
					context->resume();
				}
			};

			template<class F>
			class awaitable : protected my_awaitable_base, protected F, protected supports_timeout<awaitable<F>>
			{
				PTP_IO m_io{ nullptr };
				HANDLE object;

				virtual void resume() override
				{
					reset_timer();
					m_resume();
				}

			public:
				awaitable(PTP_IO io, HANDLE object, F &&callback, winrt::Windows::Foundation::TimeSpan timeout) noexcept :
					m_io{ io },
					object{ object },
					F{ std::forward<F>(callback) },
					supports_timeout_base{ timeout }
				{}

				bool await_ready() const noexcept
				{
					return false;
				}

				auto await_suspend(std::experimental::coroutine_handle<> resume_handle)
				{
					m_resume = resume_handle;
					StartThreadpoolIo(m_io);

					try
					{
						return call(std::is_same<void, decltype((*this)(std::declval<OVERLAPPED &>()))>{});
					}
					catch (...)
					{
						CancelThreadpoolIo(m_io);
						throw;
					}
				}

				void call(std::true_type)
				{
					(*this)(*this);
					set_timer();
				}

				bool call(std::false_type)
				{
					if ((*this)(*this))
					{
						set_timer();
						return true;
					}
					else
					{
						CancelThreadpoolIo(m_io);
						return false;
					}
				}

				uint32_t await_resume()
				{
					if (m_result != NO_ERROR && m_result != ERROR_HANDLE_EOF)
					{
						if (m_result == ERROR_OPERATION_ABORTED)
							m_result = ERROR_TIMEOUT;
						throw hresult_error(HRESULT_FROM_WIN32(m_result));
					}

					return static_cast<uint32_t>(InternalHigh);
				}

				void on_timeout()
				{
					// cancel io
					CancelIoEx(object, this);
				}
			};

			winrt::impl::handle<io_traits> m_io;
			HANDLE object;

			//
		public:
			resumable_io_timeout(HANDLE object) :
				object{ object },
				m_io{ CreateThreadpoolIo(object, my_awaitable_base::callback, nullptr, nullptr) }
			{
				if (!m_io)
				{
					winrt::throw_last_error();
				}
			}

			template <typename F>
			auto start(F &&callback, winrt::Windows::Foundation::TimeSpan timeout)
			{
				return awaitable<F>{get(), object, std::forward<F>(callback), timeout};
			}

			PTP_IO get() const noexcept
			{
				return winrt::get_abi(m_io);
			}
		};
	}

	// Bring public stuff to winrt_ex namespace
	using details::future;
	using details::no_result;
	using details::default_policy;
	using details::ex_policy;
	using details::async_timer;
	using details::resumable_io_timeout;

	using details::start;
	using details::start_async;
	using details::when_all;
	using details::when_any;
}

namespace winrt_ex
{
	namespace details
	{
		// execute_with_timeout
		inline future<void> throwing_timer(result_type<void>, winrt::Windows::Foundation::TimeSpan timeout)
		{
			co_await winrt::resume_after{ timeout };
			throw winrt::hresult_canceled{};
		}

#pragma warning(push)
#pragma warning(disable:4033)
		template<class T>
		inline future<std::decay_t<T>> throwing_timer(result_type<T>, winrt::Windows::Foundation::TimeSpan timeout)
		{
			co_await winrt::resume_after{ timeout };
			throw winrt::hresult_canceled{};
			co_return std::decay_t<T> {};	// this line is unnecessary, but prevents ICE (!!!)
		}
#pragma warning(pop)

		template<class Awaitable>
		inline auto execute_with_timeout(const Awaitable &awaitable, winrt::Windows::Foundation::TimeSpan timeout)
		{
			return when_any(awaitable, throwing_timer(get_result_type(awaitable), timeout));
		}
	}

	using details::execute_with_timeout;
}

namespace winrt_ex
{
	namespace details
	{

	}

	using details::future;
}