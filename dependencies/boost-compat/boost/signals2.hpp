// boost::signals2::signal and connection, reduced to what EventService uses:
// connect, disconnect and emission, from any thread. Slots run in the order
// they were connected, as in Boost; a slot may disconnect itself while it runs.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace boost { namespace signals2 {

namespace detail {

struct slot_list_base
{
	virtual ~slot_list_base() = default;
	virtual void remove(uint64_t id) = 0;
};

} // namespace detail

class connection
{
public:
	connection() = default;
	connection(std::weak_ptr<detail::slot_list_base> list, uint64_t id) : m_list(std::move(list)), m_id(id) {}

	void disconnect() const
	{
		if (auto list = m_list.lock())
			list->remove(m_id);
	}

	bool connected() const { return !m_list.expired() && m_id != 0; }

	uint64_t id() const { return m_id; }

private:
	std::weak_ptr<detail::slot_list_base> m_list;
	uint64_t m_id = 0;
};

template<class Signature>
class signal;

template<class R, class... Args>
class signal<R(Args...)>
{
	struct slot_list : detail::slot_list_base
	{
		std::mutex mutex;
		std::vector<std::pair<uint64_t, std::shared_ptr<std::function<R(Args...)>>>> slots;
		uint64_t nextId = 1;

		void remove(uint64_t id) override
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (auto it = slots.begin(); it != slots.end(); ++it)
			{
				if (it->first == id)
				{
					slots.erase(it);
					return;
				}
			}
		}
	};

public:
	signal() : m_list(std::make_shared<slot_list>()) {}
	signal(const signal&) = delete;
	signal& operator=(const signal&) = delete;

	template<class F>
	connection connect(F&& f)
	{
		std::lock_guard<std::mutex> lock(m_list->mutex);
		const uint64_t id = m_list->nextId++;
		m_list->slots.emplace_back(id, std::make_shared<std::function<R(Args...)>>(std::forward<F>(f)));
		return connection(m_list, id);
	}

	void disconnect(const connection& c) { m_list->remove(c.id()); }

	void disconnect_all_slots()
	{
		std::lock_guard<std::mutex> lock(m_list->mutex);
		m_list->slots.clear();
	}

	bool empty() const
	{
		std::lock_guard<std::mutex> lock(m_list->mutex);
		return m_list->slots.empty();
	}

	// The slots are called outside the lock, on a snapshot, so one may connect
	// or disconnect without deadlocking.
	template<class... CallArgs>
	void operator()(CallArgs&&... args)
	{
		std::vector<std::shared_ptr<std::function<R(Args...)>>> snapshot;
		{
			std::lock_guard<std::mutex> lock(m_list->mutex);
			snapshot.reserve(m_list->slots.size());
			for (auto& s : m_list->slots)
				snapshot.push_back(s.second);
		}
		for (auto& s : snapshot)
			(*s)(args...);
	}

private:
	std::shared_ptr<slot_list> m_list;
};

}} // namespace boost::signals2
