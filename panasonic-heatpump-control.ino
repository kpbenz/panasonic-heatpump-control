/*

Web-Server for Panasonic Heatpump CS-Z25UFRAW

HTTP/HTML only (port 80)

Uses:

Arduino Due Wifi Rev
- IR-LED at pin 3 (220 Ohm resistor)
- BMP280 via I2S bus for air pressure and temperature


*/
#include <SPI.h>
#include <WiFiNINA.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>

#include "arduino_secrets.h"
///////please enter your sensitive data in the Secret tab/arduino_secrets.h
char ssid[] = SECRET_SSID;        // your network SSID (name)
char pass[] = SECRET_PASS;    // your network password (use for WPA, or use as key for WEP)
int keyIndex = 0;                 // your network key index number (needed only for WEP)

char g_hostname[] = "ac-livingroom";
char g_ac_title[] = "Livingroom";

int status = WL_IDLE_STATUS;
WiFiServer server(80);

// BMP280 Sensor
// Module connected to SCL, SDA, 3.3V and GND
Adafruit_BMP280 bmp;
Adafruit_Sensor *bmp_temp = bmp.getTemperatureSensor();
Adafruit_Sensor *bmp_pressure = bmp.getPressureSensor();


// Infrared LED on digital PIN 3 (needs a PWM pin)
// Connect with 1 kOhm resistor in series to GND
#define IR_LED_PIN        3

// Panasonic DKE timing constants
#define PANASONIC_AIRCON2_HDR_MARK   3500
#define PANASONIC_AIRCON2_HDR_SPACE  1800
#define PANASONIC_AIRCON2_BIT_MARK   420
#define PANASONIC_AIRCON2_ONE_SPACE  1350
#define PANASONIC_AIRCON2_ZERO_SPACE 470
#define PANASONIC_AIRCON2_MSG_SPACE  10000

// Panasonic DKE codes
#define PANASONIC_AIRCON2_MODE_AUTO  0x00 // Offset 13: Operating mode
#define PANASONIC_AIRCON2_MODE_HEAT  0x40
#define PANASONIC_AIRCON2_MODE_COOL  0x30
#define PANASONIC_AIRCON2_MODE_DRY   0x20
#define PANASONIC_AIRCON2_MODE_FAN   0x60
#define PANASONIC_AIRCON2_MODE_OFF   0x00 // Power OFF
#define PANASONIC_AIRCON2_MODE_ON    0x01
#define PANASONIC_AIRCON2_MODE_UNKNOWN_BIT 0x08 // Unknown bit, use by remotecontrol

#define PANASONIC_AIRCON2_TIMER_CNL  0x08
#define PANASONIC_AIRCON2_FAN_AUTO   0xA0 // Offset 16: Fan speed
#define PANASONIC_AIRCON2_FAN1       0x30
#define PANASONIC_AIRCON2_FAN2       0x40
#define PANASONIC_AIRCON2_FAN3       0x50
#define PANASONIC_AIRCON2_FAN4       0x60
#define PANASONIC_AIRCON2_FAN5       0x70
#define PANASONIC_AIRCON2_VS_AUTO    0x0F // Offset 16: Vertical swing
#define PANASONIC_AIRCON2_VS_UP      0x01
#define PANASONIC_AIRCON2_VS_MUP     0x02
#define PANASONIC_AIRCON2_VS_MIDDLE  0x03
#define PANASONIC_AIRCON2_VS_MDOWN   0x04
#define PANASONIC_AIRCON2_VS_DOWN    0x05
#define PANASONIC_AIRCON2_HS_AUTO    0x0D // Horizontal swing
#define PANASONIC_AIRCON2_HS_MIDDLE  0x06
#define PANASONIC_AIRCON2_HS_LEFT    0x09
#define PANASONIC_AIRCON2_HS_MLEFT   0x0A
#define PANASONIC_AIRCON2_HS_MRIGHT  0x0B
#define PANASONIC_AIRCON2_HS_RIGHT   0x0C
#define PANASONIC_AIRCON2_HS_UNSUPP  0x00

#define PANASONIC_AIRCON2_POWERFUL   0x01 // Offset 21
#define PANASONIC_AIRCON2_UNKNOWN1   0x89 // Offset 23
#define PANASONIC_AIRCON2_NANOEX     0x04 // Offset 26


