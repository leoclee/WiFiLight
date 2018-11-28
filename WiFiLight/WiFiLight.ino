/*
  WiFiLight - https://github.com/leoclee/WiFiLight
  Copyright Leonard Lee
  MIT license

  Allows for the control of LEDs using HTTP, websockets, or MQTT.
*/

#include "config.h" // Set configuration options for LEDs, MQTT, etc. in this file

#include <FS.h>               // file system
#include <ESP8266WiFi.h>      // ESP8266 Core WiFi Library
#include <DNSServer.h>        // Local DNS Server used for redirecting all requests to the WiFiManager configuration portal
#include <ESP8266WebServer.h> // webserver to handle HTTP requests and serve the WiFiManager configuration portal
#include <WiFiManager.h>      // WiFiManager  by tzapu           version 0.14.0 (https://github.com/tzapu/WiFiManager)
#include <FastLED.h>          // FastLED      by Daniel Garcia   version 3.2.1  (https://github.com/FastLED/FastLED)
#include <PubSubClient.h>     // PubSubClient by Nick O'Leary    version 2.7.0  (https://pubsubclient.knolleary.net)
#include <ArduinoJson.h>      // ArduinoJson  by Benoit Blanchon version 5.13.3 (https://arduinojson.org)
#include <WebSocketsServer.h> // WebSockets   by Markus Sattler  version 2.1.1  (https://github.com/Links2004/arduinoWebSockets)
#include <ArduinoOTA.h>

// state
unsigned int hue = 180;   // hue state (0-359)
uint8_t saturation = 100; // saturation state (0-100)
uint8_t value = 50;       // value/brightness state (0-100)
boolean state = true;     // on/off state (true = "ON", false = "OFF")
char effect[10] = "none"; // "none" | "colorloop" | "trail" | "rainbow" | "fire" (size of array needs to be max length of string + 1 for terminating null character)
const char* on_state = "ON";
const char* off_state = "OFF";

// LED
CRGB leds[NUM_LEDS];
CHSV fromColor;                                                                  // the color to fade from
CHSV toColor = CHSV(hue * 255 / 359, saturation * 255 / 100, value * 255 / 100); // the color to fade to
CHSV currentColor = toColor;                                                     // the current color
unsigned long startFadeMillis = 0;       // the milliseconds when a fade started
const unsigned long fadeInterval = 1000; // the duration of each fade transition in milliseconds
bool fading = false;                     // whether a fade is currently happening
CHSV savedColor;                         // last saved color
unsigned long lastColorChangeTime = 0;   // the milliseconds when the color was last changed
unsigned long colorSaveInterval = 15000; // number of milliseconds to wait for a color change to trigger a save
bool newEffect = false;                  // true if an effect was just assigned, false otherwise; use this to reset any effect variables if necessary

// WiFi
WiFiManager wifiManager;
unsigned long resetWiFiRequestedTime = 0;

// HTTP server
ESP8266WebServer server(HTTP_SERVER_PORT);

// Websockets
WebSocketsServer webSocket(WEBSOCKET_PORT);

#if MQTT_ENABLED
// MQTT
#if MQTT_PORT == 8883
WiFiClientSecure wiFiClient;
#else
WiFiClient wiFiClient;
#endif
unsigned long lastConnectAttempt = 0;
PubSubClient pubSubClient(wiFiClient);
#endif

/**
  Unique identifer used for wifi hostname, AP SSID, MQTT ClientId, etc.
*/
String getIdentifier() {
  // the ESP8266's chip ID is probably unique enough for our purposes
  // see: https://bbs.espressif.com/viewtopic.php?t=1303
  // see: https://github.com/esp8266/Arduino/issues/921
  String chipId = String(ESP.getChipId(), HEX);
  chipId.toUpperCase();
  return ID_PREFIX + chipId;
}

