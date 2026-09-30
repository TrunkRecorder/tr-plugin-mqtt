// Boost.MQTT5 transport for the MQTT Status plugin
// ********************************
// Requires Boost 1.88 or later. MQTT 5 only.
// WebSocket brokers (ws://, wss://) need MQTT_BOOST_WEBSOCKET; it roughly doubles compile RAM.
// ********************************

#include "mqtt_transport.h"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/mqtt5/mqtt_client.hpp>
#include <boost/mqtt5/reason_codes.hpp>
#include <boost/mqtt5/ssl.hpp>
#include <boost/mqtt5/types.hpp>

#ifdef MQTT_BOOST_WEBSOCKET
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/mqtt5/websocket.hpp>
#include <boost/mqtt5/websocket_ssl.hpp>
#endif

#include <openssl/ssl.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <variant>

namespace asio = boost::asio;
namespace mqtt5 = boost::mqtt5;

using tcp_stream = asio::ip::tcp::socket;
using tls_stream = asio::ssl::stream<tcp_stream>;

// Boost.MQTT5 needs these to run TLS handshakes over asio::ssl
namespace boost::mqtt5
{
template <>
struct tls_handshake_type<tls_stream>
{
  static constexpr auto client = asio::ssl::stream_base::client;
  static constexpr auto server = asio::ssl::stream_base::server;
};

template <>
void assign_tls_sni(const authority_path &ap, asio::ssl::context &, tls_stream &stream)
{
  SSL_set_tlsext_host_name(stream.native_handle(), ap.host.c_str());
}
} // namespace boost::mqtt5

namespace
{

class Boost_Delivery : public Mqtt_Delivery
{
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;

public:
  void complete()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      done = true;
    }
    cv.notify_all();
  }

  bool is_complete() override
  {
    std::lock_guard<std::mutex> lock(mutex);
    return done;
  }

  void wait_for(std::chrono::milliseconds timeout) override
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, timeout, [this] { return done; });
  }
};

class Boost_Transport;

// Boost.MQTT5 has no connection callbacks; its logger hooks are the only connection events
struct Connection_Hooks
{
  Boost_Transport *owner;

  void at_resolve(mqtt5::error_code ec, std::string_view, std::string_view, const asio::ip::tcp::resolver::results_type &);
  void at_tcp_connect(mqtt5::error_code ec, asio::ip::tcp::endpoint);
  void at_tls_handshake(mqtt5::error_code ec, asio::ip::tcp::endpoint);
  void at_ws_handshake(mqtt5::error_code ec, asio::ip::tcp::endpoint);
  void at_connack(mqtt5::reason_code rc, bool, const mqtt5::connack_props &);
  void at_disconnect(mqtt5::reason_code rc, const mqtt5::disconnect_props &);
};

mqtt5::qos_e qos_of(int qos)
{
  if (qos <= 0)
    return mqtt5::qos_e::at_most_once;
  return (qos == 1) ? mqtt5::qos_e::at_least_once : mqtt5::qos_e::exactly_once;
}

// One interface over the four client types (plain or TLS, raw or WebSocket)
class Client
{
public:
  virtual ~Client() {}
  virtual void run(const Mqtt_Settings &settings, const std::string &hosts, uint16_t port, std::function<void(mqtt5::error_code)> ended) = 0;
  virtual void publish(std::string topic, std::string payload, int qos, bool retained, std::shared_ptr<Boost_Delivery> delivery) = 0;
  virtual void disconnect(std::function<void()> done) = 0;
  virtual void cancel() = 0;
};

template <typename Stream, typename Tls>
class Client_Of : public Client
{
  mqtt5::mqtt_client<Stream, Tls, Connection_Hooks> client;

public:
  Client_Of(asio::io_context &ioc, Tls tls, Connection_Hooks hooks) : client(ioc, std::move(tls), hooks) {}

  void run(const Mqtt_Settings &settings, const std::string &hosts, uint16_t port, std::function<void(mqtt5::error_code)> ended) override
  {
    bool login = !settings.username.empty() && !settings.password.empty();
    client.brokers(hosts, port)
        .credentials(settings.client_id, login ? settings.username : "", login ? settings.password : "")
        .will(mqtt5::will(settings.will_topic, settings.will_payload, qos_of(settings.qos), mqtt5::retain_e::yes))
        .async_run(std::move(ended));
  }

