/*******************************************************************************
 * This file is part of OpenPSX2AmigaPadAdapter.                               *
 *                                                                             *
 * Copyright (C) 2019 by SukkoPera <software@sukkology.net>                    *
 *                                                                             *
 * OpenPSX2AmigaPadAdapter is free software: you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or           *
 * (at your option) any later version.                                         *
 *                                                                             *
 * OpenPSX2AmigaPadAdapter is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of              *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the               *
 * GNU General Public License for more details.                                *
 *                                                                             *
 * You should have received a copy of the GNU General Public License           *
 * along with OpenPSX2AmigaPadAdapter. If not, see http://www.gnu.org/licenses.*
 *******************************************************************************
 *
 * OpenPSX2AmigaPadAdapter - Playstation/Playstation 2 to Commodore Amiga/CD32
 * controller adapter
 *
 * Please refer to the GitHub page and wiki for any information:
 * https://github.com/SukkoPera/OpenPSX2AmigaPadAdapter
 *
 * CD32 pad protocol information found at:
 * http://gerdkautzmann.de/cd32gamepad/cd32gamepad.html
 */

#include <PS2X_lib.h>

// INPUT pins, connected to PS2 controller
const byte PS2_CLK = 13;
const byte PS2_DAT = 12;
const byte PS2_CMD = 11;
const byte PS2_SEL = 10;

// PS2 Controller Class
PS2X ps2x;

// OUTPUT pins, connected to Amiga port
const byte PIN_UP = 4;    // Amiga Pin 1
const byte PIN_DOWN = 5;  // Amiga Pin 2
const byte PIN_LEFT = 6;  // Amiga Pin 3
const byte PIN_RIGHT = 7; // Amiga Pin 4
const byte PIN_BTN1 = 3;  // Amiga Pin 6
const byte PIN_BTN2 = 8;  // Amiga Pin 9

// This pin switches between Amiga (HIGH) and CD32 (LOW) mode
// It also triggers the loading of the button status shift register
const byte PIN_PADMODE = 2; // Amiga Pin 5

/* When in CD32 mode, button status is saved to an 8-bit register that gets
 * shifted out one bit at a time through this pin.
 */
const byte PIN_BTNREGOUT = PIN_BTN2;

// The shifting is clocked by rising edges on this pin
const byte PIN_BTNREGCLK = PIN_BTN1;

// Analog sticks idle value
const byte ANALOG_IDLE_VALUE = 127;

// Dead zone for analog sticks
const byte ANALOG_DEAD_ZONE = 65;

// Delay of the quadrature square waves when mouse is moving at the slowest speed
const byte MOUSE_SLOW_DELTA	= 60;

/* Delay of the quadrature square waves when mouse is moving at the fastest speed.
 * Note that a 16 MHz Arduino Uno produces irregular signals if this is too small
 * and mouse movements will be affected. The smallest value producing decently-
 * shaped waves is 6.
 */
const byte MOUSE_FAST_DELTA = 6;

// Pin for led that lights up whenever the proper controller is detected
const byte PIN_LED_PAD_OK = A1;

// Pin for led that lights up whenever the adapter is in CD32 mode
const byte PIN_LED_MODE_CD32 = A0;

/* Timeout for CD32 mode: normal joystick mode will be entered if PIN_PADMODE is
 * not toggled for this amount of milliseconds.
 */
const byte TIMEOUT_CD32_MODE = 200;

/*******************************************************************************
 * DEBUGGING SUPPORT
 ******************************************************************************/

// Send debug messages to serial port
//~ #define ENABLE_SERIAL_DEBUG

// Print the controller status on serial. Useful for debugging.
//~ #define DEBUG_PAD

/*******************************************************************************
 * END OF SETTINGS
 ******************************************************************************/

enum PadMode {
	MODE_JOYSTICK,
	MODE_MOUSE,
	MODE_CD32
};

// Start out as a simple joystick
volatile PadMode mode = MODE_JOYSTICK;

