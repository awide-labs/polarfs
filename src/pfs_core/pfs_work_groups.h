/*
 * Common class to run a callback with a concurrency limit,
 * i.e. no more than "size" callback calls will be in progress
 * at the same time.
 */

#pragma once

#include "../ipc/access_spreader.h"
#include "../ipc/pfs_align.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>

template <typename Result> class WorkGroup : pfsutil::cacheline_align_t {
    public:
	std::optional<Result>
	wait_or_run(std::function<std::optional<Result>()> fn, bool &wait)
	{
		std::unique_lock<std::mutex> lk(mtx_);

		if (in_progress_) {
			auto prev_counter = counter_;
			cv_.wait(lk, [this, prev_counter]() {
				return counter_ != prev_counter;
			});
			wait = true;
			return result_;
		}

		in_progress_ = true;
		lk.unlock();

		auto res = fn();

		lk.lock();
		result_ = std::move(res);
		in_progress_ = false;
		counter_++;
		wait = false;
		cv_.notify_all();
		return result_;
	}

	void notify_all(std::optional<Result> result)
	{
		std::unique_lock<std::mutex> lk(mtx_, std::defer_lock);
		if (lk.try_lock()) {
			result_ = std::move(result);
			counter_++;
			cv_.notify_all();
		}
	}

    private:
	std::mutex mtx_;
	std::condition_variable cv_;
	bool in_progress_{ false };
	int counter_{ 0 };
	std::optional<Result> result_;
};

template <typename Result> class GroupedWorkRunner {
    public:
	GroupedWorkRunner(size_t size)
		: groups_(size)
	{
	}

	std::optional<Result> run(std::function<std::optional<Result>()> fn)
	{
		auto idx = pfsutil::AccessSpreader::current(groups_.size());
		bool wait;
		auto result = groups_[idx].wait_or_run(fn, wait);
		if (!wait) {
			for (size_t i = 0; i < groups_.size(); i++) {
				if (i == idx) {
					continue;
				}
				groups_[i].notify_all(result);
			}
		}
		return result;
	}

    private:
	std::vector<WorkGroup<Result>> groups_;
};