/**
   return a JSON string representation of the current state
*/
String getStateJson(bool save = false) {
  // code partially generated using https://arduinojson.org/v5/assistant/
  const size_t bufferSize = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(5);
  DynamicJsonBuffer jsonBuffer(bufferSize);
  JsonObject& root = jsonBuffer.createObject();
  if (!save) {
    root["id"] = getIdentifier();
  }
  root["brightness"] = value;
  root["state"] = state ? on_state : off_state;
  root["effect"] = effect;
  JsonObject& color = root.createNestedObject("color");
  color["h"] = hue;
  color["s"] = saturation;

  String jsonString;
  root.printTo(jsonString);
  return jsonString;
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  // load state from SPIFFS
  loadState();

  // LED
  FastLED.addLeds<NEOPIXEL, DATA_PIN>(leds, NUM_LEDS);
  FastLED.showColor(state ? currentColor : CHSV(0, 0, 0)); // turn lights on/off right away while other stuff is being setup

  // WiFi
  WiFi.hostname(getIdentifier().c_str());
  wifiManager.autoConnect(getIdentifier().c_str());
  Serial.printf("WiFi Hostname: %s\n", WiFi.hostname().c_str());
  Serial.printf("WiFi MAC addr: %s\n", WiFi.macAddress().c_str());
  Serial.printf("WiFi SSID: %s\n", WiFi.SSID().c_str());

  // HTTP server
  server.serveStatic("/", SPIFFS, "/index.html"); // handle root requests
  server.on("/light", HTTP_GET, []() {
    server.send(200, "application/json", getStateJson());
  });
  server.on("/light", HTTP_PUT, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "text/plain", "missing body on request");
      return;
    }

    handleJsonPayload(server.arg("plain").c_str());
    server.send(204);
  });
  server.on("/info", HTTP_GET, []() {
    server.send(200, "application/json", getInfoJson());
  });
  server.on("/wificonfig", HTTP_GET, []() {
    String message = "resetting wifi... please connect to ";
    message += getIdentifier();
    message += " to configure";
    server.send(200, "text/plain", message);

    resetWiFiRequestedTime = millis();
  });
  server.begin();
  Serial.printf("HTTP server started on port %d\n", HTTP_SERVER_PORT);

  // websocket server
  webSocket.begin();                          // start the websocket server
  webSocket.onEvent(webSocketCallback);       // if there's an incoming websocket message, go to function 'webSocketCallback'
  Serial.printf("WebSocket server started on port %d\n", WEBSOCKET_PORT);

#if MQTT_ENABLED
  // MQTT
  pubSubClient.setServer(MQTT_SERVER, MQTT_PORT);
  pubSubClient.setCallback(mqttCallback);
#endif

  // OTA
  ArduinoOTA.setHostname(getIdentifier().c_str()); // sets an mDNS hostname (defaults to esp8266-[ChipID] if not set)
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";
    Serial.println("OTA Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End");
  } );
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
}

void loop() {
#if MQTT_ENABLED
  // MQTT
  loop_mqtt();
#endif

  // HTTP
  server.handleClient();

  // websocket
  webSocket.loop();

  // reset WiFi
  loop_resetWiFi();

  // LED
  loop_led();

  saveColorChange();

  // OTA
  ArduinoOTA.handle();
}

void loop_resetWiFi() {
  if (resetWiFiRequestedTime > 0 && millis() - resetWiFiRequestedTime >= 3000) {
    // TODO can't get startConfigPortal to work (cannot connect to http://192.168.4.1 after connecting to AP)
    //wifiManager.startConfigPortal(getIdentifier().c_str());

    // ... so we are resetting & restarting as a workaround
    wifiManager.resetSettings(); // clear stored WiFi SSID/password
    ESP.restart(); // requires one manual reset after serial upload to work properly (https://circuits4you.com/2017/12/31/software-reset-esp8266/, https://github.com/esp8266/Arduino/issues/1017)
  }
}

#if MQTT_ENABLED
void loop_mqtt() {
  if (!pubSubClient.connected()) {
    unsigned long now = millis();
    if (now - lastConnectAttempt > 5000) {
      lastConnectAttempt = now;
      // Attempt to connect
      if (connectPubSub()) {
        lastConnectAttempt = 0;
      }
    }
  } else {
    // Client connected
    pubSubClient.loop();
  }
}

