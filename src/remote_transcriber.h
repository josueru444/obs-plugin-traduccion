#pragma once

// Define macros before including websocketpp/asio
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#ifndef _WEBSOCKETPP_CPP11_STL_
#define _WEBSOCKETPP_CPP11_STL_
#endif

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#include <opus/opus.h>

#include <asio/ssl.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Result returned by WebSocket server (JSON format)
// {"text": "Hello world", "sentence_id": 3, "is_final": true}
// ─────────────────────────────────────────────────────────────────────────────
struct TranscriptionResult {
	std::string text;
	size_t sentence_id = 0;
	bool is_final = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// RemoteTranscriber
//
// Responsibilities:
//   1. Maintain persistent WebSocket connection to transcription server.
//   2. Reconnect automatically on connection loss.
//   3. Encode PCM audio (16kHz, mono, float32) -> Opus and send as binary.
//   4. Receive JSON response from server and invoke on_result callback.
//   5. Support both ws:// (plain) and wss:// (TLS) connections.
//
// Binary protocol sent to server:
//   [4 bytes: sentence_id (uint32 LE)]
//   [1 byte:  is_final (0 or 1)]
//   [per Opus frame: [2 bytes length LE][N bytes Opus data]]
// ─────────────────────────────────────────────────────────────────────────────
class RemoteTranscriber {
public:
	using WsClientPlain = websocketpp::client<websocketpp::config::asio_client>;
	using WsClientTls = websocketpp::client<websocketpp::config::asio_tls_client>;
	using SslContext = websocketpp::lib::shared_ptr<asio::ssl::context>;
	using ResultCallback = std::function<void(const TranscriptionResult &)>;
	using StatusCallback = std::function<void(const std::string &)>;

	explicit RemoteTranscriber(const std::string &url, ResultCallback on_result, StatusCallback on_status = nullptr);
	~RemoteTranscriber();

	// Disable copy and move operations
	RemoteTranscriber(const RemoteTranscriber &) = delete;
	RemoteTranscriber &operator=(const RemoteTranscriber &) = delete;

	/**
	 * Update WebSocket URL and reconnect if changed.
	 * Thread-safe: call from any thread.
	 * Note: cannot switch between ws:// and wss:// at runtime.
	 *
	 * @param new_url New WebSocket target URL
	 */
	void update_url(const std::string &new_url);

	/**
	 * Encode speech segment to Opus and send to server.
	 * Thread-safe: call from any thread.
	 *
	 * @param pcm_16khz  Raw PCM audio: 16kHz, mono, float32 in [-1.0, 1.0]
	 * @param sentence_id Unique segment identifier (for server response correlation)
	 * @param is_final    true if final sentence segment (silence after speech)
	 */
	void send_audio(const std::vector<float> &pcm_16khz, size_t sentence_id, bool is_final);

	bool is_connected() const { return m_connected.load(); }

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
	std::unique_ptr<WsClientPlain> m_client_plain;
	std::unique_ptr<WsClientTls> m_client_tls;
	websocketpp::connection_hdl m_hdl;
	std::thread m_io_thread;
	std::atomic<bool> m_connected{false};
	std::atomic<bool> m_running{true};

	// ── Opus ───────────────────────────────────────────────────────────────────
	OpusEncoder *m_encoder{nullptr};

	static constexpr int SAMPLE_RATE = 16000; // Hz
	static constexpr int CHANNELS = 1;        // mono
	static constexpr int FRAME_SIZE = 320;    // samples per frame = 20ms at 16kHz
	static constexpr int MAX_PACKET = 4000;   // max bytes per Opus packet
};
