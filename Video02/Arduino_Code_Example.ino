// Author: Anthony Vela
// Date Started: November 2016
// Date Completed: May 2017
// Credit is given to the relevant parties/authors of the libraries included in this sketch.
// 
// What does it do:
// This sketch handles the processing for Vela Hiko MQTT switches. Due to hardware limitations, the
// sketch is only capable of ON/OFF.
// The software will connect to a Wifi network, announce mDNS, connect to an MQTT server with username and password, 
// subscribe to topics, await commands on */com and publish its current state to */state (as 0 or 1).
// It will also publish its settings via HTTP to (ip-address)/info , and receive settings via HTTP at (ip-address)/set .
// 

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <DNSServer.h>  
#include <WiFiManager.h>
#include <pcf8574_esp.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>


// Flag for saving data on Wifi setup
bool shouldSaveConfig = false;

// Chip ID
char apString[30];

// I2C initialization for PCF8574
TwoWire testWire;
PCF857x pcf8574(0x20, &testWire);

// MQTT setup
// Callback function header
void callback(char* topic, byte* payload, unsigned int length);
int InitPin[] = {0,0,0,0};
int InitPinState[] = {1,1,1,1};
int RelayState[] = {0,0,0,0};


ESP8266WebServer server(8080);
ESP8266HTTPUpdateServer httpUpdater;
WiFiClient espClient;
PubSubClient client(espClient);

// ****************************** IMPORTANT **************************************************************************
// Change switches to the amount of relays on the board. Note that '1' relay uses a different PCB (with no PCF8574) so
// is handled differently to 2-4 relays.
// Version must be updated in order to trigger an update from your server to download a new .bin file. It doesnt matter 
// whether the server .bin is updated, if this version number is not updated nothing will happen.
const int switches = 4; 
const String updateVersion = "0.0.5";

// MQTT Variables
String mqttServer = "192.168.1.255";
String mqttOld = mqttServer;
int mqttPort = 8123;
String mqttUser = "av";
String mqttPass = "av";
String roomVal[] = {"bathroom","bathroom","bathroom","bathroom"};
String typeVal[] = {"lights","heating","lamps","fan"};
int mqttCounter = 0;

// Update Server Variables
String updateAddress = "http://velahiko.onlinewebshop.net/esp/update/" + String(switches) + "relay/source.bin";

