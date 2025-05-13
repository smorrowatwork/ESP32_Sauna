#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Encoder.h>
#include <secrets.h>

// Author:  Steven Morrow & Patrick Morrow
// Date:    05/11/2025
//
// TODO:
//          √ Implement smoother timing on the web end, we only poll at 2 second
//                intervals so the time jumps by 2 or 3 seconds depending.  Perhaps
//                get a starting value from the status call and re-sync the countdown
//                but implement the countdown logic on the web client.
//          √ Add some CSS and pretty up the style of the website
//          √ Ensure that adding 15 minutes from the web interface never takes us over
//                90 minutes.  If adding 15 goes over 90 it should just set it to 90.
//          √ Implement client code that enable/disables start and stop buttons based on
//                the current sauna status.
//          √ Implement client and server code to not allow (disable) adding 15 minutes
//                when the sauna is off     

// === PINS ===
#define SSR_PIN 25
#define ENCODER_A 33 //DT
#define ENCODER_B 32 //CLK
#define ENCODER_SW 26
#define ONE_WIRE_BUS 27
//#define LCD_SDA 21
//#define LCD_SCL 22

// === LCD and sensor setup ===
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Adjust I2C address if needed
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- Encoder Setup ---
ESP32Encoder encoder;
int lastPosition = 0;

// -- Webserver ---
WebServer server(80);

// --- Flame character ---
byte flameChar[8] = {
  B00100,
  B00101,
  B00101,
  B10111,
  B10111,
  B11111,
  B11110,
  B01110
};

// --- Menu options ---
const char* menuItems[] = {"Start", "Stop", "Set", "IP"};
const int menuLength = 4;
int menuIndex = 0;

// === WiFi Info ===
// *Use secrets.h file to define primary and secondary WiFi credentials
unsigned long wifiTimeout = 20000; // 20 seconds in milliseconds

// --- Timing ---
unsigned long lastTempRead = 0;
unsigned long lastLCDUpdate = 0;

// --- Button state tracking ---
bool lastButtonState = HIGH;

// === STATE ===
bool saunaOn = false;
bool isSettingTime = false;
int setMinutes = 0;
unsigned long countdownMillis = 0;
unsigned long targetTime = 0;
String strTimeRemaining = "";

// === CONSTANTS ===
const int MAX_TIME = 90;  // 1 hour 30 minutes
const unsigned long IP_DISPLAY_TIME = 4000; // 4 seconds

// --- Temperature ---
DeviceAddress sensorAddress;
float currentTempF = 0.0;
bool tempConversionInProgress = false;
unsigned long tempRequestTime = 0;

// Displays the connected network
void showIP() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(WiFi.SSID());
  lcd.setCursor(0,1);
  lcd.print(" ");
  lcd.print(WiFi.localIP());
  delay(IP_DISPLAY_TIME);
  lcd.clear();
}

// === Function to connect to WiFi ===
void connectToWiFi(const char* ssid, const char* password) {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Try: ");
  lcd.print(ssid);
  Serial.printf("Connecting to %s...\n", ssid);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();

  lcd.setCursor(0,1);
  int progressCtr = 0;
  // Keep trying to connect until timeout
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < wifiTimeout) {
    delay(500);
    progressCtr++;
    Serial.print(".");
    lcd.print(".");
    if (progressCtr >= 6) {
      //Clear progress
      lcd.setCursor(0,1);
      lcd.print("          ");
      lcd.setCursor(0,1);
      progressCtr = 0;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    showIP();
  } else {
    Serial.println("\nFailed to connect.");
    WiFi.disconnect(true); // optional: ensure clean disconnect
    delay(1000);
  }
}

// --- Turn the Sauna On/Off
void setSauna(bool on){
  if (on) {
    digitalWrite(SSR_PIN, HIGH);
  } else {
    digitalWrite(SSR_PIN, LOW);
  }
}

void updateStateAndDisplay() {
  // Line 1: Temperature and Timer
  lcd.setCursor(0, 0);
  lcd.print(currentTempF, 1);
  lcd.print((char)223); // Degree symbol
  lcd.print("F");

  // Timer display
  unsigned long secsLeft = countdownMillis / 1000;
  int mins = secsLeft / 60;
  int secs = secsLeft % 60;
  
  // Update time remaining string
  strTimeRemaining =  mins < 10 ? "0" : "";
  strTimeRemaining += String(mins);
  strTimeRemaining += ":";
  strTimeRemaining += secs < 10 ? "0" : "";
  strTimeRemaining += String(secs);

  lcd.setCursor(8, 0);
  lcd.print(strTimeRemaining);

  // Update Sauna switch state
  setSauna(saunaOn);

  // Flame or underscore at (15, 0)
  lcd.setCursor(15, 0);
  if (saunaOn) {
    lcd.write(byte(0)); // flame icon
  } else {
    lcd.print("_");
  }

  // Line 2: Menu or Time Setting
  lcd.setCursor(0, 1);
  if (isSettingTime) {
    lcd.print(">Set Time: ");
    if (setMinutes < 10) lcd.print(" ");
    lcd.print(setMinutes);
    lcd.print("m ");
  } else {
    lcd.print(">");
    lcd.print(menuItems[menuIndex]);
    int len = strlen(menuItems[menuIndex]);
    for (int i = 0; i < 15 - len; i++) lcd.print(" ");
  }

  delay(2);  // Keep loop responsive
}