enum PadError {
	PADERR_NONE = 0,		// AKA No error
	PADERR_NOTFOUND,
	PADERR_WRONGTYPE,
	PADERR_UNKNOWN
};

// Button bits for CD32 mode
const word BTN_BLUE =		1U << 0U;
const word BTN_RED =		1U << 1U;
const word BTN_YELLOW =		1U << 2U;
const word BTN_GREEN =		1U << 3U;
const word BTN_FRONT_R =	1U << 4U;
const word BTN_FRONT_L =	1U << 5U;
const word BTN_START =		1U << 6U;

// This is only used for blinking the led when mapping is changed
enum JoyButtonMapping {
	JMAP_NORMAL = 1,
	JMAP_RACING1,
	JMAP_RACING2,
	JMAP_PLATFORM
};

/* Structure representing a standard 2-button joystick, used for gathering
 * button presses according to different button mappings. True means pressed.
 */
struct TwoButtonJoystick {
	boolean up: 1;
	boolean down: 1;
	boolean left: 1;
	boolean right: 1;
	boolean b1: 1;
	boolean b2: 1;
};

// Button mapping function
typedef void (*JoyMappingFunc) (TwoButtonJoystick& j);

// Default button mapping function
void mapJoystickNormal (TwoButtonJoystick& j);
JoyMappingFunc joyMappingFunc = mapJoystickNormal;

// True if mouse mode was enabled before switching to CD32 mode
boolean wasMouse = false;

/* Button register currently being shifted in ISRs
 * 0 means pressed
 */
volatile byte buttons;

/* Button register being updated
 * 0 means pressed, MSB must be 1 for ID sequence (for the very first report)
 */
byte buttonsLive = 0x80;

// Timestamp of last time the pad was switched out of CD32 mode
unsigned long lastSwitchedTime = 0;

#ifdef ENABLE_SERIAL_DEBUG
	#define dstart(spd) Serial.begin (spd)
	#define debug(...) Serial.print (__VA_ARGS__)
	#define debugln(...) Serial.println (__VA_ARGS__)
#else
	#define dstart(...)
	#define debug(...)
	#define debugln(...)
#endif

PadError initPad () {
	PadError ret = PADERR_NOTFOUND;

	// clock, command, attention, data, Pressures?, Rumble?
	int error = ps2x.config_gamepad (PS2_CLK, PS2_CMD, PS2_SEL, PS2_DAT, false, false);

	switch (error) {
	case 0:
		// Controller is ready
		switch (ps2x.readType ()) {
		case 0:
			debugln (F("Unknown Controller type found"));
			// Well, it might work
			ret = PADERR_NONE;
			break;
		case 1:
			debugln (F("DualShock Controller found"));
			ret = PADERR_NONE;
			break;
		case 2:
			debugln (F("GuitarHero Controller found"));
			//~ ret = PADERR_WRONGTYPE;
			ret = PADERR_NONE;
			break;
		case 3:
			debugln (F("Wireless Sony DualShock Controller found"));
			ret = PADERR_NONE;
			break;
		}
		break;
	case 1:
		debugln (F("No controller found, check wiring"));
		ret = PADERR_NOTFOUND;
		break;
	case 2:
		debugln (F("Controller found but not accepting commands"));
		ret = PADERR_UNKNOWN;
		break;
	}

	return ret;
}

// FIXME
void dumpButtons () {
#ifdef DEBUG_PAD
	static word last_pad_status = 0;

	if (pad_status != last_pad_status) {
		debug (F("Pressed: "));
		if (pad_status & BTN_UP)
			debug (F("Up "));
		if (pad_status & BTN_DOWN)
			debug (F("Down "));
		if (pad_status & BTN_LEFT)
			debug (F("Left "));
		if (pad_status & BTN_RIGHT)
			debug (F("Right "));
		if (pad_status & BTN_RED)
			debug (F("Red "));
		if (pad_status & BTN_BLUE)
			debug (F("Blue "));
		if (pad_status & BTN_YELLOW)
			debug (F("Yellow "));
		if (pad_status & BTN_GREEN)
			debug (F("Green "));
		if (pad_status & BTN_FRONT_L)
			debug (F("FrontL "));
		if (pad_status & BTN_FRONT_R)
			debug (F("FrontR "));
		if (pad_status & BTN_START)
			debug (F("Start "));
		debugln ();

		last_pad_status = pad_status;
	}
#endif
}