  void publish(std::string topic, std::string payload, int qos, bool retained, std::shared_ptr<Boost_Delivery> delivery) override
  {
    mqtt5::retain_e retain = retained ? mqtt5::retain_e::yes : mqtt5::retain_e::no;
    // Completion handlers are moved into the operation, so each call gets its own
    auto done = [delivery]() { return [delivery](auto...) { delivery->complete(); }; };
    if (qos <= 0)
      client.template async_publish<mqtt5::qos_e::at_most_once>(std::move(topic), std::move(payload), retain, mqtt5::publish_props{}, done());
    else if (qos == 1)
      client.template async_publish<mqtt5::qos_e::at_least_once>(std::move(topic), std::move(payload), retain, mqtt5::publish_props{}, done());
    else
      client.template async_publish<mqtt5::qos_e::exactly_once>(std::move(topic), std::move(payload), retain, mqtt5::publish_props{}, done());
  }

  void disconnect(std::function<void()> done) override
  {
    client.async_disconnect([done](mqtt5::error_code) { done(); });
  }

  void cancel() override
  {
    client.cancel();
  }
};

class Boost_Transport : public Mqtt_Transport
{
  // io_context is declared first so it outlives the timer and client that use it
  asio::io_context ioc;
  asio::executor_work_guard<asio::io_context::executor_type> work;
  asio::steady_timer restart_timer;
  std::unique_ptr<Client> client;
  std::thread io_thread;

  Mqtt_Settings settings;
  Mqtt_Events events;
  std::string hosts;
  uint16_t default_port = 1883;

  // io thread only
  bool connected = false;
  bool ever_connected = false;
  bool stopping = false;
  unsigned run_generation = 0;

  std::atomic<bool> disconnecting{false};

  // Result of the first connection attempt, for connect(); guarded by first_mutex
  std::mutex first_mutex;
  std::condition_variable first_cv;
  bool first_done = false;
  std::string first_error;
  std::string last_error;

  // io_context shutdown, for the destructor
  std::mutex io_mutex;
  std::condition_variable io_cv;
  bool io_done = false;

  friend struct Connection_Hooks;

  // Build the client for the broker URI: tcp:// mqtt:// ssl:// mqtts:// ws:// wss://
  std::string make_client()
  {
    std::string uri = settings.broker;
    std::string scheme = "tcp";
    size_t sep = uri.find("://");
    if (sep != std::string::npos)
    {
      scheme = uri.substr(0, sep);
      uri = uri.substr(sep + 3);
    }

    bool tls = (scheme == "ssl") || (scheme == "mqtts") || (scheme == "wss");
    bool ws = (scheme == "ws") || (scheme == "wss");
    if (!tls && !ws && (scheme != "tcp") && (scheme != "mqtt"))
      return "Unsupported broker URI: " + settings.broker;

    hosts = uri;
    default_port = ws ? (tls ? 443 : 80) : (tls ? 8883 : 1883);

    Connection_Hooks hooks{this};
#ifdef MQTT_BOOST_WEBSOCKET
    if (ws && tls)
      client.reset(new Client_Of<boost::beast::websocket::stream<tls_stream>, asio::ssl::context>(ioc, tls_context(), hooks));
    else if (ws)
      client.reset(new Client_Of<boost::beast::websocket::stream<tcp_stream>, std::monostate>(ioc, {}, hooks));
#else
    if (ws)
      return "WebSocket brokers need the plugin built with -DMQTT_BOOST_WEBSOCKET=ON: " + settings.broker;
#endif
    else if (tls)
      client.reset(new Client_Of<tls_stream, asio::ssl::context>(ioc, tls_context(), hooks));
    else
      client.reset(new Client_Of<tcp_stream, std::monostate>(ioc, {}, hooks));
    return "";
  }

  // Matches the paho transport: encrypt, but do not verify the broker's certificate
  static asio::ssl::context tls_context()
  {
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    ctx.set_verify_mode(asio::ssl::verify_none);
    return ctx;
  }

  // io thread
  void start()
  {
    if (!client)
      return;
    stopping = false;
    unsigned generation = ++run_generation;
    client->run(settings, hosts, default_port, [this, generation](mqtt5::error_code) { run_ended(generation); });
  }

  // async_run() only ends by itself on a non-recoverable error (e.g. not authorized); retry like paho
  void run_ended(unsigned generation)
  {
    if (stopping || (generation != run_generation))
      return;
    lost();
    finish_first(error_or("Connection refused"));
    restart_timer.expires_after(std::chrono::seconds(10));
    restart_timer.async_wait([this](mqtt5::error_code ec)
                             {
                               if (!ec && !stopping)
                                 start();
                             });
  }

  void set_error(const std::string &error)
  {
    std::lock_guard<std::mutex> lock(first_mutex);
    last_error = error;
  }

  std::string error_or(const std::string &fallback)
  {
    std::lock_guard<std::mutex> lock(first_mutex);
    return last_error.empty() ? fallback : last_error;
  }

  void finish_first(const std::string &error)
  {
    {
      std::lock_guard<std::mutex> lock(first_mutex);
      if (first_done)
        return;
      first_done = true;
      first_error = error;
    }
    first_cv.notify_all();
  }