// =================================
// ==  Web implementation         ==
// =================================
void handleRoot() {
  String html = R"rawliteral(
    <html>
    <head>
      <meta charset="UTF-8">
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <!--<style>
        body { font-family: sans-serif; text-align: center; padding: 20px; }
        button { padding: 10px 20px; font-size: 18px; margin: 10px; }
        .status { font-size: 24px; margin-top: 20px; }
      </style>-->
      <style>
        body {
          font-family: 'Segoe UI', sans-serif;
          background: #f5f5f5;
          color: #333;
          padding: 20px;
          text-align: center;
        }
        h1 {
          color: #444;
          margin-bottom: 10px;
        }
        .status {
          background: white;
          display: inline-block;
          padding: 20px;
          border-radius: 10px;
          box-shadow: 0 4px 12px rgba(0,0,0,0.1);
          margin-bottom: 20px;
        }
        .status p {
          margin: 10px 0;
          font-size: 1.2em;
        }
        button {
          background: #007aff;
          color: white;
          border: none;
          padding: 15px 25px;
          font-size: 1.1em;
          border-radius: 8px;
          cursor: pointer;
          margin: 10px;
          box-shadow: 0 2px 6px rgba(0,0,0,0.1);
          transition: background 0.3s;
        }
        button:hover {
          background: #005fcc;
        }
        button:disabled {
          background: #ccc;
          cursor: now-allowed;
          box-shadow: none;
        }
      </style>
    </head>
    <body>
      <h1>Sauna Controller</h1>
      <div class="status">
        <p>Temperature: <span id="temp">--</span> °F</p>
        <p>Time Remaining: <span id="time">--</span> min</p>
        <p>Status: <span id="state">--</span></p>
      </div>
      <button id="onBtn" onclick="sendCommand('/on')">Turn ON</button>
      <button id="offBtn" onclick="sendCommand('/off')">Turn OFF</button>
      <!--<button id="addBtn" onclick="sendCommand('/addtime')">Add 15 min</button>-->
      <button id="addBtn" onclick="addTimeCommand()">Add 15 min</button>

      <script>
        let remainingSeconds = 0;

        function addTimeCommand() {
          fetch('/addtime').then(() => setTimeout(() => {updateStatus();},500));
        }

        function sendCommand(endpoint) {
          fetch(endpoint).then(() => updateStatus());
        }

        function updateStatus(){
          fetch('/status')
            .then(res => res.json())
            .then(data => {
              document.getElementById('temp').textContent = data.temp;

              saunaOn = data.state === true || data.state === "On";
              document.getElementById('state').textContent = saunaOn ? 'On' : 'Off';

              // Enable/disable buttons
              document.getElementById('onBtn').disabled = saunaOn;
              document.getElementById('offBtn').disabled = !saunaOn;
              document.getElementById('addBtn').disabled = !saunaOn;

              if (saunaOn){
                const [mm,ss] = data.time.split(':').map(Number);
                remainingSeconds = mm * 60 + ss;
              } else {
                remainingSeconds = 0;
              }

              updateTimeDisplay();
            });
        }

        function updateTimeDisplay() {
          const mm = Math.floor(remainingSeconds / 60);
          const ss = remainingSeconds % 60;
          document.getElementById('time').textContent =
            `${mm.toString().padStart(2,'0')}:${ss.toString().padStart(2,'0')}`;
        }

        setInterval(() => {
          if (saunaOn && remainingSeconds > 0) {
            remainingSeconds--;
            updateTimeDisplay();
            if (remainingSeconds == 0){
              // Pre-emptively set the state to 'Off' but use an asterisk to indicate "unofficial"
              document.getElementById('state').textContent = 'Off*';
            }
          }
        }, 1000);

        setInterval(updateStatus, 5000);

        updateStatus();
      </script>
    </body>
    </html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void handleOn() {
  if (countdownMillis == 0) {
    unsigned long now = millis();
    lcd.setCursor(0,0);
    countdownMillis = 90 * 60000UL;
    targetTime = now + countdownMillis;
    saunaOn = true;
    server.send(200, "text/plain", "Sauna turned on");
  }
  else {
    server.send(200, "tesxt/plain", "Sauna already on");
  }
}

void handleOff() {
  saunaOn = false;
  countdownMillis = 0;
  server.send(200, "text/plain", "Sauna turned off");
}

void handleAddTime() {
  unsigned long now = millis();                                       // Get the current time
  unsigned long addMillis = 15 * 60000UL;                             // 15 minutes to add
  countdownMillis = constrain((countdownMillis + addMillis), 0UL, (90 * 60000UL));  // Constrain to max of 90 minutes
  targetTime = now + countdownMillis;                                 // Update target end time
  server.send(200, "text/plain", "OK");                               // Respond to browser
}

void handleStatus() {
  String json = "{";
  json += "\"temp\":" + String(currentTempF, 1) + ",";
  json += "\"time\":\"" + strTimeRemaining + "\",";
  json += "\"state\":";
  json += (saunaOn ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void setup() {
  // --- Initialize sensors, lcd, and encoder
  Serial.begin(9600);
  delay(1000); // give time for Serial

  pinMode(SSR_PIN, OUTPUT);
  pinMode(ENCODER_SW, INPUT_PULLUP);

  // Set up encoder
  encoder.attachFullQuad(ENCODER_A, ENCODER_B);
  encoder.clearCount();

  // Set up lcd
  lcd.init();
  lcd.backlight();
  lcd.createChar(0, flameChar);

  sensors.begin();
  if (sensors.getAddress(sensorAddress, 0)){
    sensors.setResolution(sensorAddress, 10);
  }

  // --- Connect to primary or secondary WiFi
  connectToWiFi(WIFI_SSID_1, WIFI_PWD_1);

  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi(WIFI_SSID_2, WIFI_PWD_2);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Could not connect to any network.");
  }

  // -- Register website paths
  server.on("/", handleRoot);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/addtime", handleAddTime);
  server.on("/status", handleStatus);
  server.begin();

  lcd.clear();
  updateStateAndDisplay();
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // --- Non-blocking Temperature read every 1s ---
  if (!tempConversionInProgress && (now - lastTempRead >= 1000)){
    sensors.requestTemperatures();
    tempRequestTime = now;
    tempConversionInProgress = true;
  }

  if (tempConversionInProgress && (now - tempRequestTime >= 750)) {
    if (sensors.isConversionComplete()){
      currentTempF = sensors.getTempFByIndex(0);
      lastTempRead = now;
      tempConversionInProgress = false;
    }
  }

  // --- Encoder Movement ---
  long newPosition = encoder.getCount() / 4;  // divide by 4 to account for encoder resolution

  if (newPosition != lastPosition) {
    int delta = newPosition - lastPosition;

    if (isSettingTime) {
      setMinutes += (delta > 0 ? 1 : -1);
      // Implemnent roll around
      if (setMinutes > MAX_TIME) {
        Serial.println("positive");
        setMinutes = 0;
      }
      //Serial.print(MAX_TIME);
      //Serial.println(" max");
      //Serial.println(setMinutes);
      if (setMinutes < 0) {
        Serial.println("negative");
        setMinutes = MAX_TIME;
      }
    } else {
      menuIndex += (delta > 0 ? 1 : -1);
      if (menuIndex < 0) menuIndex = menuLength - 1;
      if (menuIndex >= menuLength) menuIndex = 0;
    }

    lastPosition = newPosition;
  }

  // --- Button Press Detection ---
  bool currentButtonState = digitalRead(ENCODER_SW);
  if (lastButtonState == HIGH && currentButtonState == LOW) {

    // -- Handle button press --
    if (isSettingTime) {
      countdownMillis = setMinutes * 60000UL;
      isSettingTime = false;
    } else {
      String selected = menuItems[menuIndex];
      if (selected == "Start" && !saunaOn && countdownMillis > 0) {
        saunaOn = true;
        targetTime = now + countdownMillis;
      } else if (selected == "Stop" && saunaOn) {
        saunaOn = false;
      } else if (selected == "Set" && !saunaOn) {
        isSettingTime = true;
      } else if (selected == "IP") {
        showIP();
      }
    }
  }

  lastButtonState = currentButtonState;
   
  // --- Countdown logic ---
  if (saunaOn && countdownMillis > 0) {
    long remaining = targetTime - now;
    if (remaining <= 0) {
      countdownMillis = 0;
      saunaOn = false;
    } else {
      countdownMillis = remaining;
    }
  }

  // --- Update LCD every 200ms ---
  if (now - lastLCDUpdate >= 200) {
    lastLCDUpdate = now;
    updateStateAndDisplay();
  }

  // Free up the processor for a short time
  delay(10);
}