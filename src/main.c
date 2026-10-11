//
// STC15W408AS LED Clock
//
#include <stdint.h>
#include "stc15.h"
#include "global.h"
#include "timer.h"
#include "ds1302.h"
#include "adc.h"
#include "display.h"
#include "utility.h"
#if HAS_NY3P_SPEECH
#include "sound.h"
#endif
#if DEBUG
#include "serial.h"
#include "stdio.h"  // used for debug only
#endif

void checkAlarm();
void checkChime();
void soundChime();
void alarmDelay(uint8_t ticks);

__bit soundAlarm;
__idata uint8_t snoozeMinutes = 0;

int main()
{
#if DEBUG
    initSerial();           // use P3.6 & 3.7 serial port for debug!
    printf_tiny("STC15W408AS awake\n");
#endif
    // setup 20mA source for direct drive clocks
    SET_PORT_DRIVE;
    // set photoresistor & ntc pins to open-drain output
    SET_LDR_PORT;
    SET_THERMISTOR_PORT;
    // done setup of ports
    blankDisplay();         // turn everything off for startup
    initRtc();              // setup DS1302 and read config ram
    initTimer0();           // start timers and display scan
    delay3(33);             // wait 100ms - else the reset doesn't work
    if (checkForReset()){
        initColdStart();    // reset clock when both switches down @ powerup
        while (checkForRelease()) ;
    }
#if TEST_DEFAULTS
    initColdStart();        // reset everything everytime for testing on powerup
#endif
    getClock();
#if HAS_NY3P_SPEECH         // first item spoken is one less than sent!!
    speakItem(sndOh);       // must say (anything/something) to clear the chip
#endif

// soundAlarm - Toggle the buzzer on and off at an anoying rate!
// The buzzer has its own native frequency when +5 volts is applied.
// The polarity is usually low to sound buzzer so the constants
// BZR_ON and BZR_OFF are usually inverted (ON = 0, OFF = 1)
//
// Routine waits until S1 is pressed to exit (and dumps keybuf)

    while(TRUE) {
#if OPT_CHIME
        // check for Chime (guard against double firing):
        if (ChimeOn && clockRam.min == 0 && clockRam.sec == 0) {
            static uint8_t lastChimeHour = 0xFF;
            if (clockRam.hr != lastChimeHour) {
                lastChimeHour = clockRam.hr;
                checkChime();
            }
        }
#endif
#if OPT_ALARM
        // Check for Alarm:
        if (AlarmOn) {
            // Scheduled Alarm at :01 seconds:
            if (clockRam.hr == clockRam.almHour && clockRam.min == clockRam.almMin && clockRam.sec == 1) {
                displayState = stClock;
                soundAlarm = TRUE;
                snoozeMinutes = 0;
            }

            // Snooze 1-minute countdown:
            if (snoozeMinutes > 0 && clockRam.sec == 0) {
                static __idata uint8_t lastSnoozeMin = 0xFF;
                if (clockRam.min != lastSnoozeMin) {
                    lastSnoozeMin = clockRam.min;
                    if (--snoozeMinutes == 0) {
                        displayState = stClock;
                        soundAlarm = TRUE;  // Wake up again!
                    }
                }
            }
        } else {
            snoozeMinutes = 0;      // Disarmed if alarm turned off in menu
        }
#endif
        checkAlarm();               // sound alarm if needed
#if HAS_NY3P_SPEECH
    if ( displayState == stClock && pressedS3 && !soundAlarm ){
        pressedS3 = FALSE;
        speakTime();                // load buffer with sounds
    }
#endif
        // call state machine, everthing done from there
        displayFSM();
    }
}

// ###################################################################################
// End Main()
// ###################################################################################

// Alarm delay routine
// Call with number of 3ms ticks to wait
// Max is 3ms * 255 = 7.65 seconds delay

void alarmDelay(uint8_t ticks)
{
    userTimer3 = ticks;
    while (userTimer3) {
        updateClock();
        updateColonGlow();
    }
}

void checkAlarm()
{
#if OPT_ALARM
    while(soundAlarm){
        BZR_ON;
        alarmDelay(66);         // 200ms
        BZR_OFF;
        alarmDelay(33);         // 100ms
        BZR_ON;
        alarmDelay(66);         // 200ms
        BZR_OFF;
#if HAS_NY3P_SPEECH
        speakItem(sndRing);
        if (checkAndClearS3()){
            soundAlarm = FALSE;
            break;
        }
#else
        alarmDelay(225);        // 675ms

        // S2 (Bottom): Cancel alarm completely for the day:
        if (checkAndClearS2()){
            soundAlarm = FALSE;
            snoozeMinutes = 0;
            break;
        }

        // S1 (Top): Snooze for 5 minutes (define SNOOZE_TIME in global.h)!
        if (checkAndClearS1()){
            soundAlarm = FALSE;
            snoozeMinutes = SNOOZE_TIME;
        }
#endif
    }
#endif
}

// checkChime - convert current time and chime S/S to 24 hour format
// then setup proper start/stop order and compare to see if chime
// should sound

void checkChime()
{
    uint8_t tCurrent,tStart,tStop;
    __bit tFormat;
    tFormat = Select_12;
    changeTimeFormat(FALSE);    // convert to 24 hour (maybe)
    tCurrent = clockRam.hr;
    tStart = clockRam.chimeStartHour;
    tStop = clockRam.chimeStopHour;
    changeTimeFormat(tFormat);    // convert back to entry state
    if (tStop >= tStart) {
        if (tCurrent >= tStart && tCurrent <= tStop)
            soundChime();
    } else {
        if (tCurrent >= tStart || tCurrent <= tStop)
            soundChime();
    }
}

// soundChime - Toggle the buzzer on and off twice for a short
// double beep. Anything more is annoying (to me at least...)
// Fixed: earlier this routine had to consume more than 1000ms
// (or else it would refire when it returns since time would still
// be 00:00), but we now guarded against that in main loop.

void soundChime()
{

#if OPT_CHIME && !HAS_NY3P_SPEECH
    BZR_ON;
    delay3(33);         // 100ms beep
    BZR_OFF;
    delay3(33);         // 100ms pause
    BZR_ON;
    delay3(33);         // 100ms beep
    BZR_OFF;            // done (~300ms total)
#elif OPT_CHIME && HAS_NY3P_SPEECH
    speakItem(sndChime);        // updates display, returns when finished
#endif

}

