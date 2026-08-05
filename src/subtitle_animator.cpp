#include "subtitle_animator.h"
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// SubtitleAnimator — Animate and throttle subtitle display updates
// ─────────────────────────────────────────────────────────────────────────────

SubtitleAnimator::SubtitleAnimator(UpdateCallback callback)
	: m_callback(callback), m_stop(false)
{
	m_last_update_time = std::chrono::steady_clock::now();
	m_worker = std::thread(&SubtitleAnimator::worker_loop, this);
}

SubtitleAnimator::~SubtitleAnimator()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
	}
	m_cv.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}

void SubtitleAnimator::set_max_lines(size_t lines)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_max_lines = lines;
}

void SubtitleAnimator::set_auto_clear_seconds(int seconds)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_auto_clear_seconds = seconds;
}

void SubtitleAnimator::set_partial_throttle_ms(int ms)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_partial_throttle_ms = ms;
}

void SubtitleAnimator::clear()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_target_confirmed.clear();
		m_active_partial.clear();
		m_committed_prefix.clear();
		m_target_confirmed_string = "";
		m_target_partial_string = "";
		m_last_update_time = std::chrono::steady_clock::now();
	}
	m_cv.notify_one();
}

void SubtitleAnimator::trigger_auto_clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_auto_clear_seconds > 0 &&
	    (!m_target_confirmed_string.empty() || !m_target_partial_string.empty())) {
		auto now = std::chrono::steady_clock::now();
		auto secs = std::chrono::duration_cast<std::chrono::seconds>(
			now - m_last_update_time).count();
		if (secs >= m_auto_clear_seconds) {
			m_target_confirmed.clear();
			m_active_partial.clear();
			m_target_confirmed_string = "";
			m_target_partial_string = "";
			m_cv.notify_one();
		}
	}
}

void SubtitleAnimator::rebuild_target_strings()
{
	// Join confirmed sentences into a single display string
	m_target_confirmed_string = "";
	for (const auto &line : m_target_confirmed)
		m_target_confirmed_string += line + " ";
	if (!m_target_confirmed_string.empty() && m_target_confirmed_string.back() == ' ')
		m_target_confirmed_string.pop_back();

	// Extract latest partial sentence
	m_target_partial_string = "";
	if (!m_active_partial.empty())
		m_target_partial_string = m_active_partial.rbegin()->second;
}

// Update target subtitle text for confirmed or partial results
// sentence_id = -1 triggers hard reset
void SubtitleAnimator::update_text(const std::string &text, bool is_final,
                                   size_t sentence_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_last_update_time = std::chrono::steady_clock::now();

	if (sentence_id == (size_t)-1) {
		// Perform hard reset
		m_target_confirmed.clear();
		m_active_partial.clear();
		m_target_confirmed_string = "";
		m_target_partial_string = "";
		m_cv.notify_one();
		return;
	}

	if (is_final) {
		// Clear completed partial entries
		for (auto it = m_active_partial.begin(); it != m_active_partial.end(); ) {
			if (it->first <= sentence_id)
				it = m_active_partial.erase(it);
			else
				++it;
		}

		// Clear committed prefix entries for completed sentences
		for (auto it = m_committed_prefix.begin(); it != m_committed_prefix.end(); ) {
			if (it->first <= sentence_id)
				it = m_committed_prefix.erase(it);
			else
				++it;
		}

		// Append to confirmed history
		if (!text.empty()) {
			m_target_confirmed.push_back(text);
			while (m_target_confirmed.size() > 20)
				m_target_confirmed.pop_front();
		}
		m_last_final_time = std::chrono::steady_clock::now();
	} else {
		// Update active partial entry with committed prefix logic
		if (text.empty()) {
			m_active_partial.erase(sentence_id);
			m_committed_prefix.erase(sentence_id);
		} else {
			auto cp_it = m_committed_prefix.find(sentence_id);
			if (cp_it != m_committed_prefix.end() && !cp_it->second.empty()) {
				const std::string &committed = cp_it->second;
				// Check if new text starts with the committed prefix
				if (text.size() >= committed.size() &&
				    text.compare(0, committed.size(), committed) == 0) {
					// Text grew naturally — accept and extend prefix
					m_committed_prefix[sentence_id] = text;
				} else if (text.size() > committed.size()) {
					// Text is longer but start changed — accept the
					// correction (AI refined the translation)
					m_committed_prefix[sentence_id] = text;
				} else {
					// Text is shorter or same length with different start.
					// Suppress this update — wait for final to correct.
					rebuild_target_strings();
					m_cv.notify_one();
					return;
				}
			} else {
				// First partial for this sentence_id
				m_committed_prefix[sentence_id] = text;
			}
			m_active_partial[sentence_id] = text;
		}

		// Evict stale partial entries
		for (auto it = m_active_partial.begin(); it != m_active_partial.end(); ) {
			if (it->first < sentence_id)
				it = m_active_partial.erase(it);
			else
				++it;
		}
		// Evict stale committed prefix entries
		for (auto it = m_committed_prefix.begin(); it != m_committed_prefix.end(); ) {
			if (it->first < sentence_id)
				it = m_committed_prefix.erase(it);
			else
				++it;
		}
	}

	rebuild_target_strings();
	m_cv.notify_one();
}

// ─────────────────────────────────────────────────────────────────────────────
// worker_loop — display thread
// ─────────────────────────────────────────────────────────────────────────────
void SubtitleAnimator::worker_loop()
{
	std::string current_partial = "";
	std::string current_confirmed = "";
	auto last_partial_tick = std::chrono::steady_clock::now();

	while (true) {
		std::string target_confirmed;
		std::string target_partial;
		int throttle_ms;

		int final_lock_ms;
		std::chrono::steady_clock::time_point last_final_tp;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait_for(lock, std::chrono::milliseconds(50),
				[this, &current_partial, &current_confirmed]() {
					return m_stop
					    || m_target_partial_string != current_partial
					    || m_target_confirmed_string != current_confirmed;
				});

			if (m_stop) break;

			target_confirmed = m_target_confirmed_string;
			target_partial   = m_target_partial_string;
			throttle_ms      = m_partial_throttle_ms;
			final_lock_ms    = m_final_display_lock_ms;
			last_final_tp    = m_last_final_time;
		}

		if (target_confirmed == current_confirmed && target_partial == current_partial)
			continue;

		// Apply immediate update when confirmed text changes
		if (target_confirmed != current_confirmed) {
			current_confirmed = target_confirmed;
			current_partial   = target_partial;

			std::string display = current_confirmed;
			if (!current_partial.empty())
				display += (display.empty() ? "" : " ") + current_partial;

			if (m_callback) m_callback(display, !current_partial.empty());
			last_partial_tick = std::chrono::steady_clock::now();
			continue;
		}

		// Throttle partial text updates
		auto now  = std::chrono::steady_clock::now();
		auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - last_partial_tick).count();

		// Display lock: suppress partial updates briefly after a final
		auto since_final = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - last_final_tp).count();
		if (since_final < final_lock_ms) {
			continue;
		}

		// Check if partial text was cleared
		bool partial_cleared = target_partial.empty() && !current_partial.empty();

		if (!partial_cleared && diff < throttle_ms)
			continue;

		current_partial = target_partial;

		std::string display = current_confirmed;
		if (!current_partial.empty())
			display += (display.empty() ? "" : " ") + current_partial;

		if (m_callback) m_callback(display, !current_partial.empty());
		last_partial_tick = now;
	}
}