void setup(void){
  Serial.begin(115200);
  EEPROM.begin(512);
  
  // TESTING - USED TO RESET THE DEVICE BACK TO DEFAULT
  //EEPROM.write(0,0);
  //EEPROM.commit();
  //WiFi.disconnect();

  Serial.println(updateAddress);
  
  // WiFi startup
  String tmpStr;
  char tmpChr[42]; 
  mqttServer.toCharArray(tmpChr, 40);
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", tmpChr, 40);
  tmpStr = String(mqttPort);
  tmpStr.toCharArray(tmpChr, 6);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", tmpChr, 6);
  mqttUser.toCharArray(tmpChr, 32);
  WiFiManagerParameter custom_mqtt_user("user", "mqtt user", tmpChr, 32);
  mqttPass.toCharArray(tmpChr, 32);
  WiFiManagerParameter custom_mqtt_pass("pass", "mqtt password", tmpChr, 32);
  WiFiManager wifiManager;
  wifiManager.setTimeout(600);
  //wifiManager.setDebugOutput(false); // Disable DEBUG
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);
  //wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  tmpStr = String(ESP.getChipId());
  Serial.print("Chip ID: ");
  Serial.println(tmpStr);
  Serial.println("Starting WiFi...");
  sprintf(apString, "VELAHIKO-SWITCH-%06X", ESP.getChipId());
  if (!wifiManager.autoConnect(apString)) {
    Serial.println("** WIFI FAILED TO START. REBOOTING.");
      delay(3000);
      //reset and try again, or maybe put it to deep sleep
      ESP.reset();
      delay(5000);   
    Serial.println(".");
    delay(500);
  }
  Serial.print("IP address..... ");
  Serial.println(WiFi.localIP());
  Serial.print("mDNS Address.... ");
  Serial.println(String(apString) + ".local");

  // EEPROM startup
  if (shouldSaveConfig == true) {
    // First start on this network, so save the MQTT settings to EEPROM
    Serial.print("Update EEPROM...");
    mqttServer = custom_mqtt_server.getValue();
    mqttPort = atoi(custom_mqtt_port.getValue());
    mqttUser = custom_mqtt_user.getValue();
    mqttPass = custom_mqtt_pass.getValue();
    Serial.println(" OK");
    readWrite();
  } else if (EEPROM.read(0) == 0) {
    // No saved settings, use default settings
    Serial.println("Load Default settings..... OK");
  } else if (EEPROM.read(0) > 0) {
    // Saved settings exist, so load them instead
    Serial.print("Load settings from EEPROM.....");
    readRead();
    Serial.println(" OK");
  }

  // mDNS startup
  Serial.print("Starting mDNS broadcast....");
  if (MDNS.begin(apString)) {
    MDNS.addService("velahiko-switch", "tcp", 8080); // Announce esp tcp service on port 8080
    Serial.println(" OK");
  } else {
    Serial.println(" FAIL");
  }

  // HTTP startup
  Serial.print("Starting HTTP server...");
  server.on("/", handleRoot); // Send to manual MQTT settings vis HTTP
  server.on("/info", handleInfo); // Read back settings via HTTP
  server.on("/test", handleTest); // Test the relay board for errors
  server.on("/set", setInfo); // Store settings via HTTP
  server.on("/reset", resetDevice); // Reset device back to Default via HTTP (must have arg Confirm=yes)
  server.on("/reboot", rebootDevice); // Reboot device via HTTP (must have arg confirm=yes)
  server.on("/switch", getsetSwitch); // Switch the device via HTTP
  server.on("/update", updateDevice); // Check for updates
  httpUpdater.setup(&server, "/firmware", "admin", "admin"); // Firmware updater
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(" OK");

  // GPIO or PCF startup
  // (For 1 relay and 1+ relay boards respectively)
  if (switches>1) {
    // Set up the GPIO pins for I2C, set clock speed and go!
    testWire.begin(2, 0);
    // Specsheets say PCF8574 is officially rated only for 100KHz I2C-bus
    // PCF8575 is rated for 400KHz
    testWire.setClock(100000L);
    pcf8574.begin();
    // Relays:
    pcf8574.write(0, HIGH);
    pcf8574.write(1, HIGH);
    pcf8574.write(2, HIGH);
    pcf8574.write(3, HIGH);
    // Switches
    int InitPinA = 0;
    InitPinA = pcf8574.read(4);
    InitPinA = pcf8574.read(5);
    InitPinA = pcf8574.read(6);
    InitPinA = pcf8574.read(7);
  } else if (switches==1) {
      pinMode(0, OUTPUT); // Initialize the relay pin as an output     
      pinMode(3, INPUT_PULLUP); // Initialize the switch as an input
  }

  // Autodetect MQTT settings if no saved settings found
  if (mqttServer == mqttOld) {
    Serial.print("Autodetecting MQTT settings...");
    int n = MDNS.queryService("hap", "tcp"); // Send out query for openhab tcp services
    if (n == 0) {
        Serial.println(" FAIL - WARNING ONLY");
    } else {
        Serial.println(" OK");
        //IPAddress myIp = MDNS.IP(0);
        mqttServer = MDNS.IP(0).toString();
        Serial.print("Requesting MQTT details from ");
        Serial.print(mqttServer);
        Serial.print("...");
        WiFiClient httpClient;
        char mqttSv[40];
        mqttServer.toCharArray(mqttSv,40);
        const int httpPort = 7679;
        if (!httpClient.connect(mqttSv, httpPort)) {
          Serial.println(" FAIL");
          return;
        } else {
          Serial.println(" OK");
        }
        // We now create a URI for the request
        String url = "/getinfo?confirm=yes";
        //Serial.print("Requesting URL: ");
        //Serial.println(url);
        // This will send the request to the server
        httpClient.print(String("GET ") + url + " HTTP/1.1\r\n" +
                     "Host: " + mqttServer + "\r\n" + 
                     "Connection: close\r\n\r\n");
        delay(10);
        // Read all the lines of the reply from server and print them to Serial
        if(httpClient.available()){
          String line = httpClient.readString();
          //Serial.print(line);
          int tot = 0;
          for (int y = 0; y <= line.length(); y++) {
            if (String(line.charAt(y)) == ",") { 
              tot++; 
            }
          }
          for (int p = 0; p <= tot; p++) {
            String val0 = getValue(line, '^', 1);
            String val1 = getValue(val0, ',', p);
            String val2a = getValue(val1,'=',0);
            String val2b = getValue(val1,'=',1);
            //Serial.print(p);
            //Serial.print(" - Value: ");
            //Serial.println(val1);
            if (val2a == "mqttServer") {
              if (val2b == "127.0.0.1") {
                mqttServer = MDNS.IP(0).toString();
              } else {
                mqttServer = val2b;
              }
              Serial.print("MQTT Server..... ");
              Serial.println(mqttServer);
            }
            if (val2a == "mqttPort") {
              mqttPort = val2b.toInt();
              Serial.print("MQTT Port..... ");
              Serial.println(mqttPort);
            }
            if (val2a == "mqttUser") {
              mqttUser = val2b;
              Serial.print("MQTT User..... ");
              Serial.println(mqttUser);
            }
            if (val2a == "mqttPass") {
              mqttPass = val2b;
              Serial.print("MQTT Pass..... ");
              Serial.println(mqttPass);
            }
          }
        }
    }
  }

  // Finalize MQTT details ready to connect
  char mqttServ[40];
  mqttServer.toCharArray(mqttServ,40);
  client.setServer(mqttServ, mqttPort);
  client.setCallback(callback);

  // Try to connect to MQTT service once
  reconnect();

  // Lets rock!!
  Serial.println("Joining main loop....");
}

