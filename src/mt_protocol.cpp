#include "mt_internals.h"

// Magic number at the start of all MT packets
#define MT_MAGIC_0 0x94
#define MT_MAGIC_1 0xc3

// The header is the magic number plus a 16-bit payload-length field
#define MT_HEADER_SIZE 4

// The buffer used for protobuf encoding/decoding. Since there's only one, and it's global, we
// have to make sure we're only ever doing one encoding or decoding at a time.
#define PB_BUFSIZE 512
pb_byte_t pb_buf[PB_BUFSIZE+4];
size_t pb_size = 0; // Number of bytes currently in the buffer

// Nonce to request only my nodeinfo and skip other nodes in the db
#define SPECIAL_NONCE 69420

// Wait this many msec if there's nothing new on the channel
#define NO_NEWS_PAUSE 25

// Serial connections require at least one ping every 15 minutes
// Otherwise the connection is closed, and packets will no longer be received
// We will send a ping every 60 seconds, which is what the web client does
// https://github.com/meshtastic/js/blob/715e35d2374276a43ffa93c628e3710875d43907/src/adapters/serialConnection.ts#L160
#define HEARTBEAT_INTERVAL_MS 60000
uint32_t last_heartbeat_at = 0;

// The ID of the current WANT_CONFIG request
uint32_t want_config_id = 0;

// Node number of the MT node hosting our WiFi
uint32_t my_node_num = 0;

void (*text_message_callback)(uint32_t from, uint32_t to,  uint8_t channel, const char* text) = NULL;
void (*portnum_callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_PortNum port, meshtastic_Data_payload_t *payload) = NULL;
void (*encrypted_callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_MeshPacket_public_key_t pubKey, meshtastic_MeshPacket_encrypted_t *enc_payload) = NULL;

void (*node_report_callback)(mt_node_t *, mt_nr_progress_t) = NULL;
mt_node_t node;

bool mt_wifi_mode = false;
bool mt_serial_mode = false;

#define VA_BUFSIZE 512

bool mt_send_radio(const char * buf, size_t len) {
  if (mt_wifi_mode) {
    #ifdef MT_WIFI_SUPPORTED
    return mt_wifi_send_radio(buf, len);
    #else
    return false;
    #endif
  } else if (mt_serial_mode) {
    return mt_serial_send_radio(buf, len);
  } else {
    Serial.println("mt_send_radio() called but it was never initialized");
    while(1);
  }
}

bool _mt_send_toRadio(meshtastic_ToRadio toRadio) {
  pb_buf[0] = MT_MAGIC_0;
  pb_buf[1] = MT_MAGIC_1;

  pb_ostream_t stream = pb_ostream_from_buffer(pb_buf + 4, PB_BUFSIZE);
  bool status = pb_encode(&stream, meshtastic_ToRadio_fields, &toRadio);
  if (!status) {
    // d("Couldn't encode toRadio");
    return false;
  }

  // Store the payload length in the header
  pb_buf[2] = stream.bytes_written / 256;
  pb_buf[3] = stream.bytes_written % 256;

  bool rv = mt_send_radio((const char *)pb_buf, 4 + stream.bytes_written);

  // Clear the buffer so it can be used to hold reply packets
  pb_size = 0;

  return rv;
}

// Request a node report from our MT
bool mt_request_node_report(void (*callback)(mt_node_t *, mt_nr_progress_t)) {
  meshtastic_ToRadio toRadio = meshtastic_ToRadio_init_default;
  toRadio.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
  want_config_id = random(0x7FffFFff);  // random() can't handle anything bigger
  toRadio.want_config_id = want_config_id;

#ifdef MT_DEBUGGING
  Serial.print("Requesting node report with random ID ");
  Serial.println(want_config_id);
#endif

  bool rv = _mt_send_toRadio(toRadio);

  if (rv) node_report_callback = callback;
  return rv;
}