// ISR
void onPadModeChange () {
	if (digitalRead (PIN_PADMODE) == LOW) {
		if (mode != MODE_CD32) {
			// Switch to CD32 mode
			toCD32 ();
		}
		
		// Sample input values, they will be shifted out on subsequent clock inputs
		//~ buttons = buttonsLive | 0x80;   // Make sure bit MSB is 1 for ID sequence
		buttons = buttonsLive;		/* The above is now handled elsewhere so that
									 * here we can run as fast as possible
									 */

		// Output first bit immediately
		digitalWrite (PIN_BTNREGOUT, buttons & 0x01);
		buttons >>= 1;	/* MSB will be zeroed during shifting, this will report
						 * non-existing button 9 as pressed for the ID sequence
						 */
	} else {
		/* Mark time pin went high so that we can see if it goes back high in a
		 * short while
		 */
		lastSwitchedTime = millis ();
	}
}

// ISR
void onClockEdge () {
	digitalWrite (PIN_BTNREGOUT, buttons & 0x01);
	buttons >>= 1;	/* Again, non-existing button 10 will be reported as pressed
	                 * for the ID sequence
	                 */
}

void mypanic (int interval) {
	while (42) {
		digitalWrite (PIN_LED_PAD_OK, HIGH);
		delay (interval);
		digitalWrite (PIN_LED_PAD_OK, LOW);
		delay (interval);
	}
}

void setup () {
	dstart (115200);
	debugln (F("Starting up..."));

	pinMode (PIN_PADMODE, INPUT_PULLUP);
	//~ pinMode (PIN_BTNREGOUT, OUTPUT);
	//~ pinMode (PIN_BTNREGCLK, INPUT);

	// Prepare leds
	pinMode (PIN_LED_PAD_OK, OUTPUT);
	pinMode (PIN_LED_MODE_CD32, OUTPUT);

	// Give wireless PS2 module some time to startup, before configuring it
	delay (300);

	PadError err = initPad ();
	if (err != PADERR_NONE) {
		int interval = 1000;
		switch (err) {
		case PADERR_NOTFOUND:
			debugln (F("No controller found"));
			interval = 200;
			break;
		case PADERR_WRONGTYPE:
			debugln (F("Unsupported controller"));
			interval = 333;
			break;
		case PADERR_UNKNOWN:
		default:
			debugln (F("Cannot initialize controller"));
			interval = 500;
			break;
		}

		mypanic (interval);
	}

	digitalWrite (PIN_LED_PAD_OK, HIGH);

	// Call ISR on changes of the CD32 pad mode pin
	attachInterrupt (digitalPinToInterrupt (PIN_PADMODE), onPadModeChange, CHANGE);
}

void toMouse () {
	debugln (F("To mouse mode"));
	
	// Direction pins must be outputs
	pinMode (PIN_UP, OUTPUT);
	pinMode (PIN_DOWN, OUTPUT);
	pinMode (PIN_LEFT, OUTPUT);
	pinMode (PIN_RIGHT, OUTPUT);

	// We're not going to care for clock pulses anymore
	detachInterrupt (digitalPinToInterrupt (PIN_BTNREGCLK));
	
	mode = MODE_MOUSE;
}

void toJoystick () {
	debugln (F("To joystick mode"));
	
	// All pins to Hi-Z without pull-up
	digitalWrite (PIN_UP, LOW);
	pinMode (PIN_UP, INPUT);
	digitalWrite (PIN_DOWN, LOW);
	pinMode (PIN_DOWN, INPUT);
	digitalWrite (PIN_LEFT, LOW);
	pinMode (PIN_LEFT, INPUT);
	digitalWrite (PIN_RIGHT, LOW);
	pinMode (PIN_RIGHT, INPUT);
	
	detachInterrupt (digitalPinToInterrupt (PIN_BTNREGCLK));

	mode = MODE_JOYSTICK;
}