void loop(void){
  // HTTP server check
  server.handleClient();

  // MQTT connection check - Check once every minute (roughly)
  mqttCounter = mqttCounter + 1;
  if (mqttCounter >= 60000) { 
    reconnect();
    mqttCounter = 0;
  }
  // MQTT feed check
  client.loop();

  // Physical Light switches
  int i;
  for (i = 0; i < int(switches); i = i + 1) {
      // IF statement added to check between 1 and 2+ relay boards. "1" relay board is driven from the GPIO
      if (switches > 1) {
        InitPin[i] = pcf8574.read(i + 4);
      } else if (switches == 1) {
        InitPin[i] = digitalRead(3);
      }
      
      // Compare previous state to current state and check for false tripping
      int tripCheck = 0; // int to check for false tripping of the switch
      if (InitPin[i] != InitPinState[i]) {
        delay(10); // wait for 300ms
        if (switches > 1) { // check which method to read
          if (InitPin[i] == pcf8574.read(i + 4)) { // Check if PCF state remains the same as 300ms ago
            tripCheck = 1; // Intentional switch, tell program to continue on
          } else {
            tripCheck = 0; // Unntentional switch, tell program not to trigger a switch
          }
        } else if (switches == 1) {
          if (InitPin[i] == digitalRead(3)) { // Check if GPIO state remains the same as 300ms ago
            tripCheck = 1; // Intentional switch, tell program to continue on
          } else {
            tripCheck = 0; // Unntentional switch, tell program not to trigger a switch
          }
        }
      }
      
      if (tripCheck == 1) {
        Serial.print("SWITCH: ");
        InitPinState[i] = InitPin[i];
        if (RelayState[i] == 0) {
          Serial.print("Relay ");
          Serial.print(i);
          switchOn(i);
        } else if (RelayState[i] == 1) {
          Serial.print("Relay ");
          Serial.print(i);
          switchOff(i);
        }
      }
  }
}

void switchOn(int relay) {
    // IF statement added to check between 1 and 2+ relay boards. 1 relay board is driven from the GPIO
    if (switches > 1) {
      pcf8574.write(relay, HIGH);   // Turn the light on for PCF
    } else if (switches==1) {
      digitalWrite(0, HIGH); // Turn the light on for GPIO
    }
    Serial.println(" ON");
    RelayState[relay] = 1;
    announceState("1",relay);
    delay(10);
}

void switchOff(int relay) {
    // IF statement added to check between 1 and 2+ relay boards. 1 relay board is driven from the GPIO
    if (switches > 1) {
      pcf8574.write(relay, LOW);  // Turn the light off for PCF
    } else if (switches==1) {
      digitalWrite(0, LOW); // Turn the light off for GPIO
    }
    Serial.println(" OFF");
    RelayState[relay] = 0;
    announceState("0",relay);
    delay(10);
}

