// Paho MQTT transport for the MQTT Status plugin
// ********************************
// Requires the Paho MQTT C and C++ libraries
// ********************************

#include "mqtt_transport.h"

#include <mqtt/async_client.h>

namespace
{

class Paho_Delivery : public Mqtt_Delivery
{
  mqtt::delivery_token_ptr token;

public:
  explicit Paho_Delivery(mqtt::delivery_token_ptr tok) : token(std::move(tok)) {}

  bool is_complete() override
  {
    return token->is_complete();
  }

  void wait_for(std::chrono::milliseconds timeout) override
  {
    // wait_for() throws if the delivery failed; callers only need to know it finished
    try
    {
      token->wait_for(timeout);
    }
    catch (const mqtt::exception &)
    {
    }
  }
};

class Paho_Transport : public Mqtt_Transport, public virtual mqtt::callback
{
  // client is declared last so it is destroyed first, before the events it calls
  Mqtt_Events events;
  mqtt::connect_options conn_opts;
  std::unique_ptr<mqtt::async_client> client;

public:
  std::string connect(const Mqtt_Settings &settings, const Mqtt_Events &ev) override
  {
    events = ev;

    auto will_msg = mqtt::message(settings.will_topic, settings.will_payload.c_str(), settings.will_payload.size(), settings.qos, true);

    // Set SSL options
    mqtt::ssl_options sslopts = mqtt::ssl_options_builder()
                                    .verify(false)
                                    .enable_server_cert_auth(false)
                                    .finalize();

    // Set connection options; kept for reconnect()
    conn_opts = mqtt::connect_options_builder()
                    .clean_session()
                    .ssl(sslopts)
                    .automatic_reconnect(std::chrono::seconds(10), std::chrono::seconds(40))
                    .will(will_msg)
                    .finalize();

    if (!settings.username.empty() && !settings.password.empty())
    {
      conn_opts.set_user_name(settings.username);
      conn_opts.set_password(settings.password);
    }

    // MQTT 5 replaces clean session with clean start
    if (settings.version == 5)
    {
      conn_opts.set_mqtt_version(MQTTVERSION_5);
      conn_opts.set_clean_start(true);
    }

    client.reset(new mqtt::async_client(settings.broker, settings.client_id, mqtt::create_options((settings.version == 5) ? MQTTVERSION_5 : MQTTVERSION_DEFAULT)));
    client->set_callback(*this);

    // MQTT 5 brokers report why they closed the connection
    if (settings.version == 5)
    {
      client->set_disconnected_handler([this](const mqtt::properties &, mqtt::ReasonCode reason)
                                       {
                                         if (events.broker_disconnect)
                                           events.broker_disconnect(mqtt::exception::reason_code_str(reason));
                                       });
    }

    try
    {
      client->connect(conn_opts)->wait();
    }
    catch (const mqtt::exception &exc)
    {
      return exc.what();
    }
    return "";
  }

  bool reconnect() override
  {
    try
    {
      client->connect(conn_opts);
      return true;
    }
    catch (const mqtt::exception &)
    {
      return false;
    }
  }

  Mqtt_Delivery_Ptr publish(const std::string &topic, const std::string &payload, int qos, bool retained) override
  {
    try
    {
      return std::make_shared<Paho_Delivery>(client->publish(mqtt::message_ptr_builder()
                                                                 .topic(topic)
                                                                 .payload(payload)
                                                                 .qos(qos)
                                                                 .retained(retained)
                                                                 .finalize()));
    }
    catch (const mqtt::exception &)
    {
      return nullptr;
    }
  }

  std::string disconnect(std::chrono::milliseconds timeout) override
  {
    try
    {
      client->disconnect()->wait_for(timeout);
    }
    catch (const mqtt::exception &exc)
    {
      return exc.what();
    }
    return "";
  }

  // Paho mqtt::callbacks, called on paho's thread
  void connected(const std::string &cause) override
  {
    if (events.connected)
      events.connected(cause);
  }

  void connection_lost(const std::string &cause) override
  {
    if (events.connection_lost)
      events.connection_lost(cause);
  }
};

} // namespace

const char *mqtt_transport_name()
{
  return "paho";
}

bool mqtt_transport_supports(int version)
{
  return (version == 3) || (version == 5);
}

std::unique_ptr<Mqtt_Transport> make_mqtt_transport()
{
  return std::unique_ptr<Mqtt_Transport>(new Paho_Transport());
}
