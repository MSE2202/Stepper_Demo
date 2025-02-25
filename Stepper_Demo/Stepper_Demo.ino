// 
// MSE 2202 stepper motor example
// 
//  Language: Arduino (C++)
//  Target:   ESP32
//  Author:   Michael Naish
//  Date:     2025 02 25
//
//  Note that this code does not implement any acceleration or deceleration. The stepper may "lock up" if the
//  direction is changed when the motor is spinning quickly or if the speed increases too quickly.
//

#define STEP_OUTPUT_ON                                                 // uncomment to turn on output of stepper motor information
// #define POT_OUTPUT_ON                                                  // uncomment to turn on output of pot values

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// Function declarations
void doHeartbeat();
void ARDUINO_ISR_ATTR buttonISR(void* arg);
void ARDUINO_ISR_ATTR timerISR();

// Button structure
struct Button {
  const int pin;                                                       // GPIO pin for button
  uint32_t numberPresses;                                              // counter for number of button presses
  uint32_t nextPressTime;                                              // time of next allowable press in milliseconds
  bool pressed;                                                        // flag for button press event
};

// Constants
const int cHeartbeatInterval = 75;                                     // heartbeat update interval, in milliseconds
const int cSmartLED          = 23;                                     // when DIP switch S1-4 is on, SMART LED is connected to GPIO21
const int cSmartLEDCount     = 1;                                      // number of Smart LEDs in use
const int cPotPin            = 36;                                     // when DIP switch S1-3 is on, pot (R1) is connected to GPIO36 (ADC1-0)
const int cToggle1           = 39;                                     // DIP switch S1-1 turns stepper on/off  
const int cStepPin           = 19;                                     // GPIO pin for step signal to A4988
const int cDirPin            = 18;                                     // GPIO pin for direction signal to A4988
const int cStepRes           = 8;                                      // bit resolution for stepper PWM stepper
const int cStepFreq          = 100;                                    // initial frequency of stepper PWM
const long cDebounceDelay    = 20;                                     // button debounce delay in milliseconds

// Variables
boolean heartbeatState       = true;                                   // state of heartbeat LED
unsigned long lastHeartbeat  = 0;                                      // time of last heartbeat state change
unsigned long curMillis      = 0;                                      // current time, in milliseconds
unsigned long prevMillis     = 0;                                      // start time for delay cycle, in milliseconds
Button button                = {0, 0, 0, false};                       // NO pushbutton PB1 on GPIO 0, low state when pressed
hw_timer_t * pTimer          = NULL;                                   // pointer to timer used by timer interrupt
boolean stepDir              = true;                                   // step direction
volatile int32_t stepCount   = 0;                                      // number of steps
boolean runState             = false;                                  // 0 = stopped; 1 = running

// Declare SK6812 SMART LED object
//   Argument 1 = Number of LEDs (pixels) in use
//   Argument 2 = ESP32 pin number 
//   Argument 3 = Pixel type flags, add together as needed:
//     NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//     NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
//     NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
//     NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
//     NEO_RGBW    Pixels are wired for RGBW bitstream (NeoPixel RGBW products)
Adafruit_NeoPixel SmartLEDs(cSmartLEDCount, cSmartLED, NEO_RGB + NEO_KHZ800);

// Smart LED brightness for heartbeat
unsigned char LEDBrightnessIndex = 0; 
unsigned char LEDBrightnessLevels[] = {0, 0, 0, 5, 15, 30, 45, 60, 75, 90, 105, 120, 135, 
                                       150, 135, 120, 105, 90, 75, 60, 45, 30, 15, 5, 0};