bool mt_send_text(const char * text, uint32_t dest, uint8_t channel_index) {
  meshtastic_MeshPacket meshPacket = meshtastic_MeshPacket_init_default;
  meshPacket.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  meshPacket.id = random(0x7FFFFFFF);
  meshPacket.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  meshPacket.to = dest;
  meshPacket.channel = channel_index;
  meshPacket.want_ack = true;
  meshPacket.decoded.payload.size = strlen(text);
  memcpy(meshPacket.decoded.payload.bytes, text, meshPacket.decoded.payload.size);

  meshtastic_ToRadio toRadio = meshtastic_ToRadio_init_default;
  toRadio.which_payload_variant = meshtastic_ToRadio_packet_tag;
  toRadio.packet = meshPacket;
  
  Serial.print("Sending text message '");
  Serial.print(text);
  Serial.print("' to ");
  Serial.println(dest);
  return _mt_send_toRadio(toRadio);
}

bool mt_send_heartbeat() {

  // d("Sending heartbeat");

  meshtastic_ToRadio toRadio = meshtastic_ToRadio_init_default;
  toRadio.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
  toRadio.heartbeat = meshtastic_Heartbeat_init_default;

  return _mt_send_toRadio(toRadio);
}

void set_portnum_callback(void (*callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_PortNum port, meshtastic_Data_payload_t *payload)) {
  portnum_callback = callback;
}

void set_encrypted_callback(void (*callback)(uint32_t from, uint32_t to,  uint8_t channel, meshtastic_MeshPacket_public_key_t pubKey, meshtastic_MeshPacket_encrypted_t *payload)) {
  encrypted_callback = callback;
}

void set_text_message_callback(void (*callback)(uint32_t from, uint32_t to,  uint8_t channel, const char* text)) {
  text_message_callback = callback;
}

bool handle_id_tag(uint32_t id) {
  // d("id_tag: ID: %d\r\n", id);
  return true;
}