  void lost()
  {
    if (!connected)
      return;
    connected = false;
    if (!stopping && events.connection_lost)
      events.connection_lost("");
  }

  // Each connection step; a new attempt while connected means the connection was lost
  void attempt(mqtt5::error_code ec)
  {
    lost();
    if (ec)
      set_error(ec.message());
  }

  void connack(mqtt5::reason_code rc)
  {
    if (rc)
    {
      set_error(rc.message());
      finish_first(rc.message());
      return;
    }
    connected = true;
    if (!stopping && events.connected)
      events.connected(ever_connected ? "automatic reconnect" : "");
    ever_connected = true;
    finish_first("");
  }

  void broker_disconnect(mqtt5::reason_code rc)
  {
    if (!connected)
      return;
    connected = false;
    if (!stopping && events.broker_disconnect)
      events.broker_disconnect(rc.message());
  }

public:
  Boost_Transport() : work(asio::make_work_guard(ioc)), restart_timer(ioc)
  {
    io_thread = std::thread([this]
                            {
                              ioc.run();
                              std::lock_guard<std::mutex> lock(io_mutex);
                              io_done = true;
                              io_cv.notify_all();
                            });
  }

  ~Boost_Transport()
  {
    asio::post(ioc, [this]
               {
                 stopping = true;
                 restart_timer.cancel();
                 if (client)
                   client->cancel();
                 work.reset();
               });
    {
      // Cancelling ends all client operations; stop the loop outright if something lingers
      std::unique_lock<std::mutex> lock(io_mutex);
      if (!io_cv.wait_for(lock, std::chrono::seconds(3), [this] { return io_done; }))
        ioc.stop();
    }
    io_thread.join();
    client.reset();
  }

  std::string connect(const Mqtt_Settings &s, const Mqtt_Events &ev) override
  {
    settings = s;
    events = ev;
    std::string error = make_client();
    if (!error.empty())
      return error;

    asio::post(ioc, [this] { start(); });

    // Boost.MQTT5 retries on its own and does not report a failed first attempt, so wait briefly
    std::unique_lock<std::mutex> lock(first_mutex);
    if (first_cv.wait_for(lock, std::chrono::seconds(5), [this] { return first_done; }))
      return first_error;
    return last_error.empty() ? "Timed out waiting for the broker" : last_error;
  }

  bool reconnect() override
  {
    if (disconnecting)
      return false;
    asio::post(ioc, [this] { start(); });
    return true;
  }

  Mqtt_Delivery_Ptr publish(const std::string &topic, const std::string &payload, int qos, bool retained) override
  {
    std::shared_ptr<Boost_Delivery> delivery = std::make_shared<Boost_Delivery>();
    asio::post(ioc, [this, delivery, topic, payload, qos, retained]() mutable
               {
                 if (stopping || !client)
                   delivery->complete();
                 else
                   client->publish(std::move(topic), std::move(payload), qos, retained, delivery);
               });
    return delivery;
  }

  std::string disconnect(std::chrono::milliseconds timeout) override
  {
    std::shared_ptr<Boost_Delivery> done = std::make_shared<Boost_Delivery>();
    disconnecting = true;
    asio::post(ioc, [this, done]
               {
                 stopping = true;
                 connected = false;
                 restart_timer.cancel();
                 if (!client)
                 {
                   disconnecting = false;
                   done->complete();
                   return;
                 }
                 client->disconnect([this, done]
                                    {
                                      disconnecting = false;
                                      done->complete();
                                    });
               });
    done->wait_for(timeout);
    return done->is_complete() ? "" : "Timed out closing the connection";
  }
};

void Connection_Hooks::at_resolve(mqtt5::error_code ec, std::string_view, std::string_view, const asio::ip::tcp::resolver::results_type &) { owner->attempt(ec); }
void Connection_Hooks::at_tcp_connect(mqtt5::error_code ec, asio::ip::tcp::endpoint) { owner->attempt(ec); }
void Connection_Hooks::at_tls_handshake(mqtt5::error_code ec, asio::ip::tcp::endpoint) { owner->attempt(ec); }
void Connection_Hooks::at_ws_handshake(mqtt5::error_code ec, asio::ip::tcp::endpoint) { owner->attempt(ec); }
void Connection_Hooks::at_connack(mqtt5::reason_code rc, bool, const mqtt5::connack_props &) { owner->connack(rc); }
void Connection_Hooks::at_disconnect(mqtt5::reason_code rc, const mqtt5::disconnect_props &) { owner->broker_disconnect(rc); }

} // namespace

const char *mqtt_transport_name()
{
  return "boost";
}

bool mqtt_transport_supports(int version)
{
  return version == 5;
}

std::unique_ptr<Mqtt_Transport> make_mqtt_transport()
{
  return std::unique_ptr<Mqtt_Transport>(new Boost_Transport());
}