void toCD32 () {
	debugln (F("To CD32 mode"));
		
	// Pin 9 becomes our data output
	pinMode (PIN_BTNREGOUT, OUTPUT);
	
	// Pin 6 becomes an input for clock
	pinMode (PIN_BTNREGCLK, INPUT);
	attachInterrupt (digitalPinToInterrupt (PIN_BTNREGCLK), onClockEdge, RISING);
	
	if (mode == MODE_MOUSE)
		wasMouse = true;
	else
		wasMouse = false;
		
	mode = MODE_CD32;
}

inline void buttonPress (byte pin) {
	/* Drive pins in open-collector style, so that we are compatible with the
	 * C64 too
	 */
	pinMode (pin, OUTPUT);  // Low is implicit
}

inline void buttonRelease (byte pin) {
	pinMode (pin, INPUT); // Hi-Z
}

void mapAnalogStickHorizontal (TwoButtonJoystick& j) {
	int lx = ps2x.Analog (PSS_LX);   			// 0 ... 255
	int deltaLX = lx - ANALOG_IDLE_VALUE;		// --> -127 ... +128
	debug (F("Analog X = "));
	debugln (deltaLX);
	j.left = deltaLX < -ANALOG_DEAD_ZONE;
	j.right = deltaLX > +ANALOG_DEAD_ZONE;
}

void mapAnalogStickVertical (TwoButtonJoystick& j) {
	int ly = ps2x.Analog (PSS_LY);
	int deltaLY = ly - ANALOG_IDLE_VALUE;
	debug (F("Analog Y = "));
	debugln (deltaLY);
	j.up = deltaLY < -ANALOG_DEAD_ZONE;
	j.down = deltaLY > +ANALOG_DEAD_ZONE;
}

void mapJoystickNormal (TwoButtonJoystick& j) {
	// Use both analog axes
	mapAnalogStickHorizontal (j);
	mapAnalogStickVertical (j);

	// D-Pad is fully functional as well
	j.up |= ps2x.Button (PSB_PAD_UP);
	j.down |= ps2x.Button (PSB_PAD_DOWN);
	j.left |= ps2x.Button (PSB_PAD_LEFT);
	j.right |= ps2x.Button (PSB_PAD_RIGHT);

	// Square/Rx are button 1
	j.b1 = ps2x.Button (PSB_SQUARE) || ps2x.Button (PSB_R1) || ps2x.Button (PSB_R2) || ps2x.Button (PSB_R3);

	// Cross/Lx are button 1
	j.b2 = ps2x.Button (PSB_CROSS) || ps2x.Button (PSB_L1) || ps2x.Button (PSB_L2) || ps2x.Button (PSB_L3);
}

void mapJoystickRacing1 (TwoButtonJoystick& j) {
	// Use analog's horizontal axis to steer, ignore vertical
	mapAnalogStickHorizontal (j);

	// D-Pad L/R can also be used
	j.left |= ps2x.Button (PSB_PAD_LEFT);
	j.right |= ps2x.Button (PSB_PAD_RIGHT);

	// Use D-Pad U/Square to accelerate and D/Cross to brake
	j.up = ps2x.Button (PSB_PAD_UP) || ps2x.Button (PSB_SQUARE);
	j.down = ps2x.Button (PSB_PAD_DOWN) || ps2x.Button (PSB_CROSS);
	
	/* Games probably did not expect up + down at the same time, so when
	 * braking, don't accelerate
	 */
	if (j.down)
		j.up = false;

	// Triangle/Rx are button 1
	j.b1 = ps2x.Button (PSB_TRIANGLE) || ps2x.Button (PSB_R1) || ps2x.Button (PSB_R2) || ps2x.Button (PSB_R3);

	// Circle/Lx are button 2
	j.b2 = ps2x.Button (PSB_CIRCLE) || ps2x.Button (PSB_L1) || ps2x.Button (PSB_L2) || ps2x.Button (PSB_L3);
}