void setup() {
  Serial.begin(115200);

  // Set up SmartLED
  SmartLEDs.begin();                                                   // initialize smart LEDs object
  SmartLEDs.clear();                                                   // clear pixel
  SmartLEDs.setPixelColor(0, SmartLEDs.Color(0,0,0));                  // set pixel colours to black (off)
  SmartLEDs.setBrightness(0);                                          // set brightness [0-255]
  SmartLEDs.show();                                                    // update LED

  pinMode(cToggle1, INPUT_PULLUP);                                     // configure GPIO to turn stepper on/off 
  pinMode(button.pin, INPUT_PULLUP);                                   // configure GPIO for button pin as an input with pullup resistor
  attachInterruptArg(button.pin, buttonISR, &button, FALLING);         // configure ISR to trigger on low signal on pin
  pinMode(cPotPin, INPUT);                                             // configure potentiometer pin for input
  pinMode(cStepPin, OUTPUT);                                           // assign output for step signal to A4988
  pinMode(cDirPin, OUTPUT);                                            // assign output for direction signal to A4988

  pTimer = timerBegin(1000000);                                        // start timer with 1 MHz frequency
  timerAttachInterrupt(pTimer, &timerISR);                             // configure timer ISR
  timerAlarm(pTimer, 500, true, 0);                                    // set initial interrupt time (in microseconds), set to repeat indefinitely
}                

void loop() {
  runState = !digitalRead(cToggle1);                                   // read switch to determine run state (low when "on")

  if (button.pressed) {                                                // reverse step direction with each button press
    stepDir ^= 1; 
    button.pressed = false;
  }
  digitalWrite(cDirPin, stepDir);                                      // set direction pin

  int speedPot = analogRead(cPotPin);                                  // read speed pot value (between 0 and 4095)
  unsigned long stepRate = map(speedPot, 0, 4095, 500, 60000);         // map to half period in microseconds
#ifdef POT_OUTPUT_ON
  Serial.printf("Pot: %d, Period: %lu us\n", speedPot, stepRate * 2);
#endif
  timerAlarm(pTimer, stepRate, true, 0);                               // update interrupt period to adjust step frequency
#ifdef STEP_OUTPUT_ON
  long freq = 1000000 / (stepRate * 2);                                // convert step rate to frequency in Hz
  Serial.printf("Dir: %d, rate: %lu, freq: %ld Hz, count: %ld\n", stepDir, stepRate * 2, freq, stepCount);
#endif
  
  doHeartbeat();                                                       // update heartbeat LED
}

// update heartbeat LED
void doHeartbeat() {
  curMillis = millis();                                                // get the current time in milliseconds
  // check to see if elapsed time matches the heartbeat interval
  if ((curMillis - lastHeartbeat) > cHeartbeatInterval) {
    lastHeartbeat = curMillis;                                         // update the heartbeat time for the next update
    LEDBrightnessIndex++;                                              // shift to the next brightness level
    if (LEDBrightnessIndex > sizeof(LEDBrightnessLevels)) {            // if all defined levels have been used
      LEDBrightnessIndex = 0;                          // reset to starting brightness
    }
    SmartLEDs.setBrightness(LEDBrightnessLevels[LEDBrightnessIndex]);  // set brightness of heartbeat LED
    SmartLEDs.setPixelColor(0, SmartLEDs.Color(0, 250, 0));            // set pixel colours to green
    SmartLEDs.show();                                                  // update LED
  }
}

// button interrupt service routine
// argument is pointer to button structure, which is statically cast to a Button structure, 
// allowing multiple instances of the buttonISR to be created (1 per button)
void ARDUINO_ISR_ATTR buttonISR(void* arg) {
  Button* s = static_cast<Button*>(arg);                               // cast pointer to static structure

  uint32_t pressTime = millis();                                       // capture current time
  if (pressTime > s->nextPressTime) {                                  // if enough time has passed to consider a valid press
    s->numberPresses += 1;                                             // increment button press counter
    s->pressed = true;                                                 // indicate valid button press state
    s->nextPressTime = pressTime + cDebounceDelay;                     // update time for next valid press
  }
}

// timer interrupt service routine
void ARDUINO_ISR_ATTR timerISR() {
  if (runState) {                                                      // only send pulse if motor should be running
    digitalWrite(cStepPin, !digitalRead(cStepPin));                    // toggle state of step pin
    if (stepDir) {
      stepCount++;                                                     // add to count in forward direction
    }
    else {
      stepCount--;                                                     // subtract from count in reverse direction
    }
  }
}