void callback(char* topic, byte* payload, unsigned int length) {
  for (int i = 0; i < int(switches); i = i + 1) {
    char topicNOW[60];
    char roomval[32];
    char typeval[32];
    roomVal[i].toCharArray(roomval,32);
    typeVal[i].toCharArray(typeval,32);
    strcpy(topicNOW, roomval);
    strcat(topicNOW, "/");
    strcat(topicNOW, typeval);
    strcat(topicNOW, "/com");
    if (strcmp(topicNOW,topic) == 0) {
      if (((char)payload[0] == '0') && (RelayState[i] == 1)) {
        Serial.print("MQTT: RELAY ");
        Serial.print(i);
        switchOff(i);
      } else if (((char)payload[0] == '1') && (RelayState[i] == 0)) {
        Serial.print("MQTT: RELAY ");
        Serial.print(i);
        switchOn(i);
      }
    }
  }
  //resubscribe(); // Remove this to prevent ESP overloads. Seems to work fine without it.
}

void announceState(char* statement, int relay) {
        char topicNOW[60];
        char roomval[32];
        char typeval[32];
        roomVal[relay].toCharArray(roomval,32);
        typeVal[relay].toCharArray(typeval,32);
        strcpy(topicNOW, roomval);
        strcat(topicNOW, "/");
        strcat(topicNOW, typeval);
        strcat(topicNOW, "/state");
        Serial.print("Publishing Status on ");
        Serial.print(topicNOW);
        Serial.print(": ");
        Serial.println(statement);
        client.publish(topicNOW, statement);
}

void reconnect() {
  // Loop until we're reconnected to MQTT server
  if (!client.connected()) {
    Serial.print("Starting MQTT connection...");
    // Create a random client ID
    String clientId = "VelaHiko-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    char mqttServ[40];
    char mqttUse[20];
    char mqttPas[20];
    mqttServer.toCharArray(mqttServ,40);
    mqttUser.toCharArray(mqttUse, 20);
    mqttPass.toCharArray(mqttPas, 20);
    client.setServer(mqttServ, mqttPort);
    if (client.connect(clientId.c_str(), mqttUse, mqttPas)) {
      Serial.println(" OK");
      resubscribe();
    } else {
      Serial.println(" FAIL");
      // Wait 1 second before retrying
      delay(100);
    }
  }
}

void resubscribe () {
  for (int i = 0; i < int(switches); i = i + 1) {
    char topicNOW[60];
    char roomval[32];
    char typeval[32];
    roomVal[i].toCharArray(roomval,32);
    typeVal[i].toCharArray(typeval,32);
    strcpy(topicNOW, roomval);
    strcat(topicNOW, "/");
    strcat(topicNOW, typeval);
    strcat(topicNOW, "/com");
    client.subscribe(topicNOW);
  }
}


void saveConfigCallback () {
  shouldSaveConfig = true;
}

void configModeCallback (WiFiManager *myWiFiManager) {
//  Old Code:
//  Serial.println(WiFi.softAPIP());
//  Serial.println(myWiFiManager->getConfigPortalSSID());
  
}

void rebootDevice() {
  if (server.args() > 0 ) {
    for ( uint8_t i = 0; i < server.args(); i++ ) {
      if (server.argName(i) == "confirm") {
        if (server.arg(i) == "yes") {
          Serial.println("Reboot command received");
          server.send(200, "text/plain", "OK");
          if (shouldSaveConfig == true) {
            readWrite();
          }
          delay(1000);
          ESP.restart(); // Simple reboot to restore settings, load configs etc
        }
      }
    }
  }
}

void resetDevice() {
  if (server.args() > 0 ) {
    for ( uint8_t i = 0; i < server.args(); i++ ) {
      if (server.argName(i) == "confirm") {
        if (server.arg(i) == "yes") {
          Serial.println("Reset command received");
          EEPROM.write(0,0); // Setting addr0 to 0 will tell the device that no settings exist,therefore resetting back to default
          server.send(200, "text/plain", "OK");
          delay(1000);
          WiFi.disconnect(); // Reset WiFi settings back to config mode
          ESP.restart();
        }
      }
    }
  }
}

