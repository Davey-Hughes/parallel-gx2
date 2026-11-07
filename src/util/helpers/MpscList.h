#pragma once

#include <atomic>
#include <utility>

// Many threads hand items to one, without a lock: Push links a node onto a
// stack with one compare-exchange, TakeAll swaps the whole stack out and hands
// it back oldest first. The consumer takes everything at once, so a node is
// never popped while another thread looks at it (no ABA). For rare messages -
// a game asking for the GamePad's motor - drained once a frame.
template<typename T>
class MpscList
{
	struct Node
	{
		T value;
		Node* next;
	};

public:
	MpscList() = default;
	MpscList(const MpscList&) = delete;
	MpscList& operator=(const MpscList&) = delete;
	~MpscList() { TakeAll([](T&&) {}); }

	void Push(T value)
	{
		Node* node = new Node{std::move(value), m_head.load(std::memory_order_relaxed)};
		while (!m_head.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed))
			;
	}

	// Calls fn for everything pushed so far, in the order it was pushed
	template<typename TFn>
	void TakeAll(TFn fn)
	{
		Node* list = m_head.exchange(nullptr, std::memory_order_acquire);
		Node* reversed = nullptr;
		while (list)
		{
			Node* next = list->next;
			list->next = reversed;
			reversed = list;
			list = next;
		}
		while (reversed)
		{
			Node* next = reversed->next;
			fn(std::move(reversed->value));
			delete reversed;
			reversed = next;
		}
	}

private:
	std::atomic<Node*> m_head{nullptr};
};
