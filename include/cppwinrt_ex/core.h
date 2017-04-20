#include <array>
#include <atomic>
#include <winrt/base.h>

namespace winrt_ex
{
	namespace details
	{
		// when_all
		template<class Awaitable>
		struct awaitable_type;

		template<>
		struct awaitable_type<winrt::Windows::Foundation::IAsyncAction>
		{
			using type = void;
		};

		template<class T>
		struct awaitable_type<winrt::Windows::Foundation::IAsyncOperation<T>>
		{
			using type = T;
		};

		template<class T>
		using awaitable_type_t = typename awaitable_type<std::decay_t<T>>::type;

		template<class...T>
		struct get_first;

		template<class F,class...R>
		struct get_first<F, R...>
		{
			using type = F;
		};

		template<class...T>
		using get_first_t = typename get_first<T...>::type;

		template<class Master, class Awaitable>
		inline winrt::fire_and_forget when_all_helper_single(Master &master, Awaitable task)
		{
			try
			{
				co_await task;
				master.finished();
			}
			catch (...)
			{
				master.finished_exception();
			}
		}

		template<size_t Index, class Master, class Awaitable>
		inline winrt::fire_and_forget when_all_helper_single2(Master &master, Awaitable task)
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
		inline void when_all_helper(Master &master, const Tuple &tuple, std::index_sequence<I...>)
		{
			[[maybe_unused]] auto x = { when_all_helper_single(master, std::get<I>(tuple))... };
		}

		template<class Master, class Tuple, size_t...I>
		inline void when_all_helper2(Master &master, const Tuple &tuple, std::index_sequence<I...>)
		{
			[[maybe_unused]] auto x = { when_all_helper_single2<I>(master, std::get<I>(tuple))... };
		}

		// void case
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
				counter{ o.counter.load(std::memory_order_relaxed) },	// it is safe to "move" atomic this way because we don't use it until the final instance is allocated
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

		template<class...Awaitables>
		struct when_all_awaitable_void : when_all_awaitable_base<Awaitables...>
		{
			when_all_awaitable_void(Awaitables &&...awaitables) noexcept :
				when_all_awaitable_base{ std::forward<Awaitables>(awaitables)... }
			{}

			when_all_awaitable_void(when_all_awaitable_void &&o) noexcept :
				when_all_awaitable_base{ static_cast<when_all_awaitable_base &&>(o) }
			{}

			void finished() noexcept
			{
				check_resume();
			}

			void await_suspend(std::experimental::coroutine_handle<> handle)
			{
				resume = handle;
				using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
				when_all_helper(*this, awaitables, index_t{});
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
			using results_t = std::tuple<awaitable_type_t<Awaitables>...>;
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

			void await_suspend(std::experimental::coroutine_handle<> handle)
			{
				resume = handle;
				using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
				when_all_helper2(*this, awaitables, index_t{});
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
			static_assert(sizeof...(Awaitables) != 0, "when_all must be passed at least one argument");
			using first_type = get_first_t<std::decay_t<Awaitables>...>;

			return when_all_impl(std::is_base_of<winrt::Windows::Foundation::IAsyncAction,first_type>{}, std::forward<Awaitables>(awaitables)...);
		}

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

			void finished(T &&result_) noexcept
			{
				if (resume.load(std::memory_order_relaxed))
				{
					auto value = resume.exchange(nullptr, std::memory_order_relaxed);
					if (value)
					{
						result = std::move(result_);
						value();
					}
				}
			}
		};

		template<size_t Index, class Awaitable>
		inline winrt::fire_and_forget when_any_helper_single(std::shared_ptr<when_any_block_void> master, Awaitable task)
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

		template<class Tuple, size_t...I>
		inline void when_any_helper(const std::shared_ptr<when_any_block_void> &master, const Tuple &tuple, std::index_sequence<I...>)
		{
			[[maybe_unused]] auto x = { when_any_helper_single<I>(master, std::get<I>(tuple))... };
		}

		template<class...Awaitables>
		inline auto when_any_impl(std::true_type, Awaitables &&...awaitables)
		{
			struct when_any_awaitable
			{
				std::shared_ptr<when_any_block_void> ptr;
				std::tuple<std::decay_t<Awaitables>...> awaitables;

				when_any_awaitable(Awaitables &&...awaitables) noexcept :
					awaitables{ std::forward<Awaitables>(awaitables)... },
					ptr{ std::make_shared<when_any_block_void>() }
				{}

				bool await_ready() const noexcept
				{
					return false;
				}

				void await_suspend(std::experimental::coroutine_handle<> handle)
				{
					ptr->resume.store(handle, std::memory_order_relaxed);
					using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
					when_any_helper(ptr, awaitables, index_t{});
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
		inline winrt::fire_and_forget when_any_helper_single_value(std::shared_ptr<when_any_block_value<T>> master, Awaitable task)
		{
			try
			{
				master->finished(co_await task);
			}
			catch(...)
			{
				master->finished_exception();
			}
		}

		template<class T, class Tuple, size_t...I>
		inline void when_any_helper_value(const std::shared_ptr<when_any_block_value<T>> &master, const Tuple &tuple, std::index_sequence<I...>)
		{
			[[maybe_unused]] auto x = { when_any_helper_single_value<T>(master, std::get<I>(tuple))... };
		}


		template<class...Awaitables>
		inline auto when_any_impl(std::false_type, Awaitables &&...awaitables)
		{
			struct when_any_awaitable
			{
				using result_type = awaitable_type_t<get_first_t<Awaitables...>>;
				std::shared_ptr<when_any_block_value<result_type>> ptr;
				std::tuple<std::decay_t<Awaitables>...> awaitables;

				when_any_awaitable(Awaitables &&...awaitables) noexcept :
					awaitables{ std::forward<Awaitables>(awaitables)... },
					ptr{ std::make_shared<when_any_block_value<result_type>>() }
				{}

				bool await_ready() const noexcept
				{
					return false;
				}

				void await_suspend(std::experimental::coroutine_handle<> handle)
				{
					ptr->resume.store(handle, std::memory_order_relaxed);
					using index_t = std::make_index_sequence<sizeof...(Awaitables)>;
					when_any_helper_value(ptr, awaitables, index_t{});
				}

				result_type await_resume() const
				{
					if (ptr->exception)
						std::rethrow_exception(ptr->exception);
					else
						return std::move(ptr->result);
				}
			};

			return when_any_awaitable{ std::forward<Awaitables>(awaitables)... };
		}

		template<class...Awaitables>
		inline auto when_any(Awaitables &&...awaitables)
		{
			static_assert(sizeof...(Awaitables) != 0, "when_any must be passed at least one argument");
			using first_type = get_first_t<std::decay_t<Awaitables>...>;

			return when_any_impl(std::is_base_of<winrt::Windows::Foundation::IAsyncAction, first_type>{}, std::forward<Awaitables>(awaitables)...);
		}
	}

	using details::when_all;
	using details::when_any;
}