void mapJoystickRacing2 (TwoButtonJoystick& j) {
	// Use analog's horizontal axis to steer, ignore vertical
	mapAnalogStickHorizontal (j);

	// D-Pad L/R can also be used
	j.left |= ps2x.Button (PSB_PAD_LEFT);
	j.right |= ps2x.Button (PSB_PAD_RIGHT);

	// Use D-Pad U/R1/R2 to accelerate and D/Cross/L1/L2 to brake
	j.up = ps2x.Button (PSB_PAD_UP) || ps2x.Button (PSB_R1) || ps2x.Button (PSB_R2);
	j.down = ps2x.Button (PSB_PAD_DOWN) || ps2x.Button (PSB_CROSS) || ps2x.Button (PSB_L1) || ps2x.Button (PSB_L2);

	/* Games probably did not expect up + down at the same time, so when
	 * braking, don't accelerate
	 */
	if (j.down)
		j.up = false;

	// Square/R3 are button 1
	j.b1 = ps2x.Button (PSB_SQUARE) || ps2x.Button (PSB_R3);

	// Triangle/L3 are button 2
	j.b2 = ps2x.Button (PSB_TRIANGLE) || ps2x.Button (PSB_L3);
}

void mapJoystickPlatform (TwoButtonJoystick& j) {
	// Use horizontal analog axis fully, but only down on vertical
	mapAnalogStickHorizontal (j);
	mapAnalogStickVertical (j);

	// D-Pad is fully functional
	j.up = ps2x.Button (PSB_PAD_UP);		// Note the '=', will override analog UP
	j.down |= ps2x.Button (PSB_PAD_DOWN);
	j.left |= ps2x.Button (PSB_PAD_LEFT);
	j.right |= ps2x.Button (PSB_PAD_RIGHT);

	// Cross is up/jump
	j.up |= ps2x.Button (PSB_CROSS);

	// Square/Rx are button 1
	j.b1 = ps2x.Button (PSB_SQUARE) || ps2x.Button (PSB_R1) || ps2x.Button (PSB_R2) || ps2x.Button (PSB_R3);

	// Triangle/Lx are button 2
	j.b2 = ps2x.Button (PSB_TRIANGLE) || ps2x.Button (PSB_L1) || ps2x.Button (PSB_L2) || ps2x.Button (PSB_L3);
}

void flashLed (byte n) {
	for (byte i = 0; i < n; ++i) {
		digitalWrite (PIN_LED_MODE_CD32, HIGH);
		delay (40);
		digitalWrite (PIN_LED_MODE_CD32, LOW);
		delay (80);
	}
}