/**
   returns true if the MQTT connection attempt was successful; false otherwise
*/
boolean connectPubSub() {
  Serial.println("attempting MQTT connect");
  if (pubSubClient.connect(getIdentifier().c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.printf("MQTT connection established to %s:%d\n", MQTT_SERVER, MQTT_PORT);
    publishState();
    pubSubClient.subscribe(MQTT_COMMAND_TOPIC);
  }
  return pubSubClient.connected();
}

/**
   publishes the current state to the MQTT state topic
*/
void publishState() {
  String stateJson = getStateJson();
  Serial.printf("MQTT publish message [%s]: %s\n", MQTT_STATE_TOPIC, stateJson.c_str());
  pubSubClient.publish(MQTT_STATE_TOPIC, stateJson.c_str());
}

/**
  handle incoming MQTT message
*/
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("MQTT message arrived [%s]: ", topic);
  Serial.write(payload, length);
  Serial.println();

  if (strcmp(topic, MQTT_COMMAND_TOPIC) == 0) { // we only care about messages from the command topic
    handleJsonPayload((const char*) payload);
  }
}
#endif

/**
  returns true if the given effect string is non-empty, one of the valid effect values, and different from the current state's effect; false otherwise
*/
bool isValidAndDifferentEffect(const char *ef) {
  return ef && (strcmp(ef, "none") == 0 || strcmp(ef, "colorloop") == 0 || strcmp(ef, "trail") == 0 || strcmp(ef, "rainbow") == 0 || strcmp(ef, "fire") == 0) && strcmp(ef, effect) != 0;
}

/**
   return a JSON string representation of miscellaneous wifi, memory, etc. information
*/
String getInfoJson() {
  // code partially generated using https://arduinojson.org/v5/assistant/
  const size_t bufferSize = JSON_OBJECT_SIZE(9);
  DynamicJsonBuffer jsonBuffer(bufferSize);
  JsonObject& root = jsonBuffer.createObject();
  root["hostname"] = WiFi.hostname();
  root["ssid"] = WiFi.SSID();
  root["ipAddress"] = WiFi.localIP().toString();
  root["macAddress"] = WiFi.macAddress();
  root["rssi"] = WiFi.RSSI();
  root["sketchSize"] = ESP.getSketchSize();
  root["freeSketchSpace"] = ESP.getFreeSketchSpace();
  root["freeHeap"] = ESP.getFreeHeap();
  root["cpuFreqMHz"] = ESP.getCpuFreqMHz();

  String jsonString;
  root.printTo(jsonString);
  return jsonString;
}

/**
  sends the current state to all currently connected websocket clients
*/
void broadcastState() {
  String stateJson = getStateJson();
  Serial.printf("WebSocket broadcast: %s\n", stateJson.c_str());
  webSocket.broadcastTXT(stateJson);
}

/**
  notifies others of the current state, presumably because a change occurred
*/
void notify() {
#if MQTT_ENABLED
  publishState();
#endif
  broadcastState();
}

void loop_led() {
  if (state) {
    fadeToColor();
    if (strcmp(effect, "none") == 0) {
      FastLED.showColor(currentColor);
    } else if (strcmp(effect, "colorloop") == 0) {
      colorloop();
    } else if (strcmp(effect, "trail") == 0) {
      trail();
    } else if (strcmp(effect, "rainbow") == 0) {
      rainbow();
    } else if (strcmp(effect, "fire") == 0) {
      fire();
    }
    newEffect = false; // effect isn't new anymore
  } else {
    FastLED.clear(true); // turn off all LEDs
  }
}

/**
  Logic for the "colorloop" effect. It overrides the hue of the current color by a adding a constantly incrementing hue offset.
  The summed hue value is automatically kept in the expected range of 0-255, due to the uint8_t type.
  see: https://github.com/FastLED/FastLED/wiki/FastLED-HSV-Colors#why-fastled-full-range-one-byte-hues-are-faster
*/
void colorloop() {
  static uint8_t colorloopHueOffset = 0;
  if (newEffect) {
    colorloopHueOffset = 0; // reset hue offset so starting the colorloop effect won't cause a sudden jump in hue
  }
  EVERY_N_MILLISECONDS(10) { // TODO configurable colorloop speed?
    CHSV colorloopColor = CHSV(++colorloopHueOffset + currentColor.h, currentColor.s, currentColor.v);
    FastLED.showColor(colorloopColor);
  }
}

