#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <utility>
#include <vector>
#include <mutex>
#include <optional>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <numeric>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <cmath>

// Boost & SSL
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <nlohmann/json.hpp>

// OpenSSL pro HMAC-SHA256
#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

// --- POMOCNÁ FUNKCE PRO HMAC-SHA256 PODPIS ---
std::string hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    HMAC(EVP_sha256(), key.c_str(), key.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         hash, &len);

    std::ostringstream os;
    for (unsigned int i = 0; i < len; i++) {
        os << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return os.str();
}

// Helper funkce pro správný zaokrouhlovací krok spotových měn na Binance
inline double adjust_qty_precision(const std::string& symbol, double raw_qty) {
    if (symbol == "BTCUSDC") {
        return std::floor(raw_qty * 100000.0) / 100000.0;
    } else if (symbol == "ETHUSDC") {
        return std::floor(raw_qty * 10000.0) / 10000.0;
    } else if (symbol == "SOLUSDC") {
        return std::floor(raw_qty * 100.0) / 100.0;
    } else if (symbol == "SUIUSDC") {
        return std::floor(raw_qty * 10.0) / 10.0;
    }
    return std::floor(raw_qty * 100.0) / 100.0;
}

// --- REST CLIENT OŠETŘENÝ PRO THREAD-SAFETY ---
class BinanceRestClient {
private:
    std::string api_key;
    std::string secret_key;
    std::string host = "api.binance.com";
    std::string port = "443";
    int64_t time_offset_ms = 0;
    std::mutex http_mtx;

public:
    BinanceRestClient(std::string key, std::string secret) 
        : api_key(std::move(key)), secret_key(std::move(secret)) {
        sync_time_with_binance();
    }

    void sync_time_with_binance() {
        try {
            asio::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            tcp::resolver resolver(ioc);
            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) return;

            auto const results = resolver.resolve(host, port);
            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(ssl::stream_base::client);

            http::request<http::string_body> req{http::verb::get, "/api/v3/time", 11};
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "C++ Binance MultiBot");

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            beast::error_code ec;
            stream.shutdown(ec);

            auto j = json::parse(res.body());
            if (j.contains("serverTime")) {
                int64_t server_time = j["serverTime"].get<int64_t>();
                int64_t local_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                time_offset_ms = server_time - local_time;
                std::cout << "[REST API] Cas synchronizovan s Binance. Offset: " 
                          << time_offset_ms << " ms" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[REST API WARN] Nelze synchronizovat cas: " << e.what() << std::endl;
        }
    }

    double get_free_balance(const std::string& asset) {
        std::lock_guard<std::mutex> lock(http_mtx);
        try {
            asio::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            tcp::resolver resolver(ioc);
            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) return 0.0;

            auto const results = resolver.resolve(host, port);
            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(ssl::stream_base::client);

            int64_t local_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            int64_t timestamp = local_time + time_offset_ms;

            std::string query_string = "timestamp=" + std::to_string(timestamp) + "&recvWindow=60000";
            std::string signature = hmac_sha256(secret_key, query_string);

            std::string target = "/api/v3/account?" + query_string + "&signature=" + signature;

            http::request<http::string_body> req{http::verb::get, target, 11};
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "C++ Binance MultiBot");
            req.set("X-MBX-APIKEY", api_key);

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            beast::error_code ec;
            stream.shutdown(ec);

            auto j = json::parse(res.body());
            if (j.contains("balances")) {
                for (const auto& b : j["balances"]) {
                    if (b["asset"] == asset) {
                        return std::stod(b["free"].get<std::string>());
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[BALANCE CHECK ERROR] " << e.what() << std::endl;
        }
        return 0.0;
    }

    bool send_order(const std::string& symbol, const std::string& side, double quantity) {
        std::lock_guard<std::mutex> lock(http_mtx);
        try {
            asio::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            tcp::resolver resolver(ioc);
            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) return false;

            auto const results = resolver.resolve(host, port);
            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(ssl::stream_base::client);

            int64_t local_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            int64_t timestamp = local_time + time_offset_ms;

            std::ostringstream query_ss;
            query_ss << "symbol=" << symbol
                     << "&side=" << side
                     << "&type=MARKET"
                     << "&quantity=" << std::fixed << std::setprecision(8) << quantity
                     << "&recvWindow=60000"
                     << "&timestamp=" << timestamp;

            std::string query_string = query_ss.str();
            std::string signature = hmac_sha256(secret_key, query_string);

            std::string target = "/api/v3/order?" + query_string + "&signature=" + signature;

            http::request<http::string_body> req{http::verb::post, target, 11};
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "C++ Binance MultiBot");
            req.set("X-MBX-APIKEY", api_key);
            req.set(http::field::content_type, "application/x-www-form-urlencoded");

            http::write(stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(stream, buffer, res);

            beast::error_code ec;
            stream.shutdown(ec);

            auto j = json::parse(res.body());
            if (j.contains("orderId")) {
                std::cout << "\n[REAL REST API] OBJEDNAVKA " << side << " " << symbol 
                          << " OK! Order ID: " << j["orderId"] << std::endl;
                return true;
            } else {
                std::cout << "\n[REST CHYBA] " << symbol << " Response: " << res.body() << std::endl;
                return false;
            }

        } catch (const std::exception& e) {
            std::cerr << "\n[REST EXCEPTION] " << e.what() << std::endl;
            return false;
        }
    }
};

