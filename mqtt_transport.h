// MQTT transport interface for the MQTT Status plugin
// ********************************
// The plugin queues and paces messages; a transport only connects and publishes.
// One implementation is compiled in, selected by CMakeLists.txt.
// ********************************

#ifndef MQTT_TRANSPORT_H
#define MQTT_TRANSPORT_H

#include <chrono>
#include <functional>
#include <memory>
#include <string>

struct Mqtt_Settings
{
  std::string broker;
  std::string client_id;
  std::string username;
  std::string password;
  int version; // 3 (3.1.1) or 5
  int qos;     // for the will message
  std::string will_topic;
  std::string will_payload;
};

// Delivery state of one published message
class Mqtt_Delivery
{
public:
  virtual ~Mqtt_Delivery() {}
  // Written to the socket (QoS 0) or acknowledged (QoS 1+), successfully or not
  virtual bool is_complete() = 0;
  // Block until complete or the timeout expires
  virtual void wait_for(std::chrono::milliseconds timeout) = 0;
};
typedef std::shared_ptr<Mqtt_Delivery> Mqtt_Delivery_Ptr;

// Connection events, called from the transport's thread
struct Mqtt_Events
{
  std::function<void(const std::string &cause)> connected;
  std::function<void(const std::string &cause)> connection_lost;
  std::function<void(const std::string &reason)> broker_disconnect; // MQTT 5 only
};

class Mqtt_Transport
{
public:
  virtual ~Mqtt_Transport() {}
  // Connect and wait for the first attempt; reconnects automatically after that.
  // Returns an empty string on success, otherwise the error.
  virtual std::string connect(const Mqtt_Settings &settings, const Mqtt_Events &events) = 0;
  // Start connecting again after disconnect(); returns false if the client is still busy
  virtual bool reconnect() = 0;
  // Returns nullptr if the message was refused
  virtual Mqtt_Delivery_Ptr publish(const std::string &topic, const std::string &payload, int qos, bool retained) = 0;
  // Close the connection, waiting up to timeout; no automatic reconnect follows.
  // Returns an empty string on success, otherwise the error.
  virtual std::string disconnect(std::chrono::milliseconds timeout) = 0;
};

// Provided by the compiled transport
const char *mqtt_transport_name();
std::unique_ptr<Mqtt_Transport> make_mqtt_transport();

#endif
