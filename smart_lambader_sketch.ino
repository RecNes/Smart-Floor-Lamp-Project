#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <FastLED.h>

#define PIN_RED D2
#define PIN_GREEN D5
#define PIN_BLUE D8
#define PIN_BTN D4
#define MIC_PIN A0
#define LCD_SDA D6
#define LCD_SCL D1

const char* projectName = "Smart Floor Lamp";
const char* staticColor = "Static Color";
const char* rainbowColor = "Rainbow";
const char* soundReactive = "Sound Reactive";
const char* randomSparkle = "Random Sparkle";
const char* standBy = "Stand By";

#define NUM_LEDS 1
CRGB leds[NUM_LEDS];
CRGB savedColor = CRGB::White;

const char* mqtt_server = "192.168.1.0"; // Change this
const char* mqtt_user = "mqtt_user_name"; // Change this
const char* mqtt_pass = "mqtt_user_pass"; // Change this
const char* discovery_topic = "homeassistant/light/smart_floor_lamp/config";
const char* state_topic = "homeassistant/light/smart_floor_lamp/state";
const char* select_state_topic = "homeassistant/select/smart_floor_lamp/state";
const char* command_topic = "homeassistant/light/smart_floor_lamp/set";
const char* select_set_topic = "homeassistant/select/smart_floor_lamp/set";
const char* select_config_topic = "homeassistant/select/smart_floor_lamp/config";

int menuMode = 0;
int lightEffect = 0;
int micValue = 0;
unsigned long lastButtonPress = 0;
unsigned long lastMqttRetry = 0;
int lastEffect = -1;
bool lightStatus = true;
int currentBrightness;

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient espClient;
PubSubClient client(espClient);

void showAnalogRGB() {
  if (lightStatus) {
    CRGB scaledColor = leds[0];
    scaledColor.nscale8(currentBrightness); 

    analogWrite(PIN_RED,   scaledColor.r);
    analogWrite(PIN_GREEN, scaledColor.g);
    analogWrite(PIN_BLUE,  scaledColor.b);
  } else {
    analogWrite(PIN_RED, 0);
    analogWrite(PIN_GREEN, 0);
    analogWrite(PIN_BLUE, 0);
  }
}

void lcd_printer(String text = "", int curX = 0, int curY = 0, bool clear = false) {
  if (text != NULL && text[0] != '\0') {
    if (clear) {
      lcd.clear();
    }
    lcd.setCursor(curX, curY);
    lcd.print(text);
    Serial.println(text);
  }
}

void display_ip() {
  lcd_printer("WiFi Connected.", 0, 0, true);
  lcd_printer("IP: " + WiFi.localIP().toString(), 0, 0);
}

void sendDiscovery() {
  String deviceBase = "{\"ids\":\"smart_floor_lamp\",\"name\":\"" + String(projectName) + "\",\"mf\":\"Sencer HAMARAT\",\"mdl\":\"NodeMCU RGB\"}";

  JsonDocument doc;
  doc["name"] = "Switch";
  doc["unique_id"] = "switch";
  doc["stat_t"] = state_topic;
  doc["cmd_t"] = command_topic;
  doc["schema"] = "json";
  doc["brightness"] = true;
  doc["brightness"] = true;
  doc["color_mode"] = true;
  JsonArray colorModes = doc["supported_color_modes"].to<JsonArray>();
  colorModes.add("rgb");
  doc["device"] = serialized(deviceBase);

  size_t n = measureJson(doc);
  char buffer[n + 1];
  serializeJson(doc, buffer, n + 1);
  client.publish(discovery_topic, buffer, true);

  JsonDocument sel;
  sel["name"] = "Modes";
  sel["unique_id"] = "effect";
  sel["stat_t"] = select_state_topic;
  sel["cmd_t"] = select_set_topic;
  sel["device"] = serialized(deviceBase);

  JsonArray opts = sel["options"].to<JsonArray>();
  opts.add(staticColor);
  opts.add(rainbowColor);
  opts.add(soundReactive);
  opts.add(randomSparkle);

  size_t nn = measureJson(sel);
  char sel_buf[nn + 1];
  serializeJson(sel, sel_buf, nn + 1);
  client.publish(select_config_topic, sel_buf, true);
}