/**
  Logic for the "trail" effect. A colored dot with a fading trail moves along the LEDs, wrapping when it reaches the end.
*/
void trail() {
  static unsigned int pos = 0;
  if (newEffect) {
    pos = 0; // always start from the beginning
    FastLED.clear(true);
  }
  EVERY_N_MILLISECONDS(10) {
    fadeToBlackBy( leds, NUM_LEDS, 8); // TODO configurable trail fade speed?
    FastLED.show();
  }
  EVERY_N_MILLISECONDS(100) { // TODO configurable trail speed?
    pos = ++pos % NUM_LEDS;
    leds[pos] = currentColor;
    FastLED.show();
  }
}

/**
  Logic for the "rainbow" effect. The full hue spectrum is evenly distributed across all LEDs. Each LEDs' hue values are constantly incremented, giving the illusion that the rainbow is moving along the LEDs.
  The summed hue value is automatically kept in the expected range of 0-255, due to the uint8_t type.
  see: https://github.com/FastLED/FastLED/wiki/FastLED-HSV-Colors#why-fastled-full-range-one-byte-hues-are-faster
*/
void rainbow() {
  static uint8_t rainbowHueOffset = 0;
  EVERY_N_MILLISECONDS(10) { // TODO configurable rainbow speed?
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i] = CHSV(rainbowHueOffset + (255 / NUM_LEDS * i), currentColor.s, currentColor.v);
    }
    FastLED.show();
    rainbowHueOffset++;
  }
}

// Fire2012 by Mark Kriegsman, July 2012
// as part of "Five Elements" shown here: http://youtu.be/knWiGsmgycY
////
// This basic one-dimensional 'fire' simulation works roughly as follows:
// There's a underlying array of 'heat' cells, that model the temperature
// at each point along the line.  Every cycle through the simulation,
// four steps are performed:
//  1) All cells cool down a little bit, losing heat to the air
//  2) The heat from each cell drifts 'up' and diffuses a little
//  3) Sometimes randomly new 'sparks' of heat are added at the bottom
//  4) The heat from each cell is rendered as a color into the leds array
//     The heat-to-color mapping uses a black-body radiation approximation.
//
// Temperature is in arbitrary units from 0 (cold black) to 255 (white hot).
//
// This simulation scales it self a bit depending on NUM_LEDS; it should look
// "OK" on anywhere from 20 to 100 LEDs without too much tweaking.
//
// I recommend running this simulation at anywhere from 30-100 frames per second,
// meaning an interframe delay of about 10-35 milliseconds.
//
// Looks best on a high-density LED setup (60+ pixels/meter).
//
//
// There are two main parameters you can play with to control the look and
// feel of your fire: COOLING (used in step 1 above), and SPARKING (used
// in step 3 above).
//
// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 55, suggested range 20-100
#define COOLING  55
// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 120
#define FRAMES_PER_SECOND 60
bool gReverseDirection = true;
CRGBPalette16 gPal;
void fire() {
  if (newEffect) {
    FastLED.clear(true);
  }

  // a custom palette based on the current color
  CRGB darkcolor  = CHSV(currentColor.h, currentColor.s, currentColor.v * 3 / 4); // pure hue, three-quarters brightness
  CRGB lightcolor = CHSV(currentColor.h, currentColor.s / 2, currentColor.v); // half 'whitened', full brightness
  gPal = CRGBPalette16( CRGB::Black, darkcolor, lightcolor);

  EVERY_N_MILLISECONDS(1000 / FRAMES_PER_SECOND) {
    // Array of temperature readings at each simulation cell
    static byte heat[NUM_LEDS];

    // Step 1.  Cool down every cell a little
    for ( int i = 0; i < NUM_LEDS; i++) {
      heat[i] = qsub8( heat[i],  random8(0, ((COOLING * 10) / NUM_LEDS) + 2));
    }

    // Step 2.  Heat from each cell drifts 'up' and diffuses a little
    for ( int k = NUM_LEDS - 1; k >= 2; k--) {
      heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2] ) / 3;
    }

    // Step 3.  Randomly ignite new 'sparks' of heat near the bottom
    if ( random8() < SPARKING ) {
      int y = random8(7);
      heat[y] = qadd8( heat[y], random8(160, 255) );
    }

    // Step 4.  Map from heat cells to LED colors
    for ( int j = 0; j < NUM_LEDS; j++) {
      // Scale the heat value from 0-255 down to 0-240
      // for best results with color palettes.
      byte colorindex = scale8( heat[j], 240);
      CRGB color = ColorFromPalette( gPal, colorindex);
      int pixelnumber;
      if ( gReverseDirection ) {
        pixelnumber = (NUM_LEDS - 1) - j;
      } else {
        pixelnumber = j;
      }
      leds[pixelnumber] = color;
    }

    FastLED.show();
  }
}