// --- STRUKTURY PRO MARKT DATA & RING BUFFER ---
struct OrderBookSnapshot {
    std::string symbol;
    double best_bid_price;
    double best_bid_qty;
    double best_ask_price;
    double best_ask_qty;
    double total_bid_qty_l10;
    double total_ask_qty_l10;
    uint64_t timestamp;

    double get_raw_obi() const {
        double total_vol = total_bid_qty_l10 + total_ask_qty_l10;
        return (total_vol > 0.0) ? (total_bid_qty_l10 - total_ask_qty_l10) / total_vol : 0.0;
    }
};

template <typename T, size_t Capacity>
class RingBuffer {
private:
    std::vector<T> buffer;
    size_t head = 0;
    size_t tail = 0;
    size_t count = 0;

public:
    RingBuffer() : buffer(Capacity) {}

    void push(const T& item) {
        buffer[head] = item;
        head = (head + 1) % Capacity;
        if (count < Capacity) count++;
        else tail = (tail + 1) % Capacity;
    }

    std::optional<T> get_latest() const {
        if (count == 0) return std::nullopt;
        size_t latest_idx = (head == 0) ? Capacity - 1 : head - 1;
        return buffer[latest_idx];
    }

    double get_smoothed_obi(size_t window_size) const {
        if (count == 0) return 0.0;

        size_t samples = std::min(window_size, count);
        double sum_obi = 0.0;

        for (size_t i = 0; i < samples; ++i) {
            size_t idx = (head >= 1 + i) ? (head - 1 - i) : (Capacity + head - 1 - i);
            sum_obi += buffer[idx].get_raw_obi();
        }

        return sum_obi / static_cast<double>(samples);
    }
};

// Pomocná funkce pro zápis dokončeného obchodu do CSV
inline void log_trade_to_csv(const std::string& symbol, double entry, double exit, double qty, double pnl, const std::string& reason) {
    std::ofstream file("real_trades.csv", std::ios::app);
    if (file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        
        file << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S") << ","
             << symbol << ","
             << entry << ","
             << exit << ","
             << qty << ","
             << pnl << ","
             << reason << "\n";
        file.close();
    }
}

class OBIScalper {
private:
    std::string symbol;
    BinanceRestClient& rest;
    bool in_position = false;
    double entry_price = 0.0;
    double executed_qty = 0.0;
    
    // Scalping parametry
    const double obi_entry_threshold = 0.25;
    const double obi_exit_threshold  = -0.55; 
    const int    exit_confirm_ticks   = 10;    
    const double target_net_usd       = 0.50;  
    const double stop_loss_pct        = 0.008; 
    const double position_size_usd    = 50.0;  
    const int    cooldown_seconds     = 10;
    
    std::chrono::steady_clock::time_point last_trade_time;
    int signal_hold_ticks = 0;
    int drop_ticks_count = 0;              
    const int required_ticks = 15;         

public:
    OBIScalper(std::string sym, BinanceRestClient& client) 
        : symbol(std::move(sym)), rest(client) {
        last_trade_time = std::chrono::steady_clock::now() - std::chrono::seconds(cooldown_seconds);
    }