void applyAndReport() {
  showAnalogRGB(); 
  
  updateLCD(); 

  JsonDocument doc;
  doc["state"] = lightStatus ? "ON" : "OFF";
  doc["brightness"] = currentBrightness;
  
  JsonObject color = doc["color"].to<JsonObject>();
  color["r"] = leds[0].r;
  color["g"] = leds[0].g;
  color["b"] = leds[0].b;

  String currentModeName = "";
  if (lightEffect == 0) currentModeName = staticColor;
  else if (lightEffect == 1) currentModeName = rainbowColor;
  else if (lightEffect == 2) currentModeName = soundReactive;
  else if (lightEffect == 3) currentModeName = randomSparkle;

  char buffer[256];
  serializeJson(doc, buffer);
  client.publish(state_topic, buffer, true);
  client.publish(select_state_topic, currentModeName.c_str(), true);
  
  Serial.println("Status reported.");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (!error && topicStr == command_topic) {
    if (doc.containsKey("state")) {
      lightStatus = (doc["state"] == "ON");
      if (lightStatus && currentBrightness < 5) currentBrightness = 255;
    }
    
    if (doc.containsKey("brightness")) {
      currentBrightness = doc["brightness"];
    }

    if (doc.containsKey("color")) {
      int r = doc["color"]["r"];
      int g = doc["color"]["g"];
      int b = doc["color"]["b"];
      
      leds[0].setRGB(r, g, b);
      savedColor = leds[0];
      
      lightStatus = true;
      lightEffect = 0;
    }
  }

  else if (topicStr == select_set_topic) {
    String msg = "";
    for (int i = 0; i < length; i++) msg += (char)payload[i];
    bool effectChanged = false;

    if (msg == staticColor)        { lightEffect = 0; menuMode = 1; effectChanged = true; leds[0] = savedColor;}
    else if (msg == rainbowColor)  { lightEffect = 1; menuMode = 2; effectChanged = true; }
    else if (msg == soundReactive) { lightEffect = 2; menuMode = 3; effectChanged = true; }
    else if (msg == randomSparkle) { lightEffect = 3; menuMode = 4; effectChanged = true; }

    if (effectChanged) {
      lightStatus = true;
      currentBrightness = 255;
      applyAndReport();
    }

  }

  applyAndReport();
  Serial.print("Brightness: "); Serial.println(currentBrightness);
}

