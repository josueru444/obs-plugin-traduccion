#include "remote_transcriber.h"
#include <obs-module.h>

#include <asio/steady_timer.hpp>
#include <chrono>

// Parse JSON response payload
TranscriptionResult RemoteTranscriber::parse_json_response(const std::string &json)
{
	TranscriptionResult result;

	// Extract JSON string property
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

	// Extract JSON unsigned integer property
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
		try {
			return static_cast<size_t>(std::stoul(json.substr(npos)));
		} catch (...) {
			return 0;
		}
	};

	// Extract JSON boolean property
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

// Return reference to io_service context
asio::io_service &RemoteTranscriber::get_io_service()
{
	return m_io_service;
}

// Process incoming WebSocket message payload
void RemoteTranscriber::process_message(const std::string &payload)
{
	TranscriptionResult res = parse_json_response(payload);
	if (m_callback) {
		m_callback(res);
	}
}

// Initialize network transcriber client instance
RemoteTranscriber::RemoteTranscriber(const std::string &url, ResultCallback on_result, StatusCallback on_status)
	: m_url(url), m_callback(std::move(on_result)), m_status_cb(std::move(on_status)),
	  m_use_tls(url.size() >= 6 && url.substr(0, 6) == "wss://"),
	  m_work_guard(asio::make_work_guard(m_io_service)),
	  m_alive(std::make_shared<bool>(true))
{
	blog(LOG_INFO, "[RemoteTranscriber] Protocol: %s", m_use_tls ? "wss (TLS)" : "ws (plain)");

	// Initialize Opus encoder
	int err = OPUS_OK;
	m_encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &err);
	if (err != OPUS_OK || !m_encoder) {
		blog(LOG_ERROR, "[RemoteTranscriber] opus_encoder_create failed: %s", opus_strerror(err));
		m_encoder = nullptr;
	} else {
		// Configure Opus encoder parameters
		opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(24000));
		opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
		opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5));
		opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));
		blog(LOG_INFO, "[RemoteTranscriber] Opus encoder initialized (16kHz, mono, 24kbps)");
	}

	// Configure TLS and plain WebSocket client handlers
	auto open_handler = [this](websocketpp::connection_hdl hdl) { on_open(hdl); };
	auto close_handler = [this](websocketpp::connection_hdl hdl) { on_close(hdl); };
	auto fail_handler = [this](websocketpp::connection_hdl hdl) { on_fail(hdl); };

#ifdef HAVE_OPENSSL
	// Configure TLS client
	m_client_tls = std::make_unique<WsClientTls>();
	m_client_tls->init_asio(&m_io_service);
	m_client_tls->set_tls_init_handler([](websocketpp::connection_hdl) -> SslContext {
		auto ctx = websocketpp::lib::make_shared<asio::ssl::context>(
			asio::ssl::context::sslv23_client);
		ctx->set_options(asio::ssl::context::default_workarounds |
				 asio::ssl::context::no_sslv2 |
				 asio::ssl::context::no_sslv3);
		ctx->set_default_verify_paths();
		ctx->set_verify_mode(asio::ssl::verify_none);
		return ctx;
	});

	m_client_tls->set_socket_init_handler([this](websocketpp::connection_hdl hdl, asio::ssl::stream<asio::ip::tcp::socket>& ssl_stream) {
		websocketpp::lib::error_code ec;
		auto con = m_client_tls->get_con_from_hdl(hdl, ec);
		if (!ec && con) {
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
					SSL_set_tlsext_host_name(ssl_stream.native_handle(), sni_host.c_str());
				}
			}
		}
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
#endif

	// Configure plain WS client
	m_client_plain = std::make_unique<WsClientPlain>();
	m_client_plain->init_asio(&m_io_service);
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

	// Run Asio event loop on dedicated network thread
	m_io_thread = std::thread([this]() {
		blog(LOG_INFO, "[RemoteTranscriber] Network thread started");
		m_io_service.run();
		blog(LOG_INFO, "[RemoteTranscriber] Network thread stopped");
	});

	// Initiate initial connection if URL non-empty
	if (!m_url.empty()) {
		asio::post(m_io_service, [this]() {
			connect();
		});
	}
}