void handleJoystick () {
	int rx = ps2x.Analog (PSS_RX);   // 0 ... 255
	int deltaRX = rx - ANALOG_IDLE_VALUE;
	int deltaRXabs = abs (deltaRX);

	int ry = ps2x.Analog (PSS_RY);
	int deltaRY = ry - ANALOG_IDLE_VALUE;
	int deltaRYabs = abs (deltaRY);
	if (deltaRXabs > ANALOG_DEAD_ZONE || deltaRYabs > ANALOG_DEAD_ZONE) {
		// Right analog stick moved, switch to Mouse mode
		toMouse ();
	} else if (ps2x.Button (PSB_SELECT)) {
		debugln (F("Select is being held"));
		
		// Select pressed, change button mapping
		if (ps2x.Button (PSB_SQUARE)) {
			joyMappingFunc = mapJoystickNormal;
			flashLed (JMAP_NORMAL);
		} else if (ps2x.Button (PSB_TRIANGLE)) {
			joyMappingFunc = mapJoystickRacing1;
			flashLed (JMAP_RACING1);
		} else if (ps2x.Button (PSB_CIRCLE)) {
			joyMappingFunc = mapJoystickRacing2;
			flashLed (JMAP_RACING2);
		} else if (ps2x.Button (PSB_CROSS)) {
			joyMappingFunc = mapJoystickPlatform;
			flashLed (JMAP_PLATFORM);
		}
	} else {
		// Call button mapping function
		TwoButtonJoystick j = {false, false, false, false, false, false};
		//~ if (!joyMappingFunc)
			//~ joyMappingFunc = mapJoystickNormal;			
		joyMappingFunc (j);

		// Make mapped buttons affect the actual pins
		if (j.up) {
			buttonPress (PIN_UP);
		} else {
			buttonRelease (PIN_UP);
		}

		if (j.down) {
			buttonPress (PIN_DOWN);
		} else {
			buttonRelease (PIN_DOWN);
		}

		if (j.left) {
			buttonPress (PIN_LEFT);
		} else {
			buttonRelease (PIN_LEFT);
		}

		if (j.right) {
			buttonPress (PIN_RIGHT);
		} else {
			buttonRelease (PIN_RIGHT);
		}

		/* If the interrupt that switches us to CD32 mode is
		 * triggered while we are here we might end up setting pin states after
		 * we should have relinquished control of the pins, so let's avoid this
		 * disabling interrupts, we will handle them in a few microseconds.
		 */
		noInterrupts ();

		if (j.b1) {
			buttonPress (PIN_BTN1);
		} else {
			buttonRelease (PIN_BTN1);
		}

		if (j.b2) {
			buttonPress (PIN_BTN2);
		} else {
			buttonRelease (PIN_BTN2);
		}

		interrupts ();
	}
}

void handleMouse () {
	if (ps2x.Button (PSB_PAD_UP) || ps2x.Button (PSB_PAD_DOWN) ||
	    ps2x.Button (PSB_PAD_LEFT) || ps2x.Button (PSB_PAD_RIGHT)) {
		// D-Pad pressed, go back to joystick mode
		toJoystick ();
	} else {
		static unsigned long tx = 0, ty = 0;
		
		// Right analog stick works as a mouse - Horizontal axis
		int x = ps2x.Analog (PSS_RX);   // 0 ... 255
		int deltaX = x - ANALOG_IDLE_VALUE;
		int deltaXabs = abs (deltaX);
		if (deltaXabs > ANALOG_DEAD_ZONE) {
			unsigned int period = map (deltaXabs, ANALOG_DEAD_ZONE, ANALOG_IDLE_VALUE, MOUSE_SLOW_DELTA, MOUSE_FAST_DELTA);
			debug (F("x = "));
			debug (x);
			debug (F(" --> period = "));
			debugln (period);

			byte leadingPin;
			byte trailingPin;
			if (deltaX > 0) {
				// Right
				leadingPin = PIN_RIGHT;
				trailingPin = PIN_DOWN;
			} else {
				// Left
				leadingPin = PIN_DOWN;
				trailingPin = PIN_RIGHT;
			}

			if (millis () - tx >= period) {
				digitalWrite (leadingPin, !digitalRead (leadingPin));
				tx = millis ();
			}
			
			if (millis () - tx >= period / 2) {
				digitalWrite (trailingPin, !digitalRead (leadingPin));
			}
		}

		// Vertical axis
		int y = ps2x.Analog (PSS_RY);
		int deltaY = y - ANALOG_IDLE_VALUE;
		int deltaYabs = abs (deltaY);
		if (deltaYabs > ANALOG_DEAD_ZONE) {
			unsigned int period = map (deltaYabs, ANALOG_DEAD_ZONE, ANALOG_IDLE_VALUE, MOUSE_SLOW_DELTA, MOUSE_FAST_DELTA);
			debug (F("y = "));
			debug (y);
			debug (F(" --> period = "));
			debugln (period);

			byte leadingPin;
			byte trailingPin;
			if (deltaY > 0) {
				// Down
				leadingPin = PIN_LEFT;
				trailingPin = PIN_UP;
			} else {
				// Up
				leadingPin = PIN_UP;
				trailingPin = PIN_LEFT;
			}

			if (millis () - ty >= period) {
				digitalWrite (leadingPin, !digitalRead (leadingPin));
				ty = millis ();
			}
			
			if (millis () - ty >= period / 2) {
				digitalWrite (trailingPin, !digitalRead (leadingPin));
			}
		}

		// Buttons
		noInterrupts ();
		
		if (ps2x.Button (PSB_L1) || ps2x.Button (PSB_L2) || ps2x.Button (PSB_L3)) {
			buttonPress (PIN_BTN1);
		} else {
			buttonRelease (PIN_BTN1);
		}

		if (ps2x.Button (PSB_R1) || ps2x.Button (PSB_R2) || ps2x.Button (PSB_R3)) {
			buttonPress (PIN_BTN2);
		} else {
			buttonRelease (PIN_BTN2);
		}

		interrupts ();
	}
}