void setInfo() {
  if (server.args() > 0 ) {
    for ( uint8_t i = 0; i < server.args(); i++ ) {
      if (server.argName(i) == "mqtt_server") {
        mqttServer = server.arg(i);
      }
      if (server.argName(i) == "mqtt_port") {
        mqttPort = server.arg(i).toInt();
      }
      if (server.argName(i) == "mqtt_user") {
        mqttUser = server.arg(i);
      }
      if (server.argName(i) == "mqtt_pass") {
        mqttPass = server.arg(i);
      }
      for (int jj = 0; jj < switches; jj = jj + 1) {
        delay(10);
        if (server.argName(i) == (String(jj) + "A")) {
          roomVal[jj] = server.arg(i);
        }
        if (server.argName(i) == (String(jj) + "B")) {
          typeVal[jj] = server.arg(i);
        }
      }
    }
  }
  // Commit to EEPROM
  readWrite();
  handleRoot();
}

void handleInfo() {
String responseHTML = "\
version=(updateversion)\n\
mqtt_server=(mqttserver)\n\
mqtt_port=(mqttport)\n\
mqtt_user=(mqttuser)\n\
mqtt_pass=(mqttpass)\n\
mqtt_conn=(mqttconn)\n\
switches=(switchcount)\n\
(roomvals)\n\
(typevals)\n\
";
  if (!client.connected()) { responseHTML.replace("(mqttconn)","0"); } else { responseHTML.replace("(mqttconn)","1"); }
  String rmvals = "";
  String tyvals = "";
  for (int k = 0; k < switches; k = k + 1) {
    rmvals = rmvals + String(k) + "=" + roomVal[k] + "\n";
    tyvals = tyvals + String(k) + "=" + typeVal[k] + "\n";
  }
  rmvals.remove((rmvals.length()) - 1);
  tyvals.remove((tyvals.length()) - 1);
  responseHTML.replace("(roomvals)",rmvals);
  responseHTML.replace("(typevals)",tyvals);
  responseHTML.replace("(mqttserver)",mqttServer);
  responseHTML.replace("(mqttport)",String(mqttPort));
  responseHTML.replace("(mqttuser)",mqttUser);
  responseHTML.replace("(mqttpass)",mqttPass);
  responseHTML.replace("(switchcount)",String(switches));
  responseHTML.replace("(updateversion)",updateAddress);
  server.send(200, "text/plain", responseHTML);
}

// To handle the root directory of the webserver
void handleRoot() {
  //WiFi.localIP();
  String tempHTML = "<html><head><title>Vela Hiko | WiFi Switch | Configuration</title>\
                     <style>\
                        body { background-color: #ffffff; font-family: Arial, Helvetica, Sans-Serif; Color: #0d0f11; }\
                        .button {\
                          background-color: #4CAF50;\
                          border: none;\
                          color: white;\
                          padding: 15px 32px;\
                          text-align: center;\
                          text-decoration: none;\
                          display: inline-block;\
                          font-size: 16px;\
                          margin: 4px 2px;\
                          cursor: pointer;\
                        }\
                     </style>\
                     </head>\
                     <body>\
                     <img src='https://atvela.files.wordpress.com/2016/10/velahiko.png' align='middle'>\
                     <p><b>Chip ID:</b> (chipid) </p>\
                     <p><br><b>Firmware</b></p>\
                     <p>Current Firmware: (fwversion)\
                     <br>Upload:\
                     <br><iframe src='./firmware' width='600' style='border:0px'></iframe></p>\
                     <p><br><b>MQTT - Configuration</b></p>\
                     <form action='/set' method='GET'>\
                     <p>MQTT Server: <input type='text' name='mqtt_server' value='(mqttserver)'>\
                     <br>MQTT Port: <input type='text' name='mqtt_port' value='(mqttport)'></p>\
                     <p>MQTT Username: <input type='text' name='mqtt_user' value='(mqttuser)'>\
                     <br>MQTT Password: <input type='text' name='mqtt_pass' value='(mqttpass)'></p>\
                     <p>MQTT Topic(s):\
                     <br>(topics)\
                     </p>\
                     <p><input type='submit' class='button' value='Update Settings'></p>\
                     </form></body></html>";
  String main_topic = "";
  for (int k = 0; k < switches; k++) {
    main_topic = main_topic + "<br>Switch " + String(k) + ": ";
    main_topic = main_topic + "<input type='text' name='" + String(k) + "A' value='" + roomVal[k] + "'> / <input type='text' name='" + String(k) + "B' value='" + typeVal[k] + "'>";
  }
  tempHTML.replace("(chipid)",apString);                   
  tempHTML.replace("(mqttserver)",mqttServer);
  tempHTML.replace("(mqttport)",String(mqttPort));
  tempHTML.replace("(mqttuser)",mqttUser);
  tempHTML.replace("(mqttpass)",mqttPass);
  tempHTML.replace("(topics)",main_topic);
  tempHTML.replace("(updateserver)",updateAddress);
  tempHTML.replace("(fwversion)",updateVersion);
  server.send(200, "text/html", tempHTML);
  delay(100);
}