// Global AC state
byte g_onOff = PANASONIC_AIRCON2_MODE_OFF;
byte g_operatingMode = PANASONIC_AIRCON2_MODE_AUTO;
byte g_temperature = 22;
byte g_fanSpeed = PANASONIC_AIRCON2_FAN_AUTO;
byte g_swingV = PANASONIC_AIRCON2_VS_AUTO;
byte g_swingH = PANASONIC_AIRCON2_HS_UNSUPP;

void printWifiStatus()
{
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
  // print where to go in a browser:
  Serial.print("To see this page in action, open a browser to http://");
  Serial.println(ip);
}


void setup() {
  Serial.begin(9600);      // initialize serial communication
  pinMode(9, OUTPUT);      // set the LED pin mode

  if(!bmp.begin(BMP280_ADDRESS_ALT,BMP280_CHIPID))
  {
    Serial.println("BMP280 chip initialisation failed!");
    // don't continue
    while (true);
  }
  else
  {
    Serial.println("BMP280 chip initialised.");
  }

  /* Default settings from datasheet. */
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */

  bmp_temp->printSensorDetails();

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true);
  }

  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }
  WiFi.setHostname(g_hostname);

  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to Network named: ");
    Serial.println(ssid);                   // print the network name (SSID);

    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);
    // wait 10 seconds for connection:
    delay(10000);
  }
  server.begin();                           // start the web server on port 80
  printWifiStatus();                        // you're connected now, so print out the status

  enableIROut(); // Safe to enable PWM after WiFi connects
}



void mark(int time)
{
  TCB1.CTRLA |= TCB_ENABLE_bm;        // Enable pin output
  delayMicroseconds(time);
}

void space(int time)
{
  TCB1.CTRLA &= ~TCB_ENABLE_bm;        // Disable pin output
  //digitalWrite(IR_LED_PIN, LOW);             // Set pin LOW after disabling PWM PORTC.OUTCLR = PIN0_bm;              // Force pin LOW
  delayMicroseconds(time);
}

// Send a byte over IR
void sendIRByte(byte sendByte, int bitMarkLength, int zeroSpaceLength, int oneSpaceLength)
{
  for (int i=0; i<8 ; i++)
  {
    if (sendByte & 0x01)
    {
      mark(bitMarkLength);
      space(oneSpaceLength);
    }
    else
    {
      mark(bitMarkLength);
      space(zeroSpaceLength);
    }

    sendByte >>= 1;
  }
}


void enableIROut()
{
   // put your setup code here, to run once:
  pinMode(IR_LED_PIN,OUTPUT);
  analogWrite(IR_LED_PIN, 128); // dummy write to set the flags right

  // 38khz
  TCB1_CTRLA=TCB_CLKSEL_CLKDIV2_gc; //set clock divider, but keep output disabled.
  TCB1_CCMP=(104<< 8) |208;
}




// Send the Panasonic DKE code

void sendPanasonicDKE(byte operatingMode, byte fanSpeed, byte temperature, byte swingV, byte swingH)
{
  // for Panasonic A/C CS-Z25UFRAW
  byte DKE_template[] = { 0x02, 0x20, 0xE0, 0x04, 0x00, 0x00, 0x00, 0x06,
                          0x02, 0x20, 0xE0, 0x04, 0x00, 0x48, 0x2E, 0x80,
                          0xA3, 0x0D, 0x00, 0x0E, 0xE0, 0x00 & ~PANASONIC_AIRCON2_POWERFUL, 0x00, PANASONIC_AIRCON2_UNKNOWN1,
                          0x00, PANASONIC_AIRCON2_NANOEX, 0x00 };


  DKE_template[13] = operatingMode;
  DKE_template[14] = temperature << 1;
  DKE_template[16] = fanSpeed | swingV;
  DKE_template[17] = swingH;

  // Checksum
  byte checksum = 0xF4; // Panasonic Checksum init
  for (int i=0; i<26; i++)
  {
    checksum += DKE_template[i];
  }

  DKE_template[26] = checksum;

  // Header
  mark(PANASONIC_AIRCON2_HDR_MARK);
  space(PANASONIC_AIRCON2_HDR_SPACE);

  // First 8 bytes
  for (int i=0; i<8; i++)
  {
    sendIRByte(DKE_template[i], PANASONIC_AIRCON2_BIT_MARK, PANASONIC_AIRCON2_ZERO_SPACE, PANASONIC_AIRCON2_ONE_SPACE);
  }

  // Pause
  mark(PANASONIC_AIRCON2_BIT_MARK);
  space(PANASONIC_AIRCON2_MSG_SPACE);

  // Header
  mark(PANASONIC_AIRCON2_HDR_MARK);
  space(PANASONIC_AIRCON2_HDR_SPACE);

  // Last 19 bytes
  for (int i=8; i<27; i++) {
    sendIRByte(DKE_template[i], PANASONIC_AIRCON2_BIT_MARK, PANASONIC_AIRCON2_ZERO_SPACE, PANASONIC_AIRCON2_ONE_SPACE);
  }

  mark(PANASONIC_AIRCON2_BIT_MARK);
  space(0);
}

void switchOnOff(byte status)
{
  g_onOff = status;
}

void switchMode(byte mode)
{
  g_operatingMode = mode;
}

void switchVSwitch(byte mode)
{
  g_swingV = mode;
}

void sendAcState(void)
{
  sendPanasonicDKE(g_operatingMode|g_onOff|0x08, g_fanSpeed, g_temperature, g_swingV, g_swingH);
}


void loop()
{
  sensors_event_t temp_event, pressure_event;



  WiFiClient client = server.available();   // listen for incoming clients

  if (client) {                             // if you get a client,
    Serial.println("new client");           // print a message out the serial port
    String currentLine = "";                // make a String to hold incoming data from the client
    while (client.connected()) {            // loop while the client's connected
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        // Serial.write(c);                    // print it out to the serial monitor
        if (c == '\n') {                    // if the byte is a newline character

          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0)
          {
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Location: /");
            client.println();
            client.println("<HEAD>");
            client.print("<TITLE>A/C-Controller ");
            client.print(g_ac_title);
            client.println("</TITLE>");
            client.println("</HEAD>");
            client.println("<BODY>");
            client.print("<H1>A/C-Controller ");
            client.print(g_ac_title);
            client.println("</H1>");

            // Get the temerature
            bmp_temp->getEvent(&temp_event);
            bmp_pressure->getEvent(&pressure_event);
            Serial.print("Temperature: ");
            Serial.print(temp_event.temperature,1);
            Serial.print(" degC    ");
            Serial.print("Air Pressure: ");
            Serial.print(pressure_event.pressure,1);
            Serial.println(" hPa");

            // the content of the HTTP response follows the header:
            client.print("Temperature: ");
            client.print(temp_event.temperature,1);
            client.print("&#x2103;<br>");
            client.print("Air Pressure: ");
            client.print(pressure_event.pressure,1);
            client.print(" hPa<br>");
            client.print("<form action=\"action\" method=\"GET\">");

            client.println("<p><b>Power</b>&nbsp;");

            client.print("<input type=\"radio\" name=\"power\" id=\"power_on\" value=\"ON\"");
            client.print(g_onOff == PANASONIC_AIRCON2_MODE_ON ? " checked":"");
            client.println("> <label for=\"power_on\">On</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"power\" id=\"power_off\" value=\"OFF\"");
            client.print(g_onOff == PANASONIC_AIRCON2_MODE_OFF ? " checked":"");
            client.println("> <label for=\"power_off\">Off</label></p>");

            client.println("<p><b>Mode</b>&nbsp;");

            client.print("<input type=\"radio\" name=\"mode\" id=\"mode_auto\" value=\"AUTO\"");
            client.print(g_operatingMode == PANASONIC_AIRCON2_MODE_AUTO ? " checked":"");
            client.println("> <label for=\"mode_auto\">Auto</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"mode\" id=\"mode_heat\" value=\"HEAT\"");
            client.print(g_operatingMode == PANASONIC_AIRCON2_MODE_HEAT ? " checked":"");
            client.println("> <label for=\"mode_heat\">Heat</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"mode\" id=\"mode_cool\" value=\"COOL\"");
            client.print(g_operatingMode == PANASONIC_AIRCON2_MODE_COOL ? " checked":"");
            client.println("> <label for=\"mode_cool\">Cool</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"mode\" id=\"mode_dry\" value=\"DRY\"");
            client.print(g_operatingMode == PANASONIC_AIRCON2_MODE_DRY ? " checked":"");
            client.println("> <label for=\"mode_cool\">Dry</label></p>");

            // Vertical Swing

            client.println("<p><b>Vertical Swing</b>&nbsp;");

            client.print("<input type=\"radio\" name=\"vswing\" id=\"vs_auto\" value=\"AUTO\"");
            client.print(g_swingV == PANASONIC_AIRCON2_VS_AUTO ? " checked":"");
            client.println("> <label for=\"vswing\">Auto</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"vswing\" id=\"vs_up\" value=\"UP\"");
            client.print(g_swingV == PANASONIC_AIRCON2_VS_UP ? " checked":"");
            client.println("> <label for=\"vs_up\">Up</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"vswing\" id=\"vs_mup\" value=\"MUP\"");
            client.print(g_swingV == PANASONIC_AIRCON2_VS_MUP ? " checked":"");
            client.println("> <label for=\"vs_mup\">Middle Up</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"vswing\" id=\"vs_middle\" value=\"MIDDLE\"");
            client.print(g_swingV == PANASONIC_AIRCON2_VS_MIDDLE ? " checked":"");
            client.println("> <label for=\"vs_middle\">Middle</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"vswing\" id=\"vs_mdown\" value=\"MDOWN\"");
            client.print(g_swingV == PANASONIC_AIRCON2_VS_MDOWN ? " checked":"");
            client.println("> <label for=\"vs_mdown\">Middle Down</label>&nbsp;&nbsp;");

            client.print("<input type=\"radio\" name=\"vswing\" id=\"vs_down\" value=\"DOWN\"");
            client.print(g_swingV == PANASONIC_AIRCON2_VS_DOWN ? " checked":"");
            client.println("> <label for=\"vs_down\">Down</label></p>");

            // Temperature

            client.println("<p><b>Temperature</b>&nbsp;");
            client.print("<input type='number' name='temperature' step='1' min='16.0' max='30.0' pattern='\d' value='");
            client.print(g_temperature);
            client.println("'>&#x2103;</p>");

            client.print("<input type=\"submit\" value=\"Send\">");
            client.print("</form");

            client.println("</BODY>");

            // The HTTP response ends with another blank line:
            client.println();
            // break out of the while loop:
            break;
          }
          else
          {    // if you got a newline, then clear currentLine:
            Serial.print("Get line: ");
            Serial.println(currentLine);
            // I wish I could use regex here: 'GET .*action\?'
            if (   strncmp(currentLine.c_str(),"GET ",4) == 0
                && strstr(currentLine.c_str(),"action?") != NULL)
            {
              Serial.println("And action!");
              // Check if anything was changed
              if (currentLine.indexOf("power=ON")  > 0 ) switchOnOff(PANASONIC_AIRCON2_MODE_ON);
              if (currentLine.indexOf("power=OFF") > 0 ) switchOnOff(PANASONIC_AIRCON2_MODE_OFF);

              if (currentLine.indexOf("mode=AUTO") > 0 ) switchMode(PANASONIC_AIRCON2_MODE_AUTO);
              if (currentLine.indexOf("mode=HEAT") > 0 ) switchMode(PANASONIC_AIRCON2_MODE_HEAT);
              if (currentLine.indexOf("mode=COOL") > 0 ) switchMode(PANASONIC_AIRCON2_MODE_COOL);
              if (currentLine.indexOf("mode=DRY")  > 0 ) switchMode(PANASONIC_AIRCON2_MODE_DRY);

              if (currentLine.indexOf("vswing=AUTO")   > 0 ) switchVSwitch(PANASONIC_AIRCON2_VS_AUTO);
              if (currentLine.indexOf("vswing=UP")     > 0 ) switchVSwitch(PANASONIC_AIRCON2_VS_UP);
              if (currentLine.indexOf("vswing=MUP")    > 0 ) switchVSwitch(PANASONIC_AIRCON2_VS_MUP);
              if (currentLine.indexOf("vswing=MIDDLE") > 0 ) switchVSwitch(PANASONIC_AIRCON2_VS_MIDDLE);
              if (currentLine.indexOf("vswing=MDOWN")  > 0 ) switchVSwitch(PANASONIC_AIRCON2_VS_MDOWN);
              if (currentLine.indexOf("vswing=DOWN")   > 0 ) switchVSwitch(PANASONIC_AIRCON2_VS_DOWN);

              char* temp_str;
              int   temp_val = 0;
              int   temp_idx;
              if ((temp_idx = currentLine.indexOf("temperature=")) > 0 )
              {
                temp_str = currentLine.c_str() + temp_idx;
                Serial.print("temp_str:");
                Serial.println(temp_str);
                if( 1 == sscanf(temp_str,"temperature=%d",&temp_val) )
                {
                  Serial.print("New temperature: ");
                  Serial.println(temp_val);
                  g_temperature = temp_val;
                }
              }

              sendAcState();
            }
            currentLine = "";
          }
        }
        else if (c != '\r')      // if you got anything else but a carriage return character,
        {
          currentLine += c;      // add it to the end of the currentLine
        }


      }
    }
    // close the connection:
    client.stop();
    Serial.println("client disconnected");
  }
}