    void evaluate(double bid, double ask, double smoothed_obi) {
        auto now = std::chrono::steady_clock::now();

        // --- VÝSTUPNÍ LOGIKA (SELL) ---
        if (in_position) {
            double current_unrealized_pnl_usd = (bid - entry_price) * executed_qty;
            double pnl_pct = (bid - entry_price) / entry_price;
            
            bool tp_hit = (current_unrealized_pnl_usd >= target_net_usd);
            bool sl_hit = (pnl_pct <= -stop_loss_pct);

            bool obi_drop_triggered = false;
            if (smoothed_obi <= obi_exit_threshold) {
                drop_ticks_count++;
                if (drop_ticks_count >= exit_confirm_ticks) {
                    obi_drop_triggered = true;
                }
            } else {
                drop_ticks_count = 0;
            }

            if (tp_hit || sl_hit || obi_drop_triggered) {
                // Extrakce podkladového aktiva (např. "SUI" z "SUIUSDC")
                std::string base_asset = symbol.substr(0, symbol.length() - 4);
                double actual_free_qty = rest.get_free_balance(base_asset);

                // Prodej min. z toho, co eviduje bot vs. co je skutečně v peněžence
                double sell_qty = adjust_qty_precision(symbol, std::min(executed_qty, actual_free_qty));

                if (sell_qty <= 0.0) {
                    std::cerr << "\n[SELL ZRUŠEN] Nedostatek " << base_asset << " v peněžence!" << std::endl;
                    in_position = false; // Reset stavy
                    return;
                }

                if (rest.send_order(symbol, "SELL", sell_qty)) {
                    std::string exit_reason = "OBI DROP";
                    if (tp_hit) exit_reason = "TARGET USD (TP)";
                    else if (sl_hit) exit_reason = "STOP LOSS (SL)";

                    std::cout << "\n[SCALPER " << symbol << "] VÝSTUP -> PnL: $" 
                              << current_unrealized_pnl_usd << " | Důvod: " 
                              << exit_reason << std::endl;

                    log_trade_to_csv(symbol, entry_price, bid, sell_qty, current_unrealized_pnl_usd, exit_reason);

                    in_position = false;
                    drop_ticks_count = 0;
                    last_trade_time = now;
                }
            }
            return;
        }

        // --- VSTUPNÍ LOGIKA (BUY) ---
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_trade_time).count();
        if (elapsed < cooldown_seconds) return;

        if (smoothed_obi >= obi_entry_threshold) signal_hold_ticks++;
        else signal_hold_ticks = 0;

        if (signal_hold_ticks >= required_ticks) {
            signal_hold_ticks = 0;
            
            double free_usdc = rest.get_free_balance("USDC");
            if (free_usdc < 6.0) { 
                std::cout << "\n[BUY ZRUŠEN " << symbol << "] Nedostatek USDC v peněžence ($" << free_usdc << ")" << std::endl;
                return;
            }

            // Alokace $50 USD nebo maximum dostupného USDC
            double trade_alloc = std::min(position_size_usd, free_usdc - 0.5);
            double raw_qty = trade_alloc / ask;
            executed_qty = adjust_qty_precision(symbol, raw_qty);

            // Kontrola minimální notionální hodnoty pro Binance (min ~5.5 USDC)
            if (executed_qty * ask < 5.5) {
                executed_qty = adjust_qty_precision(symbol, 6.0 / ask);
            }

            if (rest.send_order(symbol, "BUY", executed_qty)) {
                entry_price = ask;
                in_position = true;
                drop_ticks_count = 0;
                std::cout << "\n[SCALPER " << symbol << "] VSTUP @ " << entry_price 
                          << " | Velikost pozice: " << executed_qty << " " << symbol 
                          << " (~" << (executed_qty * ask) << " USDC)" << std::endl;
            }
        }
    }

    bool has_position() const { return in_position; }
};

// --- GLOBÁLNÍ DATOVÉ STRUKTURY A THREAD-SAFETY MUTEX ---
std::unordered_map<std::string, RingBuffer<OrderBookSnapshot, 500>> g_buffers;
std::mutex g_buffers_mutex;

