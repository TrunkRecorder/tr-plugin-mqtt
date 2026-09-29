# MQTT library: paho (MQTT 3.1.1 and 5) or Boost.MQTT5 (Boost 1.88+, MQTT 5 only).
# "auto" uses paho when installed, otherwise Boost.MQTT5.
set(MQTT_LIBRARY "auto" CACHE STRING "MQTT library for the MQTT Status plugin: auto, paho or boost")
set_property(CACHE MQTT_LIBRARY PROPERTY STRINGS auto paho boost)

find_package(eclipse-paho-mqtt-c QUIET)
find_package(PahoMqttCpp QUIET)
find_path(BOOST_MQTT5_INCLUDE_DIR boost/mqtt5.hpp HINTS ${Boost_INCLUDE_DIRS} ${Boost_INCLUDE_DIR})

if (MQTT_LIBRARY STREQUAL "paho" OR (MQTT_LIBRARY STREQUAL "auto" AND eclipse-paho-mqtt-c_FOUND AND PahoMqttCpp_FOUND))
  find_package(eclipse-paho-mqtt-c REQUIRED)
  find_package(PahoMqttCpp REQUIRED)
  set(MQTT_TRANSPORT mqtt_transport_paho.cc)
  set(MQTT_LIBRARIES eclipse-paho-mqtt-c::paho-mqtt3as PahoMqttCpp::paho-mqttpp3)
elseif (BOOST_MQTT5_INCLUDE_DIR AND (MQTT_LIBRARY STREQUAL "boost" OR MQTT_LIBRARY STREQUAL "auto"))
  set(MQTT_TRANSPORT mqtt_transport_boost.cc)
  set(MQTT_LIBRARIES "")
else()
  message(FATAL_ERROR "MQTT Status plugin: install the paho MQTT C++ library (libpaho-mqttpp-dev), or use Boost 1.88 or later")
endif()
message(STATUS "MQTT Status plugin: using ${MQTT_TRANSPORT}")

add_library(mqtt_status_plugin
MODULE
  mqtt_status_plugin.cc
  ${MQTT_TRANSPORT}
)

include_directories(
  ${CMAKE_BINARY_DIR}/../
)

 target_link_libraries(mqtt_status_plugin ${MQTT_LIBRARIES} trunk_recorder_library ssl crypto ${Boost_LIBRARIES} ${GNURADIO_PMT_LIBRARIES} ${GNURADIO_RUNTIME_LIBRARIES} ${GNURADIO_FILTER_LIBRARIES} ${GNURADIO_DIGITAL_LIBRARIES} ${GNURADIO_ANALOG_LIBRARIES} ${GNURADIO_AUDIO_LIBRARIES} ${GNURADIO_UHD_LIBRARIES} ${UHD_LIBRARIES} ${GNURADIO_BLOCKS_LIBRARIES} ${GNURADIO_OSMOSDR_LIBRARIES}  ${LIBOP25_REPEATER_LIBRARIES} gnuradio-op25_repeater) # gRPC::grpc++_reflection protobuf::libprotobuf)

 if(NOT Gnuradio_VERSION VERSION_LESS "3.8")

    target_link_libraries(mqtt_status_plugin
    gnuradio::gnuradio-analog
    gnuradio::gnuradio-blocks
    gnuradio::gnuradio-digital
    gnuradio::gnuradio-filter
    gnuradio::gnuradio-pmt
    ) 

endif()

install(TARGETS mqtt_status_plugin LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder)