// Run a series of tests on the board
void handleTest() {
  String responseHTML = "Testing...";
  server.send(200, "text/plain", responseHTML);
  // Test the relays 1 at a time without 
  
  if (switches > 1) {
    for (int k = 0; k < switches; k++) {
      pcf8574.write(k, LOW);  // Turn the light off for PCF
      delay(500);
      pcf8574.write(k, HIGH);  // Turn the light on for PCF
      delay(500);
      pcf8574.write(k, LOW);  // Turn the light on for PCF
      delay(500);
      pcf8574.write(k, HIGH);  // Turn the light on for PCF
      delay(500);
      pcf8574.write(k, LOW);  // Turn the light on for PCF
      delay(500);
      pcf8574.write(k, HIGH);  // Turn the light on for PCF
      delay(500);
      pcf8574.write(k, LOW);  // Turn the light on for PCF
      delay(3000);
      // Try again with less delay
      pcf8574.write(k, LOW);  // Turn the light off for PCF
      delay(100);
      pcf8574.write(k, HIGH);  // Turn the light on for PCF
      delay(100);
      pcf8574.write(k, LOW);  // Turn the light on for PCF
      delay(100);
      pcf8574.write(k, HIGH);  // Turn the light on for PCF
      delay(100);
      pcf8574.write(k, LOW);  // Turn the light on for PCF
      delay(100);
      pcf8574.write(k, HIGH);  // Turn the light on for PCF
      delay(100);
      pcf8574.write(k, LOW);  // Turn the light on for PCF
      delay(3000);      
    }
    // Run all relays at once
    for (int u; u < 5; u++) {
      for (int j; j < switches; j++) {
        pcf8574.write(u, LOW);  // Turn the light off for PCF
        delay(50);
      }
      delay(500);
      for (int j; j < switches; j++) {
        pcf8574.write(j, HIGH);  // Turn the light on for PCF
        delay(50);
      }
    }
  } else if (switches==1) {
    for (int k = 0; k < 5; k++) {
      digitalWrite(0, LOW); // Turn the light off for GPIO
      delay(500);
      digitalWrite(0, HIGH); // Turn the light off for GPIO
      delay(500);
    }
  }
}

void getsetSwitch() {
  int argCount = 0;
  int deviceNum = 0;
  int deviceState = 0;
  if (server.args() > 0 ) {
    for ( uint8_t i = 0; i < server.args(); i++ ) {
      if (server.argName(i) == "device") {
        deviceNum = server.arg(i).toInt();
        if (deviceNum < switches) {
          argCount++;
        }
      }
      if (server.argName(i) == "state") {
        deviceState = server.arg(i).toInt();
        argCount++;
      }
    }
  }
  if (argCount >= 2) {
    if (deviceState == 0) {
      Serial.print("Relay ");
      Serial.print(deviceNum);
      switchOff(deviceNum);
      server.send(200, "text/plain", String(RelayState[deviceNum]));
    } else if (deviceState == 1) {
      Serial.print("Relay ");
      Serial.print(deviceNum);
      switchOn(deviceNum);
      server.send(200, "text/plain", String(RelayState[deviceNum]));
    } else if (deviceState == 2) {
      Serial.println("Relay state requested");
      server.send(200, "text/plain", String(RelayState[deviceNum]));
    }
  } else {
    server.send(200, "text/plain", "Syntax Error");
  }
}