void handleCD32Pad () {
	// Directions still behave as in normal joystick mode, so abuse those functions
	TwoButtonJoystick analog;
	mapAnalogStickHorizontal (analog);
	mapAnalogStickVertical (analog);

	// D-Pad is fully functional as well, keep it in mind
	if (analog.up || ps2x.Button (PSB_PAD_UP)) {
		buttonPress (PIN_UP);
	} else {
		buttonRelease (PIN_UP);
	}

	if (analog.down || ps2x.Button (PSB_PAD_DOWN)) {
		buttonPress (PIN_DOWN);
	} else {
		buttonRelease (PIN_DOWN);
	}

	if (analog.left || ps2x.Button (PSB_PAD_LEFT)) {
		buttonPress (PIN_LEFT);
	} else {
		buttonRelease (PIN_LEFT);
	}

	if (analog.right || ps2x.Button (PSB_PAD_RIGHT)) {
		buttonPress (PIN_RIGHT);
	} else {
		buttonRelease (PIN_RIGHT);
	}

	/* Map buttons - Note that 0 means pressed and that MSB must be 1 for the ID
	 * sequence
	 */
	buttonsLive = 0xFF;

	if (ps2x.Button (PSB_START))
		buttonsLive &= ~BTN_START;

	if (ps2x.Button (PSB_TRIANGLE))
		buttonsLive &= ~BTN_GREEN;

	if (ps2x.Button (PSB_SQUARE))
		buttonsLive &= ~BTN_RED;

	if (ps2x.Button (PSB_CROSS))
		buttonsLive &= ~BTN_BLUE;

	if (ps2x.Button (PSB_CIRCLE))
		buttonsLive &= ~BTN_YELLOW;

	if (ps2x.Button (PSB_L1) || ps2x.Button (PSB_L2) || ps2x.Button (PSB_L3))
		buttonsLive &= ~BTN_FRONT_L;

	if (ps2x.Button (PSB_R1) || ps2x.Button (PSB_R2) || ps2x.Button (PSB_R3))
		buttonsLive &= ~BTN_FRONT_R;
}

void loop () {
	ps2x.read_gamepad ();

	// Handle joystick report
	switch (mode) {
	case MODE_JOYSTICK:
		handleJoystick ();
		digitalWrite (PIN_LED_MODE_CD32, LOW);
		break;
	case MODE_MOUSE:
		handleMouse ();
		digitalWrite (PIN_LED_MODE_CD32, (millis () / 500) % 2 == 0);
		break;
	case MODE_CD32:
		handleCD32Pad ();
		digitalWrite (PIN_LED_MODE_CD32, HIGH);
		break;
	default:
		// Do nothing
		digitalWrite (PIN_LED_MODE_CD32, (millis () / 250) % 2 == 0);
		break;
	}

	if (lastSwitchedTime > 0 && millis () - lastSwitchedTime > TIMEOUT_CD32_MODE) {
		// Pad Mode pin has been high for a while, disable CD32 mode
		if (wasMouse) {
			toMouse ();
		} else {
			toJoystick ();
		}

		lastSwitchedTime = 0;
	}
}
