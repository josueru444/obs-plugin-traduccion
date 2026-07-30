#include "remote_transcriber.h"
#include <obs-module.h>

#include <asio/steady_timer.hpp>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Minimal JSON Parser
// Expect: {"text": "...", "sentence_id": N, "is_final": true/false}
// Avoid external dependencies to keep binary size small.
// ─────────────────────────────────────────────────────────────────────────────
TranscriptionResult RemoteTranscriber::parse_json_response(const std::string &json)
{
	TranscriptionResult result;

	// Extract value of a JSON string field: "key": "value"
	auto extract_string = [&](const std::string &key) -> std::string {
		auto kpos = json.find("\"" + key + "\"");
		if (kpos == std::string::npos)
			return "";
		auto colon = json.find(':', kpos + key.size() + 2);
		if (colon == std::string::npos)
			return "";
		auto q1 = json.find('"', colon + 1);
		if (q1 == std::string::npos)
			return "";
		auto q2 = json.find('"', q1 + 1);
		if (q2 == std::string::npos)
			return "";
		return json.substr(q1 + 1, q2 - q1 - 1);
	};

	// Extract value of a JSON unsigned integer field: "key": 42
	auto extract_uint = [&](const std::string &key) -> size_t {
		auto kpos = json.find("\"" + key + "\"");
		if (kpos == std::string::npos)
			return 0;
		auto colon = json.find(':', kpos + key.size() + 2);
		if (colon == std::string::npos)
			return 0;
		auto npos = json.find_first_of("0123456789", colon + 1);
		if (npos == std::string::npos)
			return 0;
		return static_cast<size_t>(std::stoul(json.substr(npos)));
	};

	// Extract value of a JSON boolean field: "key": true/false
	auto extract_bool = [&](const std::string &key) -> bool {
		auto kpos = json.find("\"" + key + "\"");
		if (kpos == std::string::npos)
			return false;
		auto colon = json.find(':', kpos + key.size() + 2);
		if (colon == std::string::npos)
			return false;
		auto vpos = json.find_first_not_of(" \t\n\r", colon + 1);
		if (vpos == std::string::npos)
			return false;
		return json.compare(vpos, 4, "true") == 0;
	};

	result.text = extract_string("text");
	result.sentence_id = extract_uint("sentence_id");
	result.is_final = extract_bool("is_final");
	return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: get io_service from the active client
// ─────────────────────────────────────────────────────────────────────────────
asio::io_service &RemoteTranscriber::get_io_service()
{
	if (m_use_tls)
		return m_client_tls->get_io_service();
	return m_client_plain->get_io_service();
}

// ─────────────────────────────────────────────────────────────────────────────
// Process incoming text message (shared logic for both client types)
// ─────────────────────────────────────────────────────────────────────────────
void RemoteTranscriber::process_message(const std::string &payload)
{
	auto result = parse_json_response(payload);

	if (!result.text.empty() && m_callback) {
		blog(LOG_INFO, "[RemoteTranscriber] Received response (id=%zu, %s): %s",
		     result.sentence_id, result.is_final ? "FINAL" : "PARTIAL", result.text.c_str());
		m_callback(result);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
RemoteTranscriber::RemoteTranscriber(const std::string &url, ResultCallback on_result, StatusCallback on_status)
	: m_url(url), m_callback(std::move(on_result)), m_status_cb(std::move(on_status)),
	  m_use_tls(url.size() >= 6 && url.substr(0, 6) == "wss://")
{
	blog(LOG_INFO, "[RemoteTranscriber] Protocol: %s", m_use_tls ? "wss (TLS)" : "ws (plain)");

	// ── Initialize Opus encoder ───────────────────────────────────────────────
	int err = OPUS_OK;
	m_encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &err);
	if (err != OPUS_OK || !m_encoder) {
		blog(LOG_ERROR, "[RemoteTranscriber] opus_encoder_create failed: %s", opus_strerror(err));
		m_encoder = nullptr;
	} else {
		// Set 24kbps bitrate (sufficient for speech-to-text)
		opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(24000));
		// Optimize codec for voice signal
		opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
		// Set complexity to 5/10 (balance CPU usage and quality)
		opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5));
		// Disable DTX to send all frames continuously
		opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));
		blog(LOG_INFO, "[RemoteTranscriber] Opus encoder initialized (16kHz, mono, 24kbps)");
	}

	// ── Initialize the appropriate WebSocket client ───────────────────────────
	// Shared event handlers (connection_hdl is the same type for both clients)
	auto open_handler = [this](websocketpp::connection_hdl hdl) { on_open(hdl); };
	auto close_handler = [this](websocketpp::connection_hdl hdl) { on_close(hdl); };
	auto fail_handler = [this](websocketpp::connection_hdl hdl) { on_fail(hdl); };

	if (m_use_tls) {
		m_client_tls = std::make_unique<WsClientTls>();
		m_client_tls->init_asio();

		// TLS context for wss:// connections (support TLS 1.2 & TLS 1.3)
		m_client_tls->set_tls_init_handler([](websocketpp::connection_hdl) -> SslContext {
			auto ctx = websocketpp::lib::make_shared<asio::ssl::context>(
				asio::ssl::context::sslv23_client);
			ctx->set_options(asio::ssl::context::default_workarounds |
					 asio::ssl::context::no_sslv2 |
					 asio::ssl::context::no_sslv3);
			ctx->set_default_verify_paths();
			// Skip certificate verification for tunnel services (ngrok, Cloudflare, LitNG, etc.)
			ctx->set_verify_mode(asio::ssl::verify_none);
			return ctx;
		});

		m_client_tls->clear_access_channels(websocketpp::log::alevel::all);
		m_client_tls->clear_error_channels(websocketpp::log::elevel::all);

		m_client_tls->set_open_handler(open_handler);
		m_client_tls->set_close_handler(close_handler);
		m_client_tls->set_fail_handler(fail_handler);
		m_client_tls->set_message_handler(
			[this](websocketpp::connection_hdl, WsClientTls::message_ptr msg) {
				if (msg->get_opcode() == websocketpp::frame::opcode::text)
					process_message(msg->get_payload());
			});
	} else {
		m_client_plain = std::make_unique<WsClientPlain>();
		m_client_plain->init_asio();

		m_client_plain->clear_access_channels(websocketpp::log::alevel::all);
		m_client_plain->clear_error_channels(websocketpp::log::elevel::all);

		m_client_plain->set_open_handler(open_handler);
		m_client_plain->set_close_handler(close_handler);
		m_client_plain->set_fail_handler(fail_handler);
		m_client_plain->set_message_handler(
			[this](websocketpp::connection_hdl, WsClientPlain::message_ptr msg) {
				if (msg->get_opcode() == websocketpp::frame::opcode::text)
					process_message(msg->get_payload());
			});
	}

	// Initiate initial connection if URL non-empty
	if (!m_url.empty())
		connect();

	// Run asio io_context in dedicated network thread
	m_io_thread = std::thread([this]() {
		blog(LOG_INFO, "[RemoteTranscriber] Network thread started");
		if (m_use_tls)
			m_client_tls->run();
		else
			m_client_plain->run();
		blog(LOG_INFO, "[RemoteTranscriber] Network thread stopped");
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// Stop io_thread BEFORE freeing filter resources
// ─────────────────────────────────────────────────────────────────────────────
RemoteTranscriber::~RemoteTranscriber()
{
	// Signal stop reconnect attempts
	m_running.store(false);
	m_connected.store(false);

	// Close WebSocket connection cleanly if active
	if (m_use_tls)
		m_client_tls->stop();
	else
		m_client_plain->stop();

	// Wait for network thread to finish execution completely
	if (m_io_thread.joinable())
		m_io_thread.join();

	// Destroy Opus encoder
	if (m_encoder) {
		opus_encoder_destroy(m_encoder);
		m_encoder = nullptr;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection management
// ─────────────────────────────────────────────────────────────────────────────
void RemoteTranscriber::update_url(const std::string &new_url)
{
	if (!m_running.load())
		return;

	asio::post(get_io_service(), [this, new_url]() {
		if (m_url == new_url)
			return;

		blog(LOG_INFO, "[RemoteTranscriber] Update URL to %s", new_url.c_str());
		m_url = new_url;
		bool old_tls = m_use_tls;
		m_use_tls = (m_url.size() >= 6 && m_url.substr(0, 6) == "wss://");

		if (m_connected.load()) {
			m_connected.store(false);
			websocketpp::lib::error_code ec;
			if (old_tls && m_client_tls)
				m_client_tls->close(m_hdl, websocketpp::close::status::going_away, "URL changed", ec);
			else if (!old_tls && m_client_plain)
				m_client_plain->close(m_hdl, websocketpp::close::status::going_away, "URL changed", ec);
		} else if (!m_url.empty()) {
			connect();
		}
	});
}

void RemoteTranscriber::connect()
{
	if (!m_running.load() || m_url.empty())
		return;

	// Re-evaluate protocol for current URL
	m_use_tls = (m_url.size() >= 6 && m_url.substr(0, 6) == "wss://");

	if (m_status_cb)
		m_status_cb("🟡 Conectando...");

	if (m_use_tls) {
		websocketpp::lib::error_code ec;
		auto con = m_client_tls->get_connection(m_url, ec);
		if (ec) {
			blog(LOG_ERROR, "[RemoteTranscriber] Connect error to '%s': %s", m_url.c_str(),
			     ec.message().c_str());
			if (m_status_cb)
				m_status_cb("🔴 Desconectado");
			schedule_reconnect();
			return;
		}

		// Skip ngrok free-tier browser interstitial page
		con->append_header("ngrok-skip-browser-warning", "true");

		// Set OpenSSL SNI (Server Name Indication) for Cloudflare / LitNG / reverse proxies
		// SNI must NOT contain port number and must NOT be an IP address
		auto uri = con->get_uri();
		if (uri) {
			std::string sni_host = uri->get_host();
			size_t colon_pos = sni_host.find(':');
			if (colon_pos != std::string::npos) {
				sni_host = sni_host.substr(0, colon_pos);
			}

			bool is_ip = !sni_host.empty();
			for (char c : sni_host) {
				if (!std::isdigit(c) && c != '.' && c != ':') {
					is_ip = false;
					break;
				}
			}

			if (!is_ip && !sni_host.empty()) {
				SSL *ssl = con->get_socket().native_handle();
				if (ssl) {
					SSL_set_tlsext_host_name(ssl, sni_host.c_str());
				}
			}
		}

		m_hdl = con->get_handle();
		m_client_tls->connect(con);
	} else {
		websocketpp::lib::error_code ec;
		auto con = m_client_plain->get_connection(m_url, ec);
		if (ec) {
			blog(LOG_ERROR, "[RemoteTranscriber] Connect error to '%s': %s", m_url.c_str(),
			     ec.message().c_str());
			if (m_status_cb)
				m_status_cb("🔴 Desconectado");
			schedule_reconnect();
			return;
		}
		m_hdl = con->get_handle();
		m_client_plain->connect(con);
	}

	blog(LOG_INFO, "[RemoteTranscriber] Connecting to %s ...", m_url.c_str());
}

void RemoteTranscriber::schedule_reconnect()
{
	if (!m_running.load() || m_url.empty())
		return;

	if (m_status_cb)
		m_status_cb("🔴 Desconectado (Reintentando en 3s...)");

	// Use steady_timer to avoid blocking io_context for 3 seconds
	auto timer = std::make_shared<asio::steady_timer>(get_io_service(),
	                                                   std::chrono::seconds(3));
	timer->async_wait([this, timer](const asio::error_code &ec) {
		if (!ec && m_running.load() && !m_url.empty()) {
			blog(LOG_INFO, "[RemoteTranscriber] Retrying connection to %s ...", m_url.c_str());
			connect();
		}
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Handle WebSocket events
// (executed inside network / io_context thread)
// ─────────────────────────────────────────────────────────────────────────────
void RemoteTranscriber::on_open(websocketpp::connection_hdl hdl)
{
	m_hdl = hdl;
	m_connected.store(true);
	if (m_status_cb)
		m_status_cb("🟢 Conectado");
	blog(LOG_INFO, "[RemoteTranscriber] Connected to %s", m_url.c_str());
}

void RemoteTranscriber::on_close(websocketpp::connection_hdl hdl)
{
	(void)hdl;
	m_connected.store(false);
	if (m_status_cb)
		m_status_cb("🔴 Desconectado");
	blog(LOG_INFO, "[RemoteTranscriber] Connection closed, schedule reconnect...");
	schedule_reconnect();
}

void RemoteTranscriber::on_fail(websocketpp::connection_hdl hdl)
{
	m_connected.store(false);
	if (m_status_cb)
		m_status_cb("🔴 Desconectado");

	std::string err_reason = "Unknown error";
	websocketpp::lib::error_code ec;
	if (m_use_tls && m_client_tls) {
		auto con = m_client_tls->get_con_from_hdl(hdl, ec);
		if (!ec && con) {
			err_reason = con->get_ec().message();
			if (con->get_response_code() != websocketpp::http::status_code::uninitialized) {
				err_reason += " (HTTP " + std::to_string(con->get_response_code()) + " " +
					      con->get_response_msg() + ")";
			}
		}
	} else if (!m_use_tls && m_client_plain) {
		auto con = m_client_plain->get_con_from_hdl(hdl, ec);
		if (!ec && con) {
			err_reason = con->get_ec().message();
			if (con->get_response_code() != websocketpp::http::status_code::uninitialized) {
				err_reason += " (HTTP " + std::to_string(con->get_response_code()) + " " +
					      con->get_response_msg() + ")";
			}
		}
	}

	blog(LOG_WARNING, "[RemoteTranscriber] Failed to connect to %s: %s (retry in 3s...)",
	     m_url.c_str(), err_reason.c_str());
	schedule_reconnect();
}

// ─────────────────────────────────────────────────────────────────────────────
// PCM to Opus encoding and packet send
//
// Binary message protocol:
//   Bytes 0-3  : sentence_id (uint32, little-endian)
//   Byte  4    : is_final    (0 = partial, 1 = final)
//   Bytes 5+   : Opus frames prefixed with length:
//                  [2 bytes length LE][N bytes Opus data]
// ─────────────────────────────────────────────────────────────────────────────
void RemoteTranscriber::send_audio(const std::vector<float> &pcm, size_t sentence_id, bool is_final)
{
	if (!m_connected.load()) {
		blog(LOG_DEBUG,
		     "[RemoteTranscriber] send_audio: no connection, drop segment %zu",
		     sentence_id);
		return;
	}
	if (!m_encoder) {
		blog(LOG_ERROR, "[RemoteTranscriber] send_audio: Opus encoder unavailable");
		return;
	}
	if (pcm.empty())
		return;

	// ── Build binary message ──────────────────────────────────────────────────
	std::vector<uint8_t> message;
	// Reserve estimated size: header(5) + ~80 bytes per 20ms frame
	message.reserve(5 + (pcm.size() / FRAME_SIZE + 1) * 80);

	// Header: sentence_id (4 bytes, little-endian)
	auto sid = static_cast<uint32_t>(sentence_id);
	message.push_back(static_cast<uint8_t>(sid));
	message.push_back(static_cast<uint8_t>(sid >> 8));
	message.push_back(static_cast<uint8_t>(sid >> 16));
	message.push_back(static_cast<uint8_t>(sid >> 24));
	// Header: is_final (1 byte)
	message.push_back(is_final ? 1 : 0);

	// ── Encode Opus frames (20ms per frame = 320 samples at 16kHz) ────────────
	std::vector<uint8_t> frame_buf(MAX_PACKET);
	size_t offset = 0;
	int frames_encoded = 0;

	while (offset + static_cast<size_t>(FRAME_SIZE) <= pcm.size()) {
		int bytes = opus_encode_float(m_encoder, pcm.data() + offset, FRAME_SIZE,
		                              frame_buf.data(), MAX_PACKET);
		if (bytes > 0) {
			// Prefix 2 bytes frame length (little-endian)
			auto len = static_cast<uint16_t>(bytes);
			message.push_back(static_cast<uint8_t>(len));
			message.push_back(static_cast<uint8_t>(len >> 8));
			message.insert(message.end(), frame_buf.begin(), frame_buf.begin() + bytes);
			++frames_encoded;
		} else if (bytes < 0) {
			blog(LOG_ERROR, "[RemoteTranscriber] Opus error: %s", opus_strerror(bytes));
		}
		offset += FRAME_SIZE;
	}

	if (frames_encoded == 0) {
		blog(LOG_WARNING,
		     "[RemoteTranscriber] Zero Opus frames encoded for segment %zu",
		     sentence_id);
		return;
	}

	blog(LOG_DEBUG, "[RemoteTranscriber] Send segment %zu: %d Opus frames, %zu bytes",
	     sentence_id, frames_encoded, message.size());

	// ── Send to network thread via asio::post ─────────────────────────────────
	// Post send operation into io_context thread
	std::string payload(reinterpret_cast<const char *>(message.data()), message.size());

	asio::post(get_io_service(), [this, payload]() {
		if (!m_connected.load())
			return;
		websocketpp::lib::error_code ec;
		if (m_use_tls)
			m_client_tls->send(m_hdl, payload, websocketpp::frame::opcode::binary, ec);
		else
			m_client_plain->send(m_hdl, payload, websocketpp::frame::opcode::binary, ec);
		if (ec) {
			blog(LOG_ERROR, "[RemoteTranscriber] Send error: %s", ec.message().c_str());
		}
	});
}