void websocket_worker() {
    while (true) {
        try {
            asio::io_context ioc;
            ssl::context ctx(ssl::context::tlsv12_client);
            ctx.set_default_verify_paths();

            tcp::resolver resolver(ioc);
            websocket::stream<ssl::stream<tcp::socket>> ws(ioc, ctx);

            std::string host = "stream.binance.com";
            std::string port = "9443";
            
            std::string target = "/stream?streams="
                                 "solusdc@depth10@100ms/"
                                 "suiusdc@depth10@100ms/"
                                 "btcusdc@depth10@100ms/"
                                 "ethusdc@depth10@100ms";

            auto const results = resolver.resolve(host, port);
            asio::connect(ws.next_layer().next_layer(), results.begin(), results.end());

            if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
                throw beast::system_error(
                    beast::error_code(static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category())
                );
            }

            ws.next_layer().handshake(ssl::stream_base::client);
            ws.handshake(host, target);

            std::cout << "\n[WS MULTI-STREAM] Připojeno k Binance WS pro SOL, SUI, BTC, ETH." << std::endl;

            beast::flat_buffer buffer;

            while (true) {
                buffer.consume(buffer.size());
                ws.read(buffer);

                std::string msg = beast::buffers_to_string(buffer.data());
                auto j = json::parse(msg);

                if (j.contains("stream") && j.contains("data")) {
                    std::string stream_name = j["stream"].get<std::string>();
                    auto data = j["data"];

                    std::string symbol = stream_name.substr(0, stream_name.find('@'));
                    std::transform(symbol.begin(), symbol.end(), symbol.begin(), ::toupper);

                    if (data.contains("bids") && data.contains("asks") && !data["bids"].empty() && !data["asks"].empty()) {
                        OrderBookSnapshot snap;
                        snap.symbol = symbol;
                        snap.best_bid_price = std::stod(data["bids"][0][0].get<std::string>());
                        snap.best_bid_qty   = std::stod(data["bids"][0][1].get<std::string>());
                        snap.best_ask_price = std::stod(data["asks"][0][0].get<std::string>());
                        snap.best_ask_qty   = std::stod(data["asks"][0][1].get<std::string>());

                        double sum_bids = 0.0, sum_asks = 0.0;
                        for (const auto& b : data["bids"]) sum_bids += std::stod(b[1].get<std::string>());
                        for (const auto& a : data["asks"]) sum_asks += std::stod(a[1].get<std::string>());

                        snap.total_bid_qty_l10 = sum_bids;
                        snap.total_ask_qty_l10 = sum_asks;

                        {
                            std::lock_guard<std::mutex> lock(g_buffers_mutex);
                            g_buffers[symbol].push(snap);
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "\n[WS ENGINE ERROR] Spojení přerušeno: " << e.what() << std::endl;
            std::cout << "[WS RECONNECT] Obnovuji připojení k Binance za 3 sekundy..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
}

// --- HLAVNÍ OBCHODNÍ VLÁKNO BOTA ---
void trading_bot_worker(std::string api_key, std::string secret_key) {
    BinanceRestClient rest(api_key, secret_key);

    std::vector<std::string> symbols = {"SOLUSDC", "SUIUSDC", "BTCUSDC", "ETHUSDC"};
    std::unordered_map<std::string, OBIScalper> scalpers;

    for (const auto& sym : symbols) {
        scalpers.emplace(sym, OBIScalper(sym, rest));
    }

    std::cout << "[TRADING BOT] Obchodní logika spuštěna. Sledované páry: 4." << std::endl;

    while (true) {
        for (const auto& sym : symbols) {
            OrderBookSnapshot snap;
            double smoothed_obi = 0.0;
            bool has_data = false;

            {
                std::lock_guard<std::mutex> lock(g_buffers_mutex);
                if (g_buffers.count(sym)) {
                    auto latest = g_buffers[sym].get_latest();
                    if (latest.has_value()) {
                        snap = latest.value();
                        smoothed_obi = g_buffers[sym].get_smoothed_obi(10);
                        has_data = true;
                    }
                }
            }

            if (has_data) {
                scalpers.at(sym).evaluate(snap.best_bid_price, snap.best_ask_price, smoothed_obi);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int main() {
    std::string API_KEY = "1PXDfw5zhozze63yUcg98DRHoMDBA4gB8sZJZwhYU2zpbaRQRfo87I4EjKmHxddz";
    std::string SECRET_KEY = "st7Y9AWEyk4EKEcX8QcRoYqh4rd4P9r6CBTaMazaYHLGJo5TLEl1TRLDq7ixn0Ap";

    if (API_KEY == "TVUJ_API_KEY" || SECRET_KEY == "TVUJ_SECRET_KEY") {
        std::cerr << "[CHYBA] Nezapomeň vložit své vygenerované API klíče do proměnných v main()!" << std::endl;
        return 1;
    }

    std::thread ws_thread(websocket_worker);
    std::thread bot_thread(trading_bot_worker, API_KEY, SECRET_KEY);

    ws_thread.join();
    bot_thread.join();

    return 0;
}