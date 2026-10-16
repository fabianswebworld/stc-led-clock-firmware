#include <stdint.h>
#include "global.h"
#include "stc15.h"
#include "ds1302.h"
#include "adc.h"
#include "timer.h"
#include "display.h"
#if DEBUG
#include "serial.h"
#include "stdio.h"  // used for debug only
#endif

// calculate timer 0 reload value
#define T0_RELOAD   (65536L - (FOSC / T0TICKS))

// how long a button must be held to be considered a long press
#define LONG_PRESS_TICKS  80   // 80 * 10ms = 800ms hold time

volatile uint8_t    _3msTimer;          // 240 ticks of 12.5us = 3.0ms
volatile uint8_t    _2_5msTimer;        // 200 ticks of 12.5us = 2.5ms
volatile uint8_t    _1_5msTimer;        // 120 ticks of 12.5us = 1.5ms
volatile __bit      _1_5msPhase;        // toggles every 1.5ms (2 phases = 3ms)
volatile uint8_t    _10msPhase;         // Counts 0 to 3 (4 * 2.5ms = 10ms)
volatile uint8_t    _100msTimer;        // increments, blinks display at 5hz
volatile uint8_t    _500msTimer;        // increments, blinks ":" seperator at 1hz
volatile uint8_t    userTimer100;       // user delay in 100ms ticks (25.5 seconds max)
volatile uint8_t    userTimer3;         // user delay in 3ms ticks 765ms max (3*255)
volatile uint8_t    userTimer1_5;       // user delay in 1.5ms ticks
volatile uint8_t    colonTick = 0;      // for the colon incandescent blink simulation

volatile uint8_t    stateQueueS1;       // holds 5 key states
volatile __bit      pressedS1;          // true when pressed, user clears

volatile uint8_t    stateQueueS2;       // pressed, released, etc.
volatile __bit      pressedS2;          // true when pressed, user clears

volatile uint8_t    keyRepeatTimer;     // sets repeat time (in 10ms ticks)

volatile uint8_t    holdTimerS1 = 0;
volatile uint8_t    holdTimerS2 = 0;
volatile __bit      longPressedS1 = FALSE;
volatile __bit      longPressedS2 = FALSE;

#if HAS_NY3P_SPEECH
volatile uint8_t    stateQueueS3;       // holds 5 key states
volatile __bit      pressedS3;          // true when pressed, user clears
volatile uint8_t    soundTimer;         // typically set to 10 ticks (250us)
#endif

__bit _1hzToggle;   // 500 ms pulse width (blinks : in display)
__bit _5hzToggle;   // 100 ms used to blink digits being set

void initTimer0(void)                   // 12.5us @ 22.1184 MHz (80 kHz interrupts)
{
    AUXR |= 0x80;
    TH0 = T0_RELOAD >> 8;
    TL0 = T0_RELOAD & 0xFF;
    TF0 = 0;
    TR0 = 1;
    ET0 = 1;
    EA  = 1;
}

void timer0_isr() __interrupt (1)
{
    TH0 = T0_RELOAD >> 8;
    TL0 = T0_RELOAD & 0xFF;

    // Display update runs at 80,000 Hz!
    displayUpdateISR();

    // 1. User delay handler: fires every 1.5ms (120 ticks of 12.5us)
    if (!_1_5msTimer--){
        _1_5msTimer = 120;
        if (userTimer1_5) userTimer1_5--;

        _1_5msPhase = !_1_5msPhase;
        if (_1_5msPhase) {
            // Fires every 3.0ms (2 phases of 1.5ms):
            if (userTimer3) userTimer3--;
            colonTick++;
        }
    }

    // 2. 2.5ms sub-timer (200 ticks of 12.5us = 2.5ms)
    if (!_2_5msTimer--) {
        _2_5msTimer = 200;

        // Executes every 10ms (4 phases of 2.5ms):
        if (++_10msPhase >= 4) {
            _10msPhase = 0;

            debounceSwitches();
            if (keyRepeatTimer) keyRepeatTimer--;

            if (!_100msTimer--) {
                _100msTimer = 10;
                _5hzToggle =! _5hzToggle;
                if (userTimer100) userTimer100--;
            }
            if (!_500msTimer--) {
                _500msTimer = 50;
                _1hzToggle =! _1hzToggle;
            }
        }
    }

  #if HAS_NY3P_SPEECH
    if (soundTimer) soundTimer--;
  #endif
}

// User delay routines
// Call with number of 3ms ticks to wait
void delay3(uint8_t ticks)
{
    userTimer3 = ticks;
    while (userTimer3)
    ;
}

// Call with number of 1.5ms ticks to wait
void delay1_5(uint8_t ticks)
{
    userTimer1_5 = ticks;
    while (userTimer1_5)
    ;
}

void debounceSwitches(void)
{
    static __bit bothWerePressed = FALSE;
    __bit isSetupMenu = (displayState >= scSet && displayState <= msShowVersion);
    __bit s1Down      = isSetupMenu || (displayState == stStopRun);

    // suppress single-button releases if both were pressed together
    if (S1 == 0 && S2 == 0) {
        bothWerePressed = TRUE;
    }

    // -------------------------------------------------------------
    // SW1 (Top Button)
    // -------------------------------------------------------------
    stateQueueS1 = (stateQueueS1 << 1) | S1 | K_HELD;

    if (S1 == 0) {
        if (holdTimerS1 < 250) holdTimerS1++;

        if (holdTimerS1 == LONG_PRESS_TICKS) {
            // only trigger long S1 if currently on the normal clock
            if (displayState == stClock) {
                longPressedS1 = TRUE;
            }
            pressedS1 = FALSE;
        }
    } else {
        // only fire release if both buttons weren't held together
        if (!s1Down && !bothWerePressed && holdTimerS1 >= 3 && holdTimerS1 < LONG_PRESS_TICKS) {
            pressedS1 = TRUE;
        }
        holdTimerS1 = 0;
    }

    if (s1Down && stateQueueS1 == K_PRESSED) pressedS1 = TRUE;

    // -------------------------------------------------------------
    // SW2 (Bottom Button)
    // -------------------------------------------------------------
    stateQueueS2 = (stateQueueS2 << 1) | S2 | K_HELD;

    if (S2 == 0) {
        if (holdTimerS2 < 250) holdTimerS2++;

        if (!isSetupMenu) {
            if (holdTimerS2 == LONG_PRESS_TICKS) {
                longPressedS2 = TRUE;
                pressedS2 = FALSE;
            }
        } else {
            if (holdTimerS2 >= 40 && !keyRepeatTimer) {
                keyRepeatTimer = KEY_REPEAT;
                pressedS2 = TRUE;
            }
        }
    } else {
        // only fire release if both buttons weren't held together
        if (!isSetupMenu && !bothWerePressed && holdTimerS2 >= 3 && holdTimerS2 < LONG_PRESS_TICKS) {
            pressedS2 = TRUE;
        }
        holdTimerS2 = 0;
    }

    if (isSetupMenu && stateQueueS2 == K_PRESSED) pressedS2 = TRUE;

#if HAS_NY3P_SPEECH
    stateQueueS3 = (stateQueueS3 << 1) | S3 | K_HELD;
    if (stateQueueS3 == K_PRESSED) pressedS3 = TRUE;
#endif

    if (S1 == 1 && S2 == 1) {
        bothWerePressed = FALSE;
    }
}

__bit checkForReset()
{
    if ((stateQueueS1 == K_HELD) & (stateQueueS2 == K_HELD))
        return TRUE;
    else
        return FALSE;
}

__bit checkForRelease()
{
    pressedS1 = FALSE;
    pressedS2 = FALSE;
    longPressedS1 = FALSE;
    longPressedS2 = FALSE;
    if ((stateQueueS1 == K_RELEASED) & (stateQueueS2 == K_RELEASED))
        return FALSE;
    else
        return TRUE;
}

__bit getStateS2Flasher()
{
    if ( stateQueueS2 == K_HELD )
        return TRUE;
    else
        return _5hzToggle;
}

__bit checkAndClearS1()
{
    if (pressedS1){
        pressedS1 = FALSE;
        return TRUE;
    }
    else
        return FALSE;
}

__bit checkAndClearS2()
{
    if (pressedS2){
        pressedS2 = FALSE;
        return TRUE;
    }
    else
        return FALSE;
}

#ifdef S3
__bit checkAndClearS3()
{
    if (pressedS3){
        pressedS3 = FALSE;
        return TRUE;
    }
    else
        return FALSE;
}
#endif

__bit checkAndClearLongS1(void)
{
    if (longPressedS1) {
        longPressedS1 = FALSE;
        pressedS1 = FALSE;
        return TRUE;
    }
    return FALSE;
}

__bit checkAndClearLongS2(void)
{
    if (longPressedS2) {
        longPressedS2 = FALSE;
        pressedS2 = FALSE;
        return TRUE;
    }
    return FALSE;
}

__bit isBothPressed(void)
{
    return (S1 == 0 && S2 == 0);
}

void stateSwitchWithS1(uint8_t newState)
{
    if (pressedS1){
        pressedS1 = FALSE;
        displayState = newState;
    }
}

void stateSwitchExtendedWithS1(uint8_t newState, uint8_t text2, __bit flag)
{
    if (pressedS1){
        pressedS1 = FALSE;
        displayState = newState;
        if (!flag)
            displayState--;
        if ( text2 != NoText2 )
            setHourDigits(text2);
    }
}

void stateSwitchWithS2(uint8_t newState)
{
    if (pressedS2){
        while (!S2);
        pressedS2 = FALSE;
        displayState = newState;
    }
}

void stateSwitchExtendedWithS2(uint8_t newState, uint8_t text2, __bit flag)
{
    if (pressedS2){
        while (!S2);
        pressedS2 = FALSE;
        displayState = newState;
        if (!flag)
            displayState--;
        if ( text2 != NoText2 )
            setHourDigits(text2);
    }
}