/*
  sets the hue state; an update will not be triggered until updateColor() is called
*/
void setHue(int h) {
  hue = h % 360;
}

/*
   sets the saturation state; an update will not be triggered until updateColor() is called
*/
void setSaturation(int s) {
  saturation = constrain(s, 0, 100);
}

/*
  sets the value state; an update will not be triggered until updateColor() is called
*/
void setBrightness(int brightness) {
  value = constrain(brightness, 0, 100);
}

/**
   Sets the toColor using a FastLED CHSV object based on the current color state, which requires converting traditional HSV scales to FastLED's 0-255
   see: https://github.com/FastLED/FastLED/wiki/FastLED-HSV-Colors#numeric-range-differences-everything-here-is-0-255
*/
void updateColor() {
  setColor(CHSV(hue * 255 / 359, saturation * 255 / 100, value * 255 / 100));
}

/**
   sets the toColor using a FastLED CHSV object; the fromColor is set to the currentColor
*/
void setColor(CHSV toChsv) {
  Serial.printf("setting color to CHSV(%d,%d,%d)\n", toChsv.h, toChsv.s, toChsv.v);

  fromColor = currentColor;
  toColor = toChsv;
  unsigned long now = millis();
  startFadeMillis = now;
  fading = true;

  lastColorChangeTime = now; // prevents the state from being saved too frequently (see saveColorChange())
}

/**
   fades the currentColor from fromColor toward toColor using a linear interpolation
*/
void fadeToColor() {
  if (fading) {
    unsigned long fadeMillis = millis() - startFadeMillis; // how many milliseconds has the current fade been going
    if (fadeMillis < fadeInterval) {
      currentColor = blend(fromColor, toColor, fadeMillis * 255 / fadeInterval);
    } else {
      fading = false;
      currentColor = toColor; // in case we didn't fade all the way to toColor for some reason
    }
  }
}

/**
  Throttles color change induced saving to avoid unnecessary write / erase cycles (due to flash wear).
  Note that this means if the power is cut immediately following a color change, the new color might not have saved and the old color is shown upon restoring power.
  see: https://design.goeszen.com/mitigating-flash-wear-on-the-esp8266-or-any-other-microcontroller-with-flash.html
*/
void saveColorChange() {
  if (toColor != savedColor && ((millis() - lastColorChangeTime) > colorSaveInterval)) {
    Serial.println("state save triggered by color change");
    saveState();
  }
}