void readWrite() {
  String bigString = "";
  int bigStart = 1;
  bigString += "mqttServer=" + mqttServer + ",";
  bigString += "mqttPort=" + String(mqttPort) + ",";
  bigString += "mqttUser=" + mqttUser + ",";
  bigString += "mqttPass=" + mqttPass + ",";
  bigString += "switches=" + String(switches) + ",";
  String rmvals = "";
  String tyvals = "";
  for (int k = 0; k < switches; k = k + 1) {
    rmvals = rmvals + String(k) + "A=" + roomVal[k] + ",";
    tyvals = tyvals + String(k) + "B=" + typeVal[k] + ",";
  }
  bigString += rmvals;
  bigString += tyvals;
  
  // Remove the trailing comma
  //bigString.remove((bigString.length() - 1));

  // Display results
  int strLen = bigString.length();
  Serial.print("Saving to EEPROM.");
  EEPROM.write(0, strLen);  
  for (int p = 0; p < strLen; p++) {
    Serial.print(".");   
    EEPROM.write((p + 1), int(bigString.charAt(p))); 
  }
  Serial.println(" OK");
  EEPROM.commit();

  // Update config save flag
  shouldSaveConfig = false;
}

void readRead() {
  int readVal = EEPROM.read(0);
  String firstHalf = "";
  String secondHalf = "";
  int switchedHalf = 0;
  for (int k = 1; k <= (readVal); k++) {
    if ((String(char(EEPROM.read(k))) != "=") && (switchedHalf==0)) {
      firstHalf += String(char(EEPROM.read(k)));
    } else if ((String(char(EEPROM.read(k))) != ",") && (switchedHalf==1)) {
      secondHalf += String(char(EEPROM.read(k)));
    }
    
    if ((String(char(EEPROM.read(k))) == "=") && (switchedHalf==0)) {
      switchedHalf = 1;
    } else if ((String(char(EEPROM.read(k))) == ",") && (switchedHalf==1)) {
      Serial.print(".");
        if (firstHalf == "mqttServer") {
          mqttServer = secondHalf;   
        } else if (firstHalf == "mqttPort") {
          mqttPort = secondHalf.toInt();
        } else if (firstHalf == "mqttUser") {
          mqttUser = secondHalf;
        } else if (firstHalf == "mqttPass") {
          mqttPass = secondHalf;  
        } else if (firstHalf == "0A") {
          roomVal[0] = secondHalf;
        } else if (firstHalf == "1A") {
          roomVal[1] = secondHalf;
        } else if (firstHalf == "2A") {
          roomVal[2] = secondHalf;
        } else if (firstHalf == "3A") {
          roomVal[3] = secondHalf;
        } else if (firstHalf == "0B") {
          typeVal[0] = secondHalf;
        } else if (firstHalf == "1B") {
          typeVal[1] = secondHalf;
        } else if (firstHalf == "2B") {
          typeVal[2] = secondHalf;
        } else if (firstHalf == "3B") {
          typeVal[3] = secondHalf;
        }
      // Reset values
      firstHalf = "";
      secondHalf = "";
      switchedHalf = 0;
    }
  }
}

String getValue(String data, char separator, int index) {
    int found = 0;
    int strIndex[] = { 0, -1 };
    int maxIndex = data.length() - 1;

    for (int i = 0; i <= maxIndex && found <= index; i++) {
        if (data.charAt(i) == separator || i == maxIndex) {
            found++;
            strIndex[0] = strIndex[1] + 1;
            strIndex[1] = (i == maxIndex) ? i+1 : i;
        }
    }
    return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void updateDevice() {
  t_httpUpdate_return ret = ESPhttpUpdate.update(updateAddress);
  
  switch(ret) {
      case HTTP_UPDATE_FAILED:
          Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
          server.send(200, "text/plain", "FAIL");
          break;
      case HTTP_UPDATE_NO_UPDATES:
          Serial.println("HTTP_UPDATE_NO_UPDATES");
          server.send(200, "text/plain", "NO UPDATE");
          break;
      case HTTP_UPDATE_OK:
          Serial.println("HTTP_UPDATE_OK");
          server.send(200, "text/plain", "SUCCESS");
          //ESP.restart();
          break;
  }
}

void handleNotFound(){
  server.send(404, "text/plain", "Thats a 404 cuzzie. (Page not found)");
}
