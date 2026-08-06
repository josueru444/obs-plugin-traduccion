#pragma once

// Include Windows headers before Asio
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Define Asio macros for WebSocketPP
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#ifndef _WEBSOCKETPP_CPP11_STL_
#define _WEBSOCKETPP_CPP11_STL_
#endif

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#ifdef HAVE_OPENSSL
#include <websocketpp/config/asio_client.hpp>
#include <asio/ssl.hpp>
#endif

#if defined(__has_include) && __has_include(<opus/opus.h>)
#include <opus/opus.h>
#elif defined(__has_include) && __has_include(<opus.h>)
#include <opus.h>
#else
#include <opus/opus.h>
#endif

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Represent transcription result received from remote server
struct TranscriptionResult {
	std::string text;
	size_t sentence_id = 0;
	bool is_final = false;
};

// Maintain WebSocket client connection and stream Opus-encoded audio
class RemoteTranscriber {
public:
	using WsClientPlain = websocketpp::client<websocketpp::config::asio_client>;
#ifdef HAVE_OPENSSL
	using WsClientTls = websocketpp::client<websocketpp::config::asio_tls_client>;
	using SslContext = websocketpp::lib::shared_ptr<asio::ssl::context>;
#endif
	using ResultCallback = std::function<void(const TranscriptionResult &)>;
	using StatusCallback = std::function<void(const std::string &)>;

	explicit RemoteTranscriber(const std::string &url, ResultCallback on_result, StatusCallback on_status = nullptr);
	~RemoteTranscriber();

	// Disable copy and move operations
	RemoteTranscriber(const RemoteTranscriber &) = delete;
	RemoteTranscriber &operator=(const RemoteTranscriber &) = delete;

	// Update WebSocket target URL and reconnect
	void update_url(const std::string &new_url);

	// Encode PCM audio to Opus and send segment to server
	void send_audio(const std::vector<float> &pcm_16khz, size_t sentence_id, bool is_final);

	bool is_connected() const { return m_connected.load(); }
	std::string get_url() const { return m_url; }

private:
	// Manage connection
	void connect();
	void schedule_reconnect();

	// Handle WebSocket events
	void on_open(websocketpp::connection_hdl hdl);
	void on_close(websocketpp::connection_hdl hdl);
	void on_fail(websocketpp::connection_hdl hdl);

	// Process incoming text message payload (shared by both client types)
	void process_message(const std::string &payload);

	// Parse minimal JSON response from server
	static TranscriptionResult parse_json_response(const std::string &json);

	// ── Helpers for dual-client dispatch ────────────────────────────────────────
	asio::io_service &get_io_service();

	// ── WebSocket ──────────────────────────────────────────────────────────────
	std::string m_url;
	bool m_use_tls{false};
	ResultCallback m_callback;
	StatusCallback m_status_cb;
	asio::io_service m_io_service;
	asio::executor_work_guard<asio::io_service::executor_type> m_work_guard;
	std::unique_ptr<WsClientPlain> m_client_plain;
#ifdef HAVE_OPENSSL
	std::unique_ptr<WsClientTls> m_client_tls;
#endif
	websocketpp::connection_hdl m_hdl;
	std::thread m_io_thread;
	std::atomic<bool> m_connected{false};
	std::atomic<bool> m_running{true};
	std::shared_ptr<bool> m_alive;

	// ── Opus ───────────────────────────────────────────────────────────────────
	OpusEncoder *m_encoder{nullptr};

	// Cache Opus encoding state incrementally
	size_t m_cached_sentence_id{static_cast<size_t>(-1)};
	std::vector<uint8_t> m_encoded_frames_cache;
	size_t m_encoded_pcm_count{0};

	static constexpr int SAMPLE_RATE = 16000; // Hz
	static constexpr int CHANNELS = 1;        // mono
	static constexpr int FRAME_SIZE = 320;    // samples per frame = 20ms at 16kHz
	static constexpr int MAX_PACKET = 4000;   // max bytes per Opus packet
};
