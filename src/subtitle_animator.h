#pragma once

#include <string>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <chrono>

class SubtitleAnimator {
public:
	// Push updated text to OBS display (is_partial indicates active partial segment)
	using UpdateCallback = std::function<void(const std::string &, bool is_partial)>;

	SubtitleAnimator(UpdateCallback callback);
	~SubtitleAnimator();

	// Configuration
	void set_max_lines(size_t lines);
	void set_auto_clear_seconds(int seconds);

	// Configure partial update throttle in milliseconds
	void set_partial_throttle_ms(int ms);

	// Process transcription update
	void update_text(const std::string &text, bool is_final, size_t sentence_id);

	// Clear active state
	void clear();

	// Check auto-clear condition on silence timeout
	void trigger_auto_clear();

private:
	void worker_loop();
	void rebuild_target_strings();

	UpdateCallback m_callback;

	// Store confirmed final sentences
	std::deque<std::string> m_target_confirmed;

	// Track active partial sentences by sentence ID
	std::map<size_t, std::string> m_active_partial;

	// Committed prefix: text already displayed to the user for a partial sentence.
	// New partial updates for the same sentence_id must start with this prefix
	// or be longer than it; otherwise the update is suppressed to prevent flicker.
	std::map<size_t, std::string> m_committed_prefix;

	// Display lock: after displaying a final result, block partial updates
	// for this duration to prevent flash between sentences.
	int m_final_display_lock_ms{200};
	std::chrono::steady_clock::time_point m_last_final_time;

	// Cache display strings
	std::string m_target_confirmed_string;
	std::string m_target_partial_string;

	size_t m_max_lines{2};
	int m_auto_clear_seconds{5};

	// Throttle partial display updates in milliseconds
	int m_partial_throttle_ms{1500};

	std::chrono::steady_clock::time_point m_last_update_time;

	std::thread m_worker;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::atomic<bool> m_stop{false};
};