bool handle_config_tag(meshtastic_Config *config) {
  switch (config->which_payload_variant) {
    case meshtastic_Config_device_tag:
      // d("Config:device_tag:  role: %d\r\n", config->payload_variant.device.role);
      // d("Config:device_tag:  serial enabled: %d\r\n", config->payload_variant.device.serial_enabled);
      // d("Config:device_tag:  button gpio: %d\r\n", config->payload_variant.device.buzzer_gpio);
      // d("Config:device_tag:  rebroadcast mode: %d\r\n", config->payload_variant.device.rebroadcast_mode);
      // d("Config:device_tag:  node_info_broadcast_secs: %d\r\n", config->payload_variant.device.node_info_broadcast_secs);
      // d("Config:device_tag:  double-tap-as-button-press: %d\r\n", config->payload_variant.device.double_tap_as_button_press);
      // d("Config:device_tag:  is_managed: %d\r\n", config->payload_variant.device.is_managed);
      // d("Config:device_tag:  disable_triple_click: %d\r\n", config->payload_variant.device.disable_triple_click);
      // d("Config:device_tag:  tz_def: %s\r\n", config->payload_variant.device.tzdef);
      // d("Config:device_tag:  led_heartbeat_disabled: %d\r\n", config->payload_variant.device.led_heartbeat_disabled);
      break;

    case meshtastic_Config_position_tag:
      // d("Config:position_tag:  position_broadcast_secs: %d\r\n", config->payload_variant.position.position_broadcast_secs);
      // d("Config:position_tag:  position_broadcast_smart_enabled: %d\r\n", config->payload_variant.position.position_broadcast_smart_enabled);
      // d("Config:position_tag:  fixed_position: %d\r\n", config->payload_variant.position.fixed_position);
      // d("Config:position_tag:  gps_enabled: %d\r\n", config->payload_variant.position.gps_enabled);
      // d("Config:position_tag:  gps_update_interval: %d\r\n", config->payload_variant.position.gps_update_interval);
      // d("Config:position_tag:  gps_attempt_time: %d\r\n", config->payload_variant.position.gps_attempt_time);
      // d("Config:position_tag:  position_flags: %d\r\n", config->payload_variant.position.position_flags);
      // d("Config:position_tag:  rx_gpio: %d\r\n", config->payload_variant.position.rx_gpio);
      // d("Config:position_tag:  tx_gpio: %d\r\n", config->payload_variant.position.tx_gpio);
      // d("Config:position_tag:  broadcast_smart_min_distance: %d\r\n", config->payload_variant.position.broadcast_smart_minimum_distance);
      // d("Config:position_tag:  broadcast_smart_min_interval_secs: %d\r\n", config->payload_variant.position.broadcast_smart_minimum_interval_secs);
      // d("Config:position_tag:  gps_en_gpio: %d\r\n", config->payload_variant.position.gps_en_gpio);
      // d("Config:position_tag:  gps_mode %d\r\n", config->payload_variant.position.gps_mode);
      break;

    case meshtastic_Config_power_tag: 
      // d("Config:power_tag:  is_power_saving %d\r\n", config->payload_variant.power.is_power_saving);
      // d("Config:power_tag:  on_battery_shutdown_after_secs %d\r\n", config->payload_variant.power.on_battery_shutdown_after_secs);
      // d("Config:power_tag:  adv_multiplier_override %f\r\n", config->payload_variant.power.adc_multiplier_override);
      // d("Config:power_tag:  wait_bluetooth_secs %d\r\n", config->payload_variant.power.wait_bluetooth_secs);
      // d("Config:power_tag:  sds_secs %d\r\n", config->payload_variant.power.sds_secs);
      // d("Config:power_tag:  ls_secs %d\r\n", config->payload_variant.power.ls_secs);
      // d("Config:power_tag:  min_wake_secs %d\r\n", config->payload_variant.power.min_wake_secs);
      // d("Config:power_tag:  device_battery_ina_aaddr %d\r\n", config->payload_variant.power.device_battery_ina_address);
      // d("Config:power_tag:  powermon_enables %d\r\n", config->payload_variant.power.powermon_enables);
      break;

    case meshtastic_Config_network_tag:
      // d("Config:network_tag:wifi_enabled: %d  \r\n", config->payload_variant.network.wifi_enabled);
      // d("Config:network_tag:wifi_ssid: %s  \r\n", config->payload_variant.network.wifi_ssid);
      // d("Config:network_tag:wifi_psk: %s  \r\n", config->payload_variant.network.wifi_psk);
      // d("Config:network_tag:ntp_server: %s  \r\n", config->payload_variant.network.ntp_server);
      // d("Config:network_tag:eth_enabled: %d  \r\n", config->payload_variant.network.eth_enabled);
      // d("Config:network_tag:addr_mode: %d  \r\n", config->payload_variant.network.address_mode);
      // d("Config:network_tag:has_ipv4_config: %d  \r\n", config->payload_variant.network.has_ipv4_config);
      // d("Config:network_tag:ipv4_config: %d  \r\n", config->payload_variant.network.ipv4_config);
      // d("Config:network_tag:rsyslog_server: %s  \r\n", config->payload_variant.network.rsyslog_server);
      break;

    case meshtastic_Config_display_tag: 
      // d("Config:display_tag:screen_on_seconds: %d  \r\n", config->payload_variant.display.screen_on_secs);
      // d("Config:display_tag:gps_format: %d  \r\n", config->payload_variant.display.gps_format);
      // d("Config:display_tag:auto_screen_carousel_secs: %d  \r\n", config->payload_variant.display.auto_screen_carousel_secs);
      // d("Config:display_tag:compass_north_top: %d  \r\n", config->payload_variant.display.compass_north_top);
      // d("Config:display_tag:flip_screen: %d  \r\n", config->payload_variant.display.flip_screen);
      // d("Config:display_tag:units: %d  \r\n", config->payload_variant.display.units);
      // d("Config:display_tag:oled: %d  \r\n", config->payload_variant.display.oled);
      // d("Config:display_tag:displayMode: %d  \r\n", config->payload_variant.display.displaymode);
      // d("Config:display_tag:heading_bold: %d  \r\n", config->payload_variant.display.heading_bold);
      // d("Config:display_tag:wake_on_tap_or_motion: %d\r\n", config->payload_variant.display.wake_on_tap_or_motion);
      // d("Config:display_tag:compass_orientation: %d\r\n", config->payload_variant.display.compass_orientation);
      break;

    case meshtastic_Config_lora_tag:
      // d("Config:lora_tag:use_preset: %d  \r\n", config->payload_variant.lora.use_preset);
      // d("Config:lora_tag:modem_preset: %d  \r\n", config->payload_variant.lora.modem_preset);
      // d("Config:lora_tag:bandwidth: %d  \r\n", config->payload_variant.lora.bandwidth);
      // d("Config:lora_tag:spread_factor: %d  \r\n", config->payload_variant.lora.spread_factor);
      // d("Config:lora_tag:coding_rate: %d  \r\n", config->payload_variant.lora.coding_rate);
      // d("Config:lora_tag:frequency_offset: %d\r\n", config->payload_variant.lora.frequency_offset);
      // d("Config:lora_tag:region: %d  \r\n", config->payload_variant.lora.region);
      // d("Config:lora_tag:hot_limit: %d  \r\n", config->payload_variant.lora.hop_limit);
      // d("Config:lora_tag:tx_enabled: %d  \r\n", config->payload_variant.lora.tx_enabled);
      // d("Config:lora_tag:tx_power: %d  \r\n", config->payload_variant.lora.tx_power);
      // d("Config:lora_tag:channel_num: %d  \r\n", config->payload_variant.lora.channel_num);
      // d("Config:lora_tag:override_duty_cycle: %d\r\n", config->payload_variant.lora.override_duty_cycle);
      // d("Config:lora_tag:sx126x_rx_boosted_gain: %d\r\n", config->payload_variant.lora.sx126x_rx_boosted_gain);
      // d("Config:lora_tag:override_frequency: %d\r\n", config->payload_variant.lora.override_frequency);
      // d("Config:lora_tag:pa_fan_disabled: %d\r\n", config->payload_variant.lora.pa_fan_disabled);
      // d("Config:lora_tag:ignore_incoming_count: %d\r\n", config->payload_variant.lora.ignore_incoming_count);
      // d("Config:lora_tag:ignore_mqtt: %d\r\n", config->payload_variant.lora.ignore_mqtt);
      // d("Config:lora_tag:config_okay_to_mqtt: %d\r\n", config->payload_variant.lora.config_ok_to_mqtt);
      break;

    case meshtastic_Config_bluetooth_tag: 
      // d("Config:bluetooth_tag:enabled: %d  \r\n", config->payload_variant.bluetooth.enabled);
      // d("Config:bluetooth_tag:fixed_pin: %d  \r\n", config->payload_variant.bluetooth.fixed_pin);
      // d("Config:bluetooth_tag:mode: %d  \r\n", config->payload_variant.bluetooth.mode);
      break;

    case meshtastic_Config_security_tag: 
      // d("Config:security_tag:is_managed: %d \r\n", config->payload_variant.security.is_managed);
      // d("Config:security_tag:public_key: %x \r\n", config->payload_variant.security.public_key);
      // d("Config:security_tag:private_key: %x \r\n", config->payload_variant.security.private_key);
      // d("Config:security_tag:admin_key_count: %x \r\n", config->payload_variant.security.admin_key_count);
      // d("Config:security_tag:serial_enabled: %x \r\n", config->payload_variant.security.serial_enabled);
      // d("Config:security_tag:debug_log_api_enabled: %x \r\n", config->payload_variant.security.debug_log_api_enabled);
      // d("Config:security_tag:admin_channel_enabled: %x \r\n", config->payload_variant.security.admin_channel_enabled);
      break;

    case meshtastic_Config_sessionkey_tag: 
      // d("Config:sessionkey_tag:dummy_field: %x \r\n", config->payload_variant.sessionkey.dummy_field);
      break;

    case meshtastic_Config_device_ui_tag:
      // d("Config.device_ui:alert_enabled: %d\r\n", config->payload_variant.device_ui.alert_enabled);
      // d("Config.device_ui:banner_enabled: %d\r\n", config->payload_variant.device_ui.banner_enabled);
      // d("Config.device_ui:has_node_filter: %d\r\n", config->payload_variant.device_ui.has_node_filter);
      // d("Config.device_ui:has_node_highlight: %d\r\n", config->payload_variant.device_ui.has_node_highlight);
      // d("Config.device_ui:language: %d\r\n", config->payload_variant.device_ui.language);
      // d("Config.device_ui:node_filter: %d\r\n", config->payload_variant.device_ui.node_filter);
      // d("Config.device_ui:node_highlight: %d\r\n", config->payload_variant.device_ui.node_highlight);
      // d("Config.device_ui:pin_code: %d\r\n", config->payload_variant.device_ui.pin_code);
      // d("Config.device_ui:ring_tone_id: %d\r\n", config->payload_variant.device_ui.ring_tone_id);
      // d("Config.device_ui:screen_brightness: %d\r\n", config->payload_variant.device_ui.screen_brightness);
      // d("Config.device_ui:screen_lock: %d\r\n", config->payload_variant.device_ui.screen_lock);
      // d("Config.device_ui:screen_timeout: %d\r\n", config->payload_variant.device_ui.screen_timeout);
      break;

    default:
      // d("Unknown Config_Tag payload variant: %d\r\n", config->which_payload_variant);
  }
  return true;
}