void updateLCD() {
  switch (menuMode) {
    case 0: display_ip(); break;
    case 1:
      lcd_printer(String(staticColor), 0, 0, true);
      lightEffect = 0;
      break;
    case 2:
      lcd_printer(String(rainbowColor), 0, 0, true);
      lightEffect = 1;
      break;
    case 3:
      lcd_printer(String(soundReactive), 0, 0, true);
      lightEffect = 2;
      break;
    case 4:
      lcd_printer(String(randomSparkle), 0, 0, true);
      lightEffect = 3;
      break;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd_printer("Initializing", 0, 0, true);

  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  analogWriteRange(255);

  WiFiManager wifiManager;

  lcd_printer("Waiting for network...", 0, 0, true);
  lcd_printer(String(projectName) + " Installation", 0, 0);

  if (!wifiManager.autoConnect("Floor-Lamp-Setup")) {
    lcd_printer("Error! Reset.", 0, 0, true);
    delay(3000);
    ESP.reset();
  )

  client.setServer(mqtt_server, 1883);
  client.setBufferSize(1024);
  client.setCallback(callback);

  leds[0] = CRGB::White;

  for (int i = 0; i <= 255; i++) {
    CRGB tempColor = leds[0];
    tempColor.nscale8(i);

    analogWrite(PIN_RED, tempColor.r);
    analogWrite(PIN_GREEN, tempColor.g);
    analogWrite(PIN_BLUE, tempColor.b);

    delay(8);
  }
  lightStatus = true;
  currentBrightness = 255;
  leds[0] = CRGB::White;
  showAnalogRGB();
}

void fadeStrip(bool fadeIn, int maxBri, int wait) {
  if (fadeIn) {

    for (int i = 0; i <= maxBri; i += 5) {
      CRGB temp = leds[0];
      temp.nscale8(i);
      analogWrite(PIN_RED, temp.r);
      analogWrite(PIN_GREEN, temp.g);
      analogWrite(PIN_BLUE, temp.b);
      delay(wait);
    }

    CRGB finalColor = leds[0];
    finalColor.nscale8(maxBri);
    analogWrite(PIN_RED, finalColor.r);
    analogWrite(PIN_GREEN, finalColor.g);
    analogWrite(PIN_BLUE, finalColor.b);

  } else {
    for (int i = maxBri; i >= 0; i -= 5) {
      CRGB temp = leds[0];
      temp.nscale8(i);
      analogWrite(PIN_RED, temp.r);
      analogWrite(PIN_GREEN, temp.g);
      analogWrite(PIN_BLUE, temp.b);
      delay(wait);
      currentBrightness = i;
    }

    analogWrite(PIN_RED, 0);
    analogWrite(PIN_GREEN, 0);
    analogWrite(PIN_BLUE, 0);
  }
}


void loop() {
  if (!client.connected()) reconnect();
  client.loop();
  checkButton();

  if (lightStatus) {
    if (lightEffect != lastEffect) {
      lastEffect = lightEffect;
      runEffect();
      fadeStrip(true, currentBrightness, 3);
    }
    runEffect();

  } else {
    if (lastEffect != -1) {
      fadeStrip(false, currentBrightness, 5);
      lastEffect = -1;
    }
    analogWrite(PIN_RED, 0);
    analogWrite(PIN_GREEN, 0);
    analogWrite(PIN_BLUE, 0);
  }
}

void checkButton() {
  if (digitalRead(PIN_BTN) == LOW && millis() - lastButtonPress > 300) {
    menuMode = (menuMode + 1) % 5;
    lastButtonPress = millis();
    updateLCD();
    bool effectChanged = false;

    const char* currentEff;
    if (menuMode == 1) { lightEffect = 0; currentEff = staticColor; effectChanged = true; leds[0] = savedColor; }
    else if (menuMode == 2) { lightEffect = 1; currentEff = rainbowColor; effectChanged = true; }
    else if (menuMode == 3)  { lightEffect = 2; currentEff = soundReactive; effectChanged = true; }
    else if (menuMode == 4)  { lightEffect = 2; currentEff = randomSparkle; effectChanged = true; }
    else currentEff = standBy;
    lcd_printer("Menu Mode:" + String(menuMode), 0, 0);

    if (menuMode > 0 && effectChanged) {
      lightStatus = true;
      currentBrightness = 255;
      applyAndReport();
    }
  }
}

void reconnect() {
  if (!client.connected() && (millis() - lastMqttRetry > 5000)) {
    lastMqttRetry = millis();
    Serial.println("Retrying MQTT connection...");

    if (client.connect(projectName, mqtt_user, mqtt_pass)) {
      sendDiscovery();
      
      client.subscribe(command_topic);
      client.subscribe(select_set_topic);
      
      lcd_printer("MQTT OK!", 0, 0);
      applyAndReport(); 
      
    } else {
      lcd_printer("MQTT Error: " + String(client.state()), 0, 0);
    }
  }
}

void runSoundReactiveEffect() {
  int val = analogRead(MIC_PIN);
  int noiseThreshold = 350;
  int bri = 0;
  if (val > noiseThreshold) {
      bri = map(val, noiseThreshold, 800, 0, 255);
  } else {
      bri = 0;
  }

  bri = constrain(bri, 0, 255);
  static uint8_t visualHue = 160;
  if (bri > 150) visualHue += 2;
  leds[0] = CHSV(visualHue, 255, bri);
  showAnalogRGB();
  delay(5);
}

void runRainbowCycle() {
  static uint8_t hue = 0;
  leds[0] = CHSV(hue++, 255, 255);
  showAnalogRGB();
  delay(20);
}

void randomSparkleEffect() {
  static uint8_t targetHue = 0;
  EVERY_N_MILLISECONDS(1000) {
    targetHue = random8();
  }

  leds[0] = nblend(leds[0], CHSV(targetHue, 255, 255), 10);
  showAnalogRGB();
  delay(20);
}

void runEffect() {
  if (lightEffect == 0) showAnalogRGB();
  else if (lightEffect == 1) runRainbowCycle();
  else if (lightEffect == 2) runSoundReactiveEffect();
  else if (lightEffect == 3) randomSparkleEffect();
}