/**
   load state from SPIFFS -- should be called in setup() to restore previous state on startup
*/
void loadState() {
  // read state from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/state.json")) {
      //file exists, reading and loading
      Serial.println("reading state file");
      File stateFile = SPIFFS.open("/state.json", "r");
      if (stateFile) {
        Serial.println("opened state file");

        // read file contents into buffer
        size_t size = stateFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        stateFile.readBytes(buf.get(), size);
        stateFile.close();

        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          JsonVariant h = json["color"]["h"];
          if (h.success()) {
            setHue(h.as<int>());
          }
          JsonVariant s = json["color"]["s"];
          if (s.success()) {
            setSaturation(s.as<int>());
          }
          JsonVariant v = json["brightness"];
          if (v.success()) {
            setBrightness(v.as<int>());
          }

          // store initial color in toColor
          toColor = CHSV(hue * 255 / 359, saturation * 255 / 100, value * 255 / 100);
          savedColor = toColor; // prevent the first color change from being saved immediately
          currentColor = toColor; // make the current color the loaded color for that initial call to fill()

          const char* stateJson = json["state"]; // "ON" or "OFF"
          if (stateJson) {
            state = strcmp(stateJson, on_state) == 0;
          }

          const char* effectJson = json["effect"];
          if (isValidAndDifferentEffect(effectJson)) {
            strcpy(effect, effectJson);
          }
        } else {
          Serial.println("failed to load json state");
        }
      } else {
        Serial.println("failed to open state file for reading");
      }
    } else {
      Serial.println("state file not found");
    }
  } else {
    Serial.println("failed to mount FS");
  }
}

/**
   save state to SPIFFS -- should be called whenever the state needs to be persisted
*/
void saveState() {
  File stateFile = SPIFFS.open("/state.json", "w");
  if (!stateFile) {
    Serial.println("failed to open state file for writing");
    return;
  }

  String stateJson = getStateJson(true);
  Serial.print("saving state ");
  Serial.println(stateJson);
  stateFile.print(stateJson);
  stateFile.close();

  savedColor = toColor;
}

/**
   handles incoming websocket events
*/
void webSocketCallback(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_TEXT:
      { Serial.printf("WebSocket [%u] text: %s\n", num, payload);
        handleJsonPayload((const char *)payload);
        break;
      }
    case WStype_DISCONNECTED:
      Serial.printf("WebSocket [%u] disconnected\n", num);
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("WebSocket [%u] connected: %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);

        // immediately send state to client
        String stateJson = getStateJson();
        webSocket.sendTXT(num, stateJson);
      }
      break;
  }
}

/**
   Processes the given JSON string, making any state changes as necessary.
*/
void handleJsonPayload(const char* payload) {
  // (somewhat) adheres to Home Assistant's MQTT JSON Light format (https://www.home-assistant.io/components/light.mqtt_json/)
  // code partially generated using https://arduinojson.org/v5/assistant/
  const size_t bufferSize = JSON_OBJECT_SIZE(2) + JSON_OBJECT_SIZE(4);
  DynamicJsonBuffer jsonBuffer(bufferSize);
  JsonObject& root = jsonBuffer.parseObject(payload);

  bool saveRequired = false;

  // state
  const char* stateJson = root["state"]; // "ON" or "OFF"
  if (stateJson) {
    if (!state && strcmp(stateJson, on_state) == 0) {
      state = true;
      saveRequired = true;
    } else if (state && strcmp(stateJson, off_state) == 0) {
      state = false;
      saveRequired = true;
    }
  }

  // effect
  const char* effectJson = root["effect"];
  if (isValidAndDifferentEffect(effectJson)) {
    strcpy(effect, effectJson);
    newEffect = true;
    saveRequired = true;
  }

  // color
  bool colorChanged = false;
  JsonVariant hueJson = root["color"]["h"];        // 0 to 359
  if (hueJson.success() && hue != hueJson.as<int>()) {
    setHue(hueJson.as<int>());
    colorChanged = true;
  }
  JsonVariant saturationJson = root["color"]["s"]; // 0 to 100
  if (saturationJson.success() && saturation != saturationJson.as<int>()) {
    setSaturation(saturationJson.as<int>());
    colorChanged = true;
  }
  JsonVariant brightness = root["brightness"];     // 0 to 100
  if (brightness.success() && value != brightness.as<int>()) {
    setBrightness(brightness.as<int>());
    colorChanged = true;
  }
  if (colorChanged) {
    updateColor();
  }

  if (saveRequired) {
    saveState();
  }
  if (saveRequired || colorChanged) {
    notify();
  }
}