bool handle_channel_tag(meshtastic_Channel *channel) {
  // d("ChannelTag:index: %d\r\n", channel->index);
  // d("ChannelTag:has_settings: %d\r\n", channel->has_settings);
  // d("ChannelTag:role: %d\r\n", channel->role);
  return true;
}

bool handle_FromRadio_log_record_tag(meshtastic_LogRecord *record) {
  // d("FromRadio_log_record:message: %s\r\n", record->message);
  // d("FromRadio_log_record:time: %d\r\n", record->time);
  // d("FromRadio_log_record:source: %s\r\n", record->source);
  // d("FromRadio_log_record:level: %s\r\n", record->level);
  return true;
}

bool handle_moduleConfig_tag(meshtastic_ModuleConfig *module){ 
  switch (module->which_payload_variant) {
      case meshtastic_ModuleConfig_mqtt_tag:
      // d("ModuleConfig:mqtt:enabled: %d\r\n", module->payload_variant.mqtt.enabled);
      // d("ModuleConfig:mqtt:address: %s\r\n", module->payload_variant.mqtt.address);
      // d("ModuleConfig:mqtt:username: %s\r\n", module->payload_variant.mqtt.username);
      // d("ModuleConfig:mqtt:password: %s\r\n", module->payload_variant.mqtt.password);
      // d("ModuleConfig:mqtt:encryption_enabled: %d\r\n", module->payload_variant.mqtt.encryption_enabled);
      // d("ModuleConfig:mqtt:json_enabled: %d\r\n", module->payload_variant.mqtt.json_enabled);
      // d("ModuleConfig:mqtt:root: %s\r\n", module->payload_variant.mqtt.root);
      // d("ModuleConfig:mqtt:proxy_to_client_enabled: %d\r\n", module->payload_variant.mqtt.proxy_to_client_enabled);
      // d("ModuleConfig:mqtt:map_reporting_enabled %d\r\n", module->payload_variant.mqtt.map_report_settings);
      // d("ModuleConfig:mqtt:has_map_report_settings %d\r\n", module->payload_variant.mqtt.has_map_report_settings);
      break;

      case meshtastic_ModuleConfig_serial_tag:
        // d("ModuleConfig:serial:enabled: %d\r\n", module->payload_variant.serial.enabled);
        // d("ModuleConfig:serial:echo: %d\r\n", module->payload_variant.serial.echo);
        // d("ModuleConfig:serial:rxd-gpio-pin: %d\r\n", module->payload_variant.serial.rxd);
        // d("ModuleConfig:serial:txd-gpio-pin: %d\r\n", module->payload_variant.serial.txd);
        // d("ModuleConfig:serial:baud: %d\r\n", module->payload_variant.serial.baud);
        // d("ModuleConfig:serial:timeout: %d\r\n", module->payload_variant.serial.timeout);
        // d("ModuleConfig:serial:mode: %d\r\n", module->payload_variant.serial.mode);
        // d("ModuleConfig:serial:override_console_serial_port: %d\r\n", module->payload_variant.serial.override_console_serial_port);
      break;