// This flag should be set when debugging the platform.
//#define DEBUG
// This flag should be enabled to disable real SMS sending
//#define DONT_SEND_REAL_SMS
// Enable this flag to disable the modem between detections.
// Not fully tested yet
//#define DISABLE_MODEM_WHILE_WAITING

#define hostSerial      Serial
#define gprsSerial      Serial1

#define GPRS_POWER_PIN    9    // Enables or disables power to the GPRS module
#define GPRS_STATUS_PIN   6    // Status pin from the GPRS module
#define LASER_SENSOR_PIN  5    // Connected to the laser module
#define SMS_NUMBER_PIN    4    // Used to choose between to SMS numbers
#define BEEP_PIN          3    // PWM based beeper pin
#define ADJUSTMENT_LED    10   // External indicator LED.
#define BLUE_LED          13   // Internal LED, kept off at all times

char buffer[64]; // buffer array for data recieve over serial port

boolean linkEstablished = false;
boolean messageInhibit = false;

char *STR_CALL_READY    = "Call Ready";
char *STR_OK            = "OK";
#define   STR_MESSAGE   "Ett mess fr\xe5n I V\xe5tt och Torrts entreklocka !" \
                        "N\xe5gon \xe4r h\xe4r, Skynda skynda =)"
#define   STR_WARNING   "Lasersignalen \xe4 borta, v\xe4nligen kontrollera systemet !"
#define   STR_WELCOME   "Nu har passersystemet startat !"
#define   STR_HEARTBEAT "Hej, jag lever =)"
#define   SMS_STRING1   "AT+CMGS = \"+46768582241\""    /* Carina */
#define   SMS_STRING2   "AT+CMGS = \"+46768582240\""    /* Pontus */

// Laser beam variables and state definitions
#define LASER_STATE_INACTIVE     0
#define LASER_STATE_DETECTING    1
#define LASER_STATE_DETECTED     2
#define LASER_STATE_GUARD        3
#define LASER_STATE_GUARD_START  4
#define LASER_STATE_GUARD_STOP   5

#define LASER_DETECTION_GUARD    5000
#define LASER_BLOCKED_TIMEOUT    120000
#define LASER_MIN_HIGH_STATE     35
#if defined(DEBUG)
  #define LASER_ADJUST_LOCK_TIME 2000
#else
  #define LASER_ADJUST_LOCK_TIME 10000
#endif
#define SMS_RESPONSE_TIMEOUT     60000
#define HEARTBEAT_TIMEOUT        1800000

byte laserState = LASER_STATE_INACTIVE;
byte oldLaserPin;
boolean laserBlockTimerEnabled = false;
uint32_t laserBlockTimer;
uint32_t laserTrigTimer;
uint32_t laserPresentTimer;
uint32_t heartbeatTimer;

/***************************************************************************
 *
 * Setup system
 *
 **************************************************************************/
void setup()
{
  // Set fixed pin modes
  pinMode(GPRS_POWER_PIN, OUTPUT);
  pinMode(GPRS_STATUS_PIN, INPUT);
  pinMode(LASER_SENSOR_PIN, INPUT_PULLUP);  // Has strong (4.7K) external pull up
  pinMode(BLUE_LED, OUTPUT);
  pinMode(ADJUSTMENT_LED, OUTPUT);
  pinMode(SMS_NUMBER_PIN, INPUT_PULLUP);
  pinMode(BEEP_PIN, OUTPUT);
  
  oldLaserPin = digitalRead(LASER_SENSOR_PIN);
  laserBlockTimer = millis();
  
  // Default setting for GPRS power pin
  digitalWrite(GPRS_POWER_PIN, LOW);
  
  // Default blue led
  digitalWrite(BLUE_LED, LOW);
  // Adjustment indicator LED. This LED is pulled by the MCU so it is 
  // inverted in function.
  digitalWrite(ADJUSTMENT_LED, HIGH);
  
  // Serial speeds
  hostSerial.begin(115200);    // USB on Leonardo
  gprsSerial.begin(19200);     // GPRS module
  
#if defined(DEBUG)
  while (!hostSerial);
#endif
  hostSerial.println(F("Testing the GPRS sheild !"));
  
  // Do trims first
  doTrimUnits();

  // First make sure the modem is powered off.
  modemShutDown();
  flushGprsSerial();
  // Now turn it on.
  modemPowerOn();
  
  delay(100);
  sendSms(0);
  
  laserPresentTimer = millis() + LASER_BLOCKED_TIMEOUT;

  // Setup heartbeat timer and callback
  heartbeatTimer = millis();

#if defined(DISABLE_MODEM_WHILE_WAITING)
  // But we don't want the modem on line all the time
  modemShutDown();
#endif
}

/***************************************************************************
 *
 * Turns on the beeper and returns if the duration is equal to zero.
 * If duration is set it will wait for the specified time and then return.
 *
 **************************************************************************/
void beepOn(uint8_t freq, uint8_t duration)
{
  analogWrite(BEEP_PIN, freq);
  if (duration) {
    delay(duration);
  }
}

/***************************************************************************
 *
 * Turns beeper off and return.
 *
 **************************************************************************/
void beepOff(void)
{
  analogWrite(BEEP_PIN, 0);
}

/***************************************************************************
 *
 * This function will wait for the units to lock for more than 15 seconds
 * before continuing to execute the main function.
 * This allows the user to adjust the position of the transitter and the
 * received.
 *
 **************************************************************************/
void doTrimUnits(void)
{
  boolean timeReached;
  uint32_t timer;
  uint32_t beepTimer;
  boolean beepon = false;
  
  hostSerial.println(F("Make your adjustments ladies."));
  
  do {
    digitalWrite(ADJUSTMENT_LED, HIGH);
    // Loop until a trigger is found
    while (digitalRead(LASER_SENSOR_PIN));
    
    timer = millis() + LASER_ADJUST_LOCK_TIME;
    beepTimer = millis();
    digitalWrite(ADJUSTMENT_LED, LOW);
    
    while (millis() < timer) {
      if (digitalRead(LASER_SENSOR_PIN))
        break;
      // Make some noice
      if (millis() >= beepTimer) {
        if (beepon) {
          beepOff();
          beepon = false;
        } else {
          beepOn(50, 0);
          beepon = true;
        }
        beepTimer = millis() + 200;
      }
      delay(2);
    }
  // Make sure sound is off
    beepOff();
    delay(1);
  } while (millis() < timer);

  // Shut of LED to save power
  digitalWrite(ADJUSTMENT_LED, HIGH);
  hostSerial.println(F("Adjustment is now fixed, starting main function."));
}  

/***************************************************************************
 *
 * Make sure the serial receive buffer from the GPRS module is empty
 *
 **************************************************************************/
void flushGprsSerial(void)
{
  while(gprsSerial.available()>0) gprsSerial.read();
}

/***************************************************************************
 *
 * Shut down the modem
 *
 **************************************************************************/
void modemShutDown(void)
{
  hostSerial.println(F("Checking modem status !"));
  if (digitalRead(GPRS_STATUS_PIN)) {
    hostSerial.println(F("Modem is online, so shutting it down"));
    delay(100);
    digitalWrite(GPRS_POWER_PIN, HIGH);
    while (digitalRead(GPRS_STATUS_PIN));
    digitalWrite(GPRS_POWER_PIN, LOW);
    delay(100);
  }
}

/***************************************************************************
 *
 * Power the modem on
 *
 **************************************************************************/
void modemPowerOn(void)
{
  hostSerial.println(F("Checking modem status !"));
  if (!digitalRead(GPRS_STATUS_PIN)) {
    hostSerial.println(F("Modem is off line, so starting it up !"));
    digitalWrite(GPRS_POWER_PIN, HIGH);
    while (!digitalRead(GPRS_STATUS_PIN));
    digitalWrite(GPRS_POWER_PIN, LOW);
    delay(100);
    
    // Look for start condition
    while (1) {
      gprsWaitForCmd((char *)&buffer);
#if defined(DEBUG)
      hostSerial.print(F("Received command "));
      hostSerial.print((char *)&buffer);
      hostSerial.println(F(" from SIM 900"));
#endif
      // Check if it is our wake up command
      if (strncmp((const char *)buffer, STR_CALL_READY, strlen(STR_CALL_READY)) == 0) {
        linkEstablished = true;
        break;
      }
    }
    hostSerial.println(F("Connected !!"));
    turnOffEcho();
  }
}

/***************************************************************************
 *
 * Wait for a character to come in on the gprs serial channer
 * and then return that character.
 *
 **************************************************************************/
byte getChar(void)
{
  while (!gprsSerial.available());
  return gprsSerial.read();
}

/***************************************************************************
 *
 * Send a command to the gprs modeul and wait for OK
 *
 **************************************************************************/
void gprsSendAndWait(const __FlashStringHelper *str, int timeout)
{
  uint32_t timer = millis() + timeout;
  static uint32_t seq = 1;
  byte ch;
  
  hostSerial.print(seq);
  hostSerial.print(F("  "));
  
  gprsSerial.print(str);     // Send command
  gprsSerial.write('\r');    // Terminate command
  
  // Wait for the first character in the response
  while (!gprsSerial.available() || millis() < timer);
  
  do {
    ch = gprsSerial.read();
    hostSerial.write(ch);
  } while (gprsSerial.available());
  
  seq++;
  
  hostSerial.println();
}

/***************************************************************************
 *
 * Wait for a command from the GPRS module.
 *
 **************************************************************************/
void gprsWaitForCmd(char *buf)
{
  byte ch;
  boolean first_crlf = true;
  
  // This will loop for the entire command
  while(1) {
    if (gprsSerial.available()) {
      ch = getChar();
      if (ch == '\r') {                // Is it CR ?
        ch = getChar();                // Consume LF
        *buf = 0;
        if (!first_crlf)
          return;
        first_crlf = false;
      } else {
        *buf++ = ch;
      }
    }
  }
}

/***************************************************************************
 *
 * Wait for OK to be returned from the GPRS module
 *
 **************************************************************************/
boolean gprsWaitForOK()
{
  gprsWaitForCmd((char *)&buffer);
  if (strncmp((const char *)buffer, STR_OK, strlen(STR_OK)) == 0) {
    return true;
  }
  return false;
}

/***************************************************************************
 *
 * Tell the modem to turn off the AT echo
 *
 **************************************************************************/
void turnOffEcho(void)
{
  gprsSerial.print(F("ATE0\r"));
  delay(250);
  // Make sure input channel is empty before returning
  while(gprsSerial.available()>0) gprsSerial.read();
}

/***************************************************************************
 *
 * Send an sms to the predefined subscriber
 *
 **************************************************************************/
int sendSms(byte msg)
{
#if !defined(DONT_SEND_REAL_SMS)
  uint32_t timeout;
  boolean isOk = false;

#if defined(DISABLE_MODEM_WHILE_WAITING)
  modemPowerOn();
#endif
  hostSerial.println("Sending Text...");
  
  gprsSerial.print("AT+CMGF=1\r"); // Set the shield to SMS mode
  if (!gprsWaitForOK()) goto error;
  gprsSerial.print(F("AT+CSCS=\"8859-1\"\r"));
  if (!gprsWaitForOK()) goto error;
  // Select between the two available number
  if (!digitalRead(SMS_NUMBER_PIN)) {
    gprsSerial.println(F(SMS_STRING1));
#if defined(DEBUG)
    hostSerial.println(F(SMS_STRING1));
#endif
  } else {
    gprsSerial.println(F(SMS_STRING2));
#if defined(DEBUG)
    hostSerial.println(F(SMS_STRING2));
#endif
  }
  delay(100);
  switch(msg) {
    case 0:
      gprsSerial.println(F(STR_WELCOME));
      break;
      
    case 1:
      gprsSerial.println(F(STR_MESSAGE));
      break;
      
    case 2:
      gprsSerial.println(F(STR_WARNING));
      break;

    case 3:
      gprsSerial.println(F(STR_HEARTBEAT));
      break;
    
    default:
      hostSerial.println(F("Unknown string, this is a BUG !"));
  }
  delay(100);
  gprsSerial.print((char)26);//the ASCII code of the ctrl+z is 26 (required according to the datasheet)
  delay(100);
  gprsSerial.println();

  hostSerial.println(F("Waiting for response !"));
  
  timeout = millis() + SMS_RESPONSE_TIMEOUT;
  while(millis() < timeout) {
    gprsWaitForCmd((char *)&buffer);
    if (strncmp((const char *)buffer, STR_OK, strlen(STR_OK)) == 0) {
      isOk = true;
      break;
    }
  }
  // Make sure the OK came
  if (!isOk) {
     hostSerial.println(F("Timeout while sending SMS !"));
     goto error;
  }
  
#if defined(DISABLE_MODEM_WHILE_WAITING)
  modemShutDown();
#endif
  return 0;
error:

#if defined(DISABLE_MODEM_WHILE_WAITING)
  modemShutDown();
#endif
  return 1;
#else
  hostSerial.println("Emulating Sending Text...");
  // Select between the two available number
  hostSerial.print(F("Sending to SMS number: "));
  if (!digitalRead(SMS_NUMBER_PIN)) {
    hostSerial.println(F(SMS_STRING1));
  } else {
    hostSerial.println(F(SMS_STRING2));
  }
  delay(1000);
  return 0;
#endif
}

