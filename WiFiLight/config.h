// general
#define ID_PREFIX "LIGHT-"

// LED
#define NUM_LEDS 8    // number of LEDs in the strip/ring
#define DATA_PIN D2   // the GPIO pin for the LEDs' data

// MQTT
#define MQTT_PORT 1883                                 // usually 1883 or 8883 (MQTT over TLS/SSL)
#define MQTT_SERVER "mymqttserver.com"
#define MQTT_USER "user"
#define MQTT_PASSWORD "password"
#define MQTT_STATE_TOPIC "light"       // topic for reporting changes to the current state
#define MQTT_COMMAND_TOPIC "light-set" // topic for making changes to the current state

// HTTP
#define HTTP_SERVER_PORT 80 // default HTTP port is 80

// WebSockets
#define WEBSOCKET_PORT 81 // this port number is hardcoded in data/index.html and needs to match