// Clean up network transcriber resources
RemoteTranscriber::~RemoteTranscriber()
{
	if (m_alive) {
		*m_alive = false;
	}

	// Signal stop reconnect attempts
	m_running.store(false);
	m_connected.store(false);

	// Stop both clients cleanly
#ifdef HAVE_OPENSSL
	if (m_client_tls)
		m_client_tls->stop();
#endif
	if (m_client_plain)
		m_client_plain->stop();

	m_work_guard.reset();
	m_io_service.stop();

	// Wait for network thread to finish execution completely
	if (m_io_thread.joinable())
		m_io_thread.join();

	// Destroy Opus encoder
	if (m_encoder) {
		opus_encoder_destroy(m_encoder);
		m_encoder = nullptr;
	}
}

static bool is_same_hdl(const websocketpp::connection_hdl &h1, const websocketpp::connection_hdl &h2)
{
	return !h1.owner_before(h2) && !h2.owner_before(h1);
}

// Update WebSocket endpoint URL and reconnect
void RemoteTranscriber::update_url(const std::string &new_url)
{
	if (!m_running.load())
		return;

	asio::post(get_io_service(), [this, new_url]() {
		blog(LOG_INFO, "[RemoteTranscriber] Update URL to %s", new_url.c_str());
		if (m_url == new_url && m_connected.load()) {
			// If the URL hasn't changed and we are already connected, do not reconnect.
			// This allows the update_url function to act as a "refresh status" action.
			if (m_status_cb)
				m_status_cb("🟢 Conectado");
			return;
		}
		m_url = new_url;
		connect();
	});
}


// Initiate WebSocket connection
void RemoteTranscriber::connect()
{
	if (!m_running.load())
		return;

	// Close old connection safely if active before starting a new one
	m_connected.store(false);
	if (!m_hdl.expired()) {
		websocketpp::lib::error_code ec;
#ifdef HAVE_OPENSSL
		if (m_use_tls && m_client_tls)
			m_client_tls->close(m_hdl, websocketpp::close::status::going_away, "Reconnecting", ec);
		else
#endif
		if (!m_use_tls && m_client_plain)
			m_client_plain->close(m_hdl, websocketpp::close::status::going_away, "Reconnecting", ec);
		m_hdl.reset();
	}

	if (m_url.empty()) {
		if (m_status_cb)
			m_status_cb("🔴 Desconectado");
		return;
	}

	// Validate URL scheme before any connection attempt.
	// websocketpp crashes on URIs like "ws:localhost" (missing "//").
	bool is_ws  = (m_url.size() >= 5 && m_url.substr(0, 5) == "ws://");
	bool is_wss = (m_url.size() >= 6 && m_url.substr(0, 6) == "wss://");
	if (!is_ws && !is_wss) {
		blog(LOG_WARNING, "[RemoteTranscriber] Invalid URL scheme, ignoring: %s", m_url.c_str());
		if (m_status_cb)
			m_status_cb("🔴 URL inválida");
		return;
	}

	// Re-evaluate protocol for current URL
	m_use_tls = is_wss;

	if (m_status_cb)
		m_status_cb("🟡 Conectando...");


	if (m_use_tls) {
#ifdef HAVE_OPENSSL
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
        con->replace_header("User-Agent", "OBS-Plugin-Traduccion/1.0");

        // Set Origin to avoid 400 Bad Request on strict servers/proxies
        if (auto uri = con->get_uri()) {
            std::string sni_host = uri->get_host();
            size_t colon = sni_host.find(':');
            if (colon != std::string::npos) sni_host = sni_host.substr(0, colon);
            con->append_header("Origin", "https://" + sni_host);
        }

		m_hdl = con->get_handle();
		m_client_tls->connect(con);
#else
		blog(LOG_WARNING, "[RemoteTranscriber] OpenSSL is disabled. Cannot connect to wss:// URL: %s", m_url.c_str());
		if (m_status_cb)
			m_status_cb("🔴 TLS no disponible");
		return;
#endif
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

		con->append_header("ngrok-skip-browser-warning", "true");
        con->replace_header("User-Agent", "OBS-Plugin-Traduccion/1.0");
        if (auto uri = con->get_uri()) {
            std::string sni_host = uri->get_host();
            size_t colon = sni_host.find(':');
            if (colon != std::string::npos) sni_host = sni_host.substr(0, colon);
            con->append_header("Origin", "http://" + sni_host);
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
	timer->async_wait([this, timer, alive = m_alive](const asio::error_code &ec) {
		if (!*alive) return;
		if (!ec && m_running.load() && !m_url.empty()) {
			blog(LOG_INFO, "[RemoteTranscriber] Retrying connection to %s ...", m_url.c_str());
			connect();
		}
	});
}

// Handle WebSocket connection open event
void RemoteTranscriber::on_open(websocketpp::connection_hdl hdl)
{
	if (!is_same_hdl(hdl, m_hdl))
		return;

	m_connected.store(true);
	if (m_status_cb)
		m_status_cb("🟢 Conectado");
	blog(LOG_INFO, "[RemoteTranscriber] Connected to %s", m_url.c_str());
}

void RemoteTranscriber::on_close(websocketpp::connection_hdl hdl)
{
	if (!is_same_hdl(hdl, m_hdl))
		return;

	m_connected.store(false);
	if (m_status_cb)
		m_status_cb("🔴 Desconectado");
	blog(LOG_INFO, "[RemoteTranscriber] Connection closed, schedule reconnect...");
	schedule_reconnect();
}

void RemoteTranscriber::on_fail(websocketpp::connection_hdl hdl)
{
	if (!is_same_hdl(hdl, m_hdl))
		return;

	m_connected.store(false);
	if (m_status_cb)
		m_status_cb("🔴 Desconectado");

	std::string err_reason = "Unknown error";
	websocketpp::lib::error_code ec;
#ifdef HAVE_OPENSSL
	if (m_use_tls && m_client_tls) {
		auto con = m_client_tls->get_con_from_hdl(hdl, ec);
		if (!ec && con) {
			err_reason = con->get_ec().message();
			if (con->get_response_code() != websocketpp::http::status_code::uninitialized) {
				err_reason += " (HTTP " + std::to_string(con->get_response_code()) + " " +
					      con->get_response_msg() + ")";
			}
		}
	} else
#endif
	if (!m_use_tls && m_client_plain) {
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

// Encode PCM audio to Opus and send segment over WebSocket
void RemoteTranscriber::send_audio(const std::vector<float> &pcm, size_t sentence_id, bool is_final)
{
	if (!m_connected.load()) {
		return;
	}
	if (!m_encoder) {
		blog(LOG_ERROR, "[RemoteTranscriber] send_audio: Opus encoder unavailable");
		return;
	}
	if (pcm.empty())
		return;

	// ── Incremental encoding cache management ─────────────────────────────────
	// If this is a new sentence, reset the encoder and the frame cache.
	if (sentence_id != m_cached_sentence_id) {
		opus_encoder_ctl(m_encoder, OPUS_RESET_STATE);
		m_encoded_frames_cache.clear();
		m_encoded_pcm_count = 0;
		m_cached_sentence_id = sentence_id;
		blog(LOG_DEBUG, "[RemoteTranscriber] New sentence_id=%zu, resetting Opus cache", sentence_id);
	}

	// ── Encode only the NEW PCM samples since last call ───────────────────────
	// m_encoded_pcm_count tracks how many samples have already been encoded.
	// We advance to the next complete FRAME_SIZE boundary from that offset.
	std::vector<uint8_t> frame_buf(MAX_PACKET);
	size_t offset = (m_encoded_pcm_count / FRAME_SIZE) * FRAME_SIZE; // align to frame boundary
	int frames_encoded = 0;

	while (offset + static_cast<size_t>(FRAME_SIZE) <= pcm.size()) {
		int bytes = opus_encode_float(m_encoder, pcm.data() + offset, FRAME_SIZE,
		                              frame_buf.data(), MAX_PACKET);
		if (bytes > 0) {
			auto len = static_cast<uint16_t>(bytes);
			m_encoded_frames_cache.push_back(static_cast<uint8_t>(len));
			m_encoded_frames_cache.push_back(static_cast<uint8_t>(len >> 8));
			m_encoded_frames_cache.insert(m_encoded_frames_cache.end(),
			                              frame_buf.begin(), frame_buf.begin() + bytes);
			++frames_encoded;
		} else if (bytes < 0) {
			blog(LOG_ERROR, "[RemoteTranscriber] Opus encode error: %s", opus_strerror(bytes));
		}
		offset += FRAME_SIZE;
	}
	m_encoded_pcm_count = offset; // remember up to which sample we've encoded

	// On final message: encode any remaining samples (padded to one full frame)
	if (is_final && offset < pcm.size()) {
		std::vector<float> padded_frame(FRAME_SIZE, 0.0f);
		std::copy(pcm.begin() + offset, pcm.end(), padded_frame.begin());
		int bytes = opus_encode_float(m_encoder, padded_frame.data(), FRAME_SIZE,
		                              frame_buf.data(), MAX_PACKET);
		if (bytes > 0) {
			auto len = static_cast<uint16_t>(bytes);
			m_encoded_frames_cache.push_back(static_cast<uint8_t>(len));
			m_encoded_frames_cache.push_back(static_cast<uint8_t>(len >> 8));
			m_encoded_frames_cache.insert(m_encoded_frames_cache.end(),
			                              frame_buf.begin(), frame_buf.begin() + bytes);
			++frames_encoded;
		} else if (bytes < 0) {
			blog(LOG_ERROR, "[RemoteTranscriber] Opus encode error (final pad): %s", opus_strerror(bytes));
		}
	}

	if (m_encoded_frames_cache.empty()) {
		blog(LOG_WARNING, "[RemoteTranscriber] No Opus frames in cache for segment %zu", sentence_id);
		return;
	}

	// ── Build binary message with header + ALL cached frames ──────────────────
	// The server needs the full audio context to transcribe accurately, but now
	// we build the message from the cache (already encoded) — no re-encoding.
	std::vector<uint8_t> message;
	message.reserve(5 + m_encoded_frames_cache.size());

	// Header: sentence_id (4 bytes, little-endian)
	auto sid = static_cast<uint32_t>(sentence_id);
	message.push_back(static_cast<uint8_t>(sid));
	message.push_back(static_cast<uint8_t>(sid >> 8));
	message.push_back(static_cast<uint8_t>(sid >> 16));
	message.push_back(static_cast<uint8_t>(sid >> 24));
	// Header: is_final (1 byte)
	message.push_back(is_final ? 1 : 0);

	// Body: cached Opus frames
	message.insert(message.end(), m_encoded_frames_cache.begin(), m_encoded_frames_cache.end());

	blog(LOG_DEBUG,
	     "[RemoteTranscriber] -> send_audio sid=%zu %s | new_frames=%d | total_cache=%zu bytes",
	     sentence_id, is_final ? "FINAL" : "PARTIAL", frames_encoded, m_encoded_frames_cache.size());

	// On final, clear the cache — this sentence is done.
	if (is_final) {
		m_encoded_frames_cache.clear();
		m_encoded_pcm_count = 0;
		m_cached_sentence_id = static_cast<size_t>(-1);
	}

	// ── Send to network thread via asio::post ─────────────────────────────────
	std::string payload(reinterpret_cast<const char *>(message.data()), message.size());

	asio::post(get_io_service(), [this, payload]() {
		if (!m_connected.load()) return;
		websocketpp::lib::error_code ec;
#ifdef HAVE_OPENSSL
		if (m_use_tls && m_client_tls)
			m_client_tls->send(m_hdl, payload, websocketpp::frame::opcode::binary, ec);
		else
#endif
		if (!m_use_tls && m_client_plain)
			m_client_plain->send(m_hdl, payload, websocketpp::frame::opcode::binary, ec);
		if (ec) {
			blog(LOG_ERROR, "[RemoteTranscriber] Send error: %s", ec.message().c_str());
		}
	});
}