/***************************************************************************
 *
 * The main dispatching loop
 *
 **************************************************************************/
void loop()
{
  if (millis() - heartbeatTimer > HEARTBEAT_TIMEOUT) {
    heartbeatTimer = millis();
    // Send a heartbeat to the user.
    sendSms(3);
  }
  
  // Check the state to see if we should reset the laser present counter
  if (digitalRead(LASER_SENSOR_PIN) != oldLaserPin) {
    hostSerial.println(F("Detected a state change on the laser pin"));
    laserPresentTimer = millis() + LASER_BLOCKED_TIMEOUT;
  } else {
    // Send a warning to the user if we have been without the laser beam
    // for to long.
    if (digitalRead(LASER_SENSOR_PIN)) {
      if (millis() >= laserPresentTimer) {
        hostSerial.println(F("Detected that the laser has been gone for to long"));
        sendSms(2);
        laserPresentTimer = millis() + LASER_BLOCKED_TIMEOUT;
        laserState = LASER_STATE_INACTIVE;
      }
    } else {
      // While the laser signal is present we always set a new timeout.
      laserPresentTimer = millis() + LASER_BLOCKED_TIMEOUT;
    }
  }
  switch (laserState) {
    case LASER_STATE_INACTIVE:
      if (digitalRead(LASER_SENSOR_PIN) && !oldLaserPin) {
        // The adjustment LED seconds as a detect LED
        digitalWrite(ADJUSTMENT_LED, LOW);
        laserState = LASER_STATE_DETECTING;
        laserTrigTimer = millis() + LASER_MIN_HIGH_STATE;
      }
      break;
      
    case LASER_STATE_DETECTING:
      if (!digitalRead(LASER_SENSOR_PIN) && oldLaserPin) {
        // Make sure this state has been held long enough
        if (millis() >= laserTrigTimer) {
          // The timer time was met, goto next state
          laserState = LASER_STATE_DETECTED;
        } else {
          // This happens of the trigger wasn't long enough
          laserState = LASER_STATE_INACTIVE;
          hostSerial.println(F("To short activation detected !"));
          // Disable the LED before going inactive again.
          digitalWrite(ADJUSTMENT_LED, HIGH);
        }
      }
      break;
      
    case LASER_STATE_DETECTED:
      digitalWrite(ADJUSTMENT_LED, HIGH);
      hostSerial.println(F("Dude, someone passed the line"));
      if (sendSms(1)) {
        hostSerial.println(F("Failed to send SMS to client"));
      } else {
        hostSerial.println(F("SMS sent to client"));
      }
      // Set a timeout so we don't get a new detection right away.
      laserBlockTimer = millis() + LASER_DETECTION_GUARD;
      laserState = LASER_STATE_GUARD_START;
      break;
      
    case LASER_STATE_GUARD_START:
      hostSerial.println(F("Starting guard timeout !"));
      laserState = LASER_STATE_GUARD;
      break;
      
    case LASER_STATE_GUARD:
      if (millis() > laserBlockTimer) {
        laserState = LASER_STATE_GUARD_STOP;
      }
#if defined(DEBUG)      
      if (!(millis() % 100)) {
        if (digitalRead(ADJUSTMENT_LED))
          digitalWrite(ADJUSTMENT_LED, LOW);
        else
          digitalWrite(ADJUSTMENT_LED, HIGH);
      }
#endif
      break;
      
    case LASER_STATE_GUARD_STOP:
      hostSerial.println(F("Ending guard timeout !"));
      laserState = LASER_STATE_INACTIVE;
      digitalWrite(ADJUSTMENT_LED, HIGH);
      break;

    default:
      hostSerial.println(F("Incorrect state detected"));
      break;      
  }
  oldLaserPin = digitalRead(LASER_SENSOR_PIN);
  // To get reliable readings from the GPIO port this has been insterted.
  delay(1);
    
  if (gprsSerial.available()) {
    // Read a command from the gprs module.
    hostSerial.write(gprsSerial.read());
  }
  
  if (hostSerial.available()) {
    gprsSerial.write(hostSerial.read());
  }
}
