
#include <stdint.h>
#include "stc15.h"
#include "global.h"
#include "timer.h"
#include "ds1302.h"
#include "adc.h"
#include "display.h"
#include "utility.h"
#if DEBUG
#include "serial.h"
#include "stdio.h"  // used for debug only
#endif

// Insert static tables into the code segement here.
// Doing this allows this modules statements to contain
// references to this tables without having to prototype the
// variables beforehand. This gets messy since prototyping a
// an array variable before later redeclaring it with a
// static initialization it just doesn't work.
// This is all done to save code space.
// Ugly but seemingly necessary.

#include "codeTables.c"

// These are global to this module.
// Not a great practice, but when you've only got
// 80 bytes of directly addressable RAM...
// Most are used in several places at different times.

uint8_t h,m,d;                    // (h)ours and (m)inutes or (m)onth and (d)ay
uint8_t displayState;             // the main control variable

__bit   dp0,dp1;                  // decimal points for each digit
__bit   dp2,dp3;                  // numbers match Cathode[] array index
__bit   AM_PM;                    // used to set decimal point on/off
__bit   readyRead;                // used to read temperature once per second
__bit   timeChanged;              // write time to clock when set
#if OPT_BLANK_ZERO
__bit   blankZero;                // set true to suppress leading zero in clock
#endif

__idata uint8_t cdMin = 0;        // last used countdown timer - minutes
__idata uint8_t cdSec = 59;       // last used countdown timer - seconds
__idata uint8_t cdInactivity = 0; // inactivity timer before starting cd timer

uint8_t maskOnOff;                // set to 0x7F or 0xFF to blink decimal point
uint8_t maskDp0;                  // set to 0x7F or 0xFF with dp0 (=dp on or off)
uint8_t maskDp1;                  // set to 0x7F or 0xFF with dp1 (=dp on or off)
uint8_t maskDp2;                  // set to 0x7F or 0xFF with dp2 (=dp on or off)
uint8_t maskDp3;                  // set to 0x7F or 0xFF with dp3 (=dp on or off)

uint8_t brightLevel;              // result of LDR and config settings
uint8_t actualTemp;               // result of ADC and transform

// declared static - in routine that is called from from an ISR

uint8_t CathodeBuf[4];            // holds each digits segments (0 = on)
volatile uint8_t aPos;            // cycles through 0-3 anode positions
volatile uint8_t aOnTicks;
volatile uint8_t aOffTicks;
volatile uint8_t activeBright;    // current brightness during fading
volatile uint8_t colonBright;     // current brightness for "incandescent" colon
extern volatile uint8_t colonTick;

static __bit zeroLocked = FALSE;  // helper for countdown both-button press to zero

// configuration for fading and the "simulated incandescent filament" colon

#define FADE_TARGET_TICKS   255   // Target fade duration in 1.5ms steps
#define FADE_PAUSE_TICKS     40   // In 3ms steps, so 40 equals 120ms pause while dark
#define COLON_LIGHTUP_SPEED   8   // Filament heat-up speed: higher = faster (1..10)
#define COLON_DECAY_PERCENT  25   // Target level for initial cool-down (e.g. 50 = 50%)
#define COLON_DECAY_SPEED    10   // Initial cool-down speed (1 = slowest, 10 = fastest)
#define COLON_AFTERGLOW_DUR  17   // Afterglow duration: higher = longer

// configuration for smoothing the dimming via the LDR

#define LDR_UPDATE_TICKS     16   // 16 * 3ms = ~48ms (~20.8 Hz)
#define LDR_DEADBAND          4   // Ignore sensor noise <= 4 counts

// configuration / definition of the Snake animation

#define SNAKE_STEPS          34

typedef struct {
    uint8_t digit;
    uint8_t mask;
} Waypoint;

// 32-step irregular track across 4 digits
__code const Waypoint snakeTrack[SNAKE_STEPS] = {
    {0, 0x01}, {1, 0x01}, {2, 0x08}, {3, 0x01}, // 0..3: Top rail (Rightward)
    {3, 0x02}, {3, 0x04}, {3, 0x08}, {3, 0x10}, // 4..7: Curve bottom of digit 3
    {2, 0x40}, {1, 0x40}, {0, 0x40},            // 8..10: Middle Rail (Leftward)
    {0, 0x10},                                  // 11: Down Left-Bottom Edge of digit 1
    {0, 0x08}, {1, 0x08}, {2, 0x01}, {3, 0x08}, // 12..15: Bottom Rail (Rightward)
    {3, 0x04}, {3, 0x02}, {3, 0x01},            // 16..18: Up Right Edge to Top Rail
    {2, 0x08}, {1, 0x01}, {0, 0x01},            // 19..21: Top Rail (Leftward)
    {0, 0x20},                                  // 22: Down Left-Top Edge
    {0, 0x40}, {1, 0x40}, {2, 0x40}, {3, 0x40}, // 23..26: Middle Rail (Rightward)
    {3, 0x04}, {3, 0x08},                       // 27..28: Down Right-Bottom Edge to Bottom Rail
    {2, 0x01}, {1, 0x08}, {0, 0x08},            // 29..31: Bottom Rail (Leftward)
    {0, 0x10}, {0, 0x20}                        // 32..33: Up Left Edge to Start
};

__idata uint8_t snakeHead = 0;
__idata uint8_t snakeLen = 2;
__idata uint8_t snakeTimer = 0;
__idata uint8_t appleDigit = 0;
__idata uint8_t appleDelay = 0;
__idata uint8_t snakeLaps = 0;


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Update the LED display based on four variables.
//
// These are:
//
//      CathodeBuf[i] = 7 segment pattern to display, i = AnodePos
//      aPos          = Current digit, 0.1.2.3 where 0 = LHS of display
//      aOnTicks      = Number of Timer 0 ticks for the Anode to be low (active /on)
//      aOffTicks     = Number of Timer 0 ticks for the Anode to be high (inactive/off)
//
// This routine is called from inside the ISR so be quick!!
//

volatile uint8_t tickCount = 0;

void displayUpdateISR(void)
{
    // Dual-Edge Boundary Modulation
    // front pulse starts at tick 0 and grows forward
    // back pulse ends at tick 127 and grows backward

    uint8_t halfBright  = activeBright >> 1;
    uint8_t frontTicks  = halfBright + (activeBright & 0x01); // Front gets the extra tick if odd

    if (tickCount < frontTicks || tickCount >= (MAX_BRIGHT - halfBright)) {
        LED_SET_CATHODES;
        LED_SET_ANODES;
        // Simulation of incandescent light bulbs for blinking colon
        // Only active in normal clock mode (stClock) on the two colon digits (1 & 2)
        if (displayState == stClock && (aPos == 1 || aPos == 2)) {
            uint8_t cHalf  = colonBright >> 1;
            uint8_t cFront = cHalf + (colonBright & 0x01);

            if (tickCount < cFront || tickCount >= (MAX_BRIGHT - cHalf)) {
                P2_7 = 0;   // Colon ON for this micro-tick
            } else {
                P2_7 = 1;   // Colon OFF for this micro-tick
            }
        }
    } else {
        LED_RESET_ANODES;
    }

    // advance to the next digit after 128 ticks (0.8 ms at 12.5us):
    if (++tickCount >= MAX_BRIGHT) {
        tickCount = 0;
        LED_RESET_ANODES;
        P2 = BLANK_CHAR;
        if (++aPos >= 4)
            aPos = 0;
    }
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// updateClock routine used by alarm. Must update the display while
// in alarm mode, beeping and waiting for the user to reset.
// Maybe need to add snooze too...

void updateClock()
{
    static __bit lastToggle = 0;

    dp0 = OFF;
#if DP1_IS_COLON
    dp1 = _1hzToggle;           // toggle clock ':'
    dp2 = OFF;
#else
    dp1 = _1hzToggle;           // toggle clock ':'
    dp2 = _1hzToggle;           // two dp's for 1" LED's
#endif
#if OPT_ALARM
    // blink dp3 while snoozing; otherwise steady if armed
    dp3 = snoozeMinutes ? _1hzToggle : (Select_12 ? AM_PM : AlarmOn);
#else
    dp3 = Select_12 ? AM_PM : AlarmOn;
#endif

    // ONLY read the DS1302 when the second tick toggles!
    if (_1hzToggle != lastToggle) {
        lastToggle = _1hzToggle;
        getClock();
    }

    setupHour(clockRam.hr);     // convert to 12 format with AM/PM
    m = clockRam.min;
#if OPT_BLANK_ZERO
    blankZero = BlankZeroOn;    // uses configuration setting
    displayHoursOn();
    blankZero = FALSE;
#else
    displayHoursOn();
#endif
    displayMinutesOn();
}

uint8_t incrementYear(uint8_t yr)
{
    __asm
    mov     a,r7;
    add     a,#1;
    da      a;
    mov     r7,a;
    __endasm;
    if (yr > 0x99) yr = 0x00;
    return yr;
}

uint8_t decBCD(uint8_t val)
{
    if ((val & 0x0F) == 0x00) {
        val = (val - 0x10) | 0x09;
    } else {
        val--;
    }
    return val;
}

static uint8_t getFadeDelay(void)
{
    uint8_t delay;
    if (brightLevel == 0) return 1;
    delay = FADE_TARGET_TICKS / brightLevel>>1;
    if (delay == 0) delay = 1;
    if (delay > 10) delay = 10;
    return delay;
}

void fadeOut(void)
{
    uint8_t stepDelay = getFadeDelay();

    while (activeBright > 0) {
        activeBright--;
        delay1_5(stepDelay);
    }

    blankDisplay();
    delay3(FADE_PAUSE_TICKS);
}

void fadeIn(void)
{
    uint8_t stepDelay = getFadeDelay();

    while (activeBright < brightLevel) {
        activeBright++;
        delay1_5(stepDelay);
    }
}

// helper function for simulation of "incandescent light bulb" blinking colon

void updateColonGlow(void)
{
    static __bit lastToggle = 0;
    static uint8_t lastTick = 0;
    static uint8_t upPace = 0;
    static uint8_t rampPace = 0;
    static uint8_t tailPace = 0;

    // Detect 1 Hz state change:
    if (_1hzToggle != lastToggle) {
        lastToggle = _1hzToggle;
        upPace = 0;
        rampPace = 0;
        tailPace = 0;
    }

    if (colonTick == lastTick) return;
    lastTick = colonTick;

    if (_1hzToggle) {
        // filament heat-up
        if (colonBright < activeBright) {
            if (++upPace >= (11 - COLON_LIGHTUP_SPEED)) {
                upPace = 0;
                uint8_t rise = ((activeBright - colonBright) >> 1) + 1;
                colonBright += rise;
            }
        } else if (colonBright > activeBright) {
            colonBright = activeBright; // Instantly adapt if room suddenly gets dark
        }
    }
    else if (!_1hzToggle && colonBright > 0) {
        uint8_t threshold = ((uint16_t)activeBright * COLON_DECAY_PERCENT) / 100;

        // cool-down quickly to threshold (~15-30 ms of real time)
        if (colonBright > threshold) {
            if (++rampPace >= (11 - COLON_DECAY_SPEED)) {
                rampPace = 0;
                uint8_t drop = ((colonBright - threshold) >> 1) + 1;
                colonBright -= drop;
            }
        }
        // exponential afterglow down to 0 (~300 ms of real time)
        else {
            if (++tailPace >= (COLON_AFTERGLOW_DUR >> 2) + 1) {
                tailPace = 0;
                uint8_t step = (colonBright >> 2) + 1;
                if (colonBright > step) {
                    colonBright -= step;
                } else {
                    colonBright = 0;    // Snap to 0
                }
            }
        }
    }
}

static __bit isAutoRotating = FALSE;

void transitionTo(uint8_t nextState)
{
    fadeOut();
    colonBright = 0;
    displayState = nextState;

    switch (nextState) {
#if OPT_TEMP_DSP
        case stOptTemp:
            displayTemperature();
            break;
#endif
#if OPT_DATE_DSP
        case stOptDate:
            displayDate();
            break;
#endif
#if OPT_DAY_DSP
        case stOptDay:
            displayDayOfWeek();
            break;
#endif
        case stOptYear:
            // e.g. '26
            dp0 = OFF; dp1 = OFF; dp2 = ON; dp3 = OFF;
            segs[H10] = BLANK_CHAR;
            segs[H01] = BLANK_CHAR;
            segs[M10] = ledSegBT[clockRam.yr >> 4] & 0x7F;
            segs[M01] = ledSegTB[clockRam.yr & 0x0F];
            break;

        case stClockSeconds:
            dp0 = OFF; dp1 = _1hzToggle; dp2 = OFF; dp3 = OFF;
            getClock();
            h = clockRam.min; m = clockRam.sec;
            displayHoursOn(); displayMinutesOn();
            break;

        case stSnake:
            blankDisplay();
            snakeHead = 0;
            snakeLen = 2;
            snakeTimer = colonTick;
            appleDigit = 0;
            appleDelay = 6;
            snakeLaps = 0;
            break;

        case stClock:
        default:
            updateClock();
            break;
    }

    fadeIn();
}

// Go to next mode in manual and auto-rotation display
// Single progression engine for both auto and manual mode:
void advanceDisplay(__bit isAuto)
{
    uint8_t next = stClock;

    if (displayState == stClock) {
        if (!isAuto) {
            next = stOptTemp;
        } else {
            // only auto-rotation skips disabled pages
#if OPT_TEMP_DSP
            if (TempOn) next = stOptTemp; else
#endif
#if OPT_DATE_DSP
            if (DateOn) next = stOptDate; else
#endif
#if OPT_DAY_DSP
            if (DowOn)  next = stOptDay;  else
#endif
            next = stClock;
        }
    }
#if OPT_TEMP_DSP
    else if (displayState == stOptTemp) {
        if (!isAuto) {
            next = stOptDate;
        } else {
#if OPT_DATE_DSP
            if (DateOn) next = stOptDate; else
#endif
#if OPT_DAY_DSP
            if (DowOn)  next = stOptDay;  else
#endif
            next = stClock;
        }
    }
#endif
#if OPT_DATE_DSP
    else if (displayState == stOptDate) {
        if (!isAuto) {
            next = stOptDay;
        } else {
#if OPT_DAY_DSP
            if (DowOn)  next = stOptDay;  else
#endif
            next = stClock;
        }
    }
#endif
#if OPT_DAY_DSP
    else if (displayState == stOptDay) {
        next = isAuto ? stClock : stOptYear;
    }
#endif
    else if (displayState == stOptYear) {
        next = isAuto ? stClock : stClockSeconds;
    }
    else if (displayState == stClockSeconds) {
        next = stClock;
    }

    if (next != displayState) {
        if (isAuto) {
            uint8_t dur = GET_PAGE_DUR();
            dur = (dur >= 1 && dur <= 5) ? dur : 4;
            userTimer100 = dur * 10;
            isAutoRotating = (next != stClock);
        } else {
            isAutoRotating = FALSE;
        }
        transitionTo(next);
    }
}

//----------------------------------------------------------------------------------------------//
//-------  The monster state machine. All mode changing and display is done from here      -----//
//-------  Rather than adding the code overhead of subroutines, all code is inline in this -----//
//-------  area. Big and clunky, yes. Code efficent, more so. If there is code space left  -----//
//-------  over when eveything else is complete, I'll go back and rework the monster.      -----//
//----------------------------------------------------------------------------------------------//

void displayFSM(void)
{
    static uint8_t lastLdrTick = 0;

  #if HAS_LDR
    if ((uint8_t)(colonTick - lastLdrTick) >= LDR_UPDATE_TICKS) {
        lastLdrTick = colonTick;

        uint8_t targetBright = mapLDR(getADCResult8(ADC_LDR));
        uint8_t diff = (targetBright > brightLevel) ? (targetBright - brightLevel) : (brightLevel - targetBright);

        // only change brightness if outside the sensor noise deadband
        if (diff > LDR_DEADBAND) {
            uint8_t step = (diff >> 2) + 1;

            if (targetBright > brightLevel) {
                brightLevel += step;
            } else {
                brightLevel -= step;
            }
            if (brightLevel < MIN_BRIGHT) brightLevel = MIN_BRIGHT;
            if (brightLevel > MAX_BRIGHT) brightLevel = MAX_BRIGHT;
            activeBright = brightLevel;
        }
    }
  #else
    brightLevel = mapLDR(MIN_BRIGHT);
    activeBright = brightLevel;
  #endif

    updateColonGlow();

  #if HAS_THERMISTOR
    if ( readyRead & _1hzToggle ){
        actualTemp = mapTemp(getADCResult(ADC_TEMP));
        readyRead = FALSE;
    }
    else if (!_1hzToggle)
        readyRead = TRUE;
  #endif

    switch (displayState) {

// ###################################################################################
// Home menu, Regular display items
// ###################################################################################

// stClock, stClockSeconds, stOptTemp, stOptDate, stOptDay

    case stClock:
        updateClock();
        stateSwitchWithS1(scSet);

        // Long S1: launch snake animation (easter egg...)
        if (checkAndClearLongS1()) {
            isAutoRotating = FALSE;
            transitionTo(stSnake);
            break;
        }

        // Long S2: enter function menu (right now only: countdown timer)
        if (checkAndClearLongS2()) {
            isAutoRotating = FALSE;
            displayState = msCtdnMenu;
            setAllDigits(txCtdn);
            dp0 = OFF; dp1 = OFF; dp2 = OFF; dp3 = OFF;
            break;
        }

        // Short S2: manual page advance (or cancel snooze if active)
        if (checkAndClearS2()) {
        #if OPT_ALARM
            if (snoozeMinutes) {
                snoozeMinutes = 0;
            } else
        #endif
            {
                advanceDisplay(FALSE);
            }
        }

        // Scheduled Snake animation trigger
        {
            // if chime is enabled, wait until sec == 01 so the chime finishes cleanly first
            uint8_t targetSec = (ChimeOn) ? 0x01 : 0x00;

            if (clockRam.sec == targetSec) {
                // if alarm is armed for this exact minute, skip snake completely
                __bit alarmConflict = (AlarmOn && clockRam.hr == clockRam.almHour && clockRam.min == clockRam.almMin);

                if (!alarmConflict) {
                    uint8_t ani = GET_ANI_MODE();
                    __bit trigAni = FALSE;
                    if (ani == 1 && (clockRam.hr == 0x12 || clockRam.hr == 0x00) && clockRam.min == 0) trigAni = TRUE;
                    else if (ani == 2 && clockRam.min == 0) trigAni = TRUE;
                    else if (ani == 3 && (clockRam.min == 0 || clockRam.min == 0x30)) trigAni = TRUE;
                    else if (ani == 4 && (clockRam.min == 0 || clockRam.min == 0x15 || clockRam.min == 0x30 || clockRam.min == 0x45)) trigAni = TRUE;
                    else if (ani == 5) trigAni = TRUE;

                    if (trigAni) {
                        isAutoRotating = FALSE;
                        transitionTo(stSnake);
                        break;
                    }
                }
            }
        }

        // Auto-rotation trigger (only if rotMode > 0):
        uint8_t rotMode = GET_ROT_MODE();
        if (rotMode > 0) {
            static uint8_t lastRotSec = 0xFF;

            // Only evaluate once per second (prevents multi-triggering):
            if (clockRam.sec != lastRotSec) {
                lastRotSec = clockRam.sec;

                __bit triggerRotation = FALSE;
                if (rotMode == 1 && clockRam.sec == 0x30) {
                    triggerRotation = TRUE;
                } else if (rotMode == 2 && (clockRam.sec == 0x00 || clockRam.sec == 0x30)) {
                    triggerRotation = TRUE;
                } else if (rotMode == 3 && (clockRam.sec == 0x00 || clockRam.sec == 0x20 || clockRam.sec == 0x40)) {
                    triggerRotation = TRUE;
                }

                if (triggerRotation) {
                    advanceDisplay(TRUE);
                }
            }
        }
        break;

    case stClockSeconds:
        dp0 = OFF;
        dp1 = _1hzToggle;
        dp2 = OFF;
        dp3 = OFF;
        getClock();
        h = clockRam.min;
        m = clockRam.sec;
        displayHoursOn();
        displayMinutesOn();

        // S1 resets seconds to :00:
        if (checkAndClearS1()) {
            clockRam.sec = 0;
            putClock();
        }

        // S2 manually advances to next mode (stClock):
        if (checkAndClearS2()) {
            advanceDisplay(FALSE);
        }
        // no auto-timeout (I don't like it)
        break;

#if OPT_TEMP_DSP
    case stOptTemp:
        displayTemperature();
        stateSwitchWithS1(scSet);

        if (checkAndClearS2()) {
            advanceDisplay(FALSE);      // Manual advance, stops auto-rotation
        }

        if (isAutoRotating && !userTimer100) {
            advanceDisplay(TRUE);
        }
        break;
#endif

#if OPT_DATE_DSP
    case stOptDate:
        displayDate();
        stateSwitchWithS1(scSet);

        if (checkAndClearS2()) {
            advanceDisplay(FALSE);      // Manual advance, stops auto-rotation
        }

        if (isAutoRotating && !userTimer100) {
            advanceDisplay(TRUE);
        }
        break;
#endif

#if OPT_DAY_DSP
    case stOptDay:
        displayDayOfWeek();
        stateSwitchWithS1(scSet);

        if (checkAndClearS2()) {
            advanceDisplay(FALSE);      // Manual advance, stops auto-rotation
        }

        if (isAutoRotating && !userTimer100) {
            advanceDisplay(TRUE);
        }
        break;
#endif

    case stOptYear:
        dp0 = OFF; dp1 = OFF; dp2 = ON; dp3 = OFF;
        segs[H10] = BLANK_CHAR;
        segs[H01] = BLANK_CHAR;
        segs[M10] = ledSegBT[clockRam.yr >> 4] & 0x7F; // e.g. '26
        segs[M01] = ledSegTB[clockRam.yr & 0x0F];

        stateSwitchWithS1(scSet);
        if (checkAndClearS2()) {
            advanceDisplay(FALSE);
        }
        break;

// ###################################################################################
// Function menu (only Ctdn for now)
// ###################################################################################

    case msCtdnMenu:
        setAllDigits(txCtdn);
        dp0 = OFF; dp1 = OFF; dp2 = OFF; dp3 = OFF;
        isAutoRotating = FALSE;
        stateSwitchWithS1(stClock);

        if (checkAndClearS2()) {
            cdMin = clockRam.cdMin;
            cdSec = clockRam.cdSec;
            cdInactivity = 20;
            displayState = stCtdnSet;
        }
        break;

    case stCtdnSet:
        dp0 = OFF; dp3 = OFF;
        segs[H10] = ledSegTB[cdMin >> 4];
        segs[H01] = ledSegTB[cdMin & 0x0F] & 0x7F;  // Bottom colon dot on
        segs[M10] = ledSegBT[cdSec >> 4]   & 0x7F;  // Top colon dot on
        segs[M01] = ledSegTB[cdSec & 0x0F];

        // Exit to clock on long-press S2:
        if (checkAndClearLongS2()) {
            transitionTo(stClock);
            break;
        }

        // Both buttons pressed = Reset to 00:00 (guarded against repeat):
        if (isBothPressed()) {
            if (!zeroLocked) {
                cdMin = 0x00;
                cdSec = 0x00;
                cdInactivity = 20;
                zeroLocked = TRUE;
            }
        } else {
            zeroLocked = FALSE;
        }

        // S1 increments minutes:
        if (checkAndClearS1()) {
            cdMin = incrementMinutes(cdMin);
            cdInactivity = 20;
        }

        // S2 increments seconds:
        if (checkAndClearS2()) {
            cdSec = incrementMinutes(cdSec);
            cdInactivity = 20;
        }

        // 2-Second inactivity auto-start:
        if (userTimer100 == 0) {
            userTimer100 = 1;
            if (cdInactivity > 0) cdInactivity--;
        }

        if (cdInactivity == 0 && (cdMin > 0 || cdSec > 0)) {
            // Save preset to RTC RAM:
            clockRam.cdMin = cdMin;
            clockRam.cdSec = cdSec;
            putConfigRam();
            displayState = stCtdnRun;
        }
        break;

    case stCtdnRun:
        // Running mode: blinking colon
        dp0 = OFF; dp3 = OFF;
        segs[H10] = ledSegTB[cdMin >> 4];
        segs[H01] = ledSegTB[cdMin & 0x0F];
        segs[M10] = ledSegBT[cdSec >> 4];
        segs[M01] = ledSegTB[cdSec & 0x0F];
        if (_1hzToggle) {
            segs[H01] &= 0x7F;  // Bottom colon dot on
            segs[M10] &= 0x7F;  // Top colon dot on
        }

        // Exit to clock on long-press S2:
        if (checkAndClearLongS2()) {
            transitionTo(stClock);
            break;
        }

        // Both buttons pressed while running: back to set mode
        if (isBothPressed()) {
            cdInactivity = 20;
            zeroLocked = TRUE;
            displayState = stCtdnSet;
            break;
        }

        // S1 pressed while running: back to set mode
        if (checkAndClearS1()) {
            cdInactivity = 20;
            displayState = stCtdnSet;
            break;
        }

        // S2 pressed while running: back to set mode
        if (checkAndClearS2()) {
            cdInactivity = 20;
            displayState = stCtdnSet;
            break;
        }

        // decrement timer every 1 second
        {
            static __bit lastSecTick = 0;
            if (_1hzToggle && !lastSecTick) {
                if (cdSec > 0) {
                    cdSec = decBCD(cdSec);
                } else if (cdMin > 0) {
                    cdMin = decBCD(cdMin);
                    cdSec = 0x59;
                } else {
                    // Time is up!
                    displayState = stCtdnAlarm;
                }
            }
            lastSecTick = _1hzToggle;
        }
        break;

    case stCtdnAlarm:
        // Time is up: all digits & colon flash rapidly (5 Hz)
        if (_5hzToggle) {
            dp0 = OFF; dp3 = OFF;
            segs[H10] = ledSegTB[0];
            segs[H01] = ledSegTB[0] & 0x7F;     // bottom colon dot
            segs[M10] = ledSegBT[0] & 0x7F;     // top colon dot
            segs[M01] = ledSegTB[0];
            BZR_ON;
        } else {
            blankDisplay();
            dp0 = OFF; dp1 = OFF; dp2 = OFF; dp3 = OFF;
            BZR_OFF;
        }

        // any button press stops alarm and returns to clock
        if (checkAndClearS1() || checkAndClearS2()) {
            BZR_OFF;
            transitionTo(stClock);
        }
        break;

// ###################################################################################
// Snake Animation
// ###################################################################################

    case stSnake:
        blankDisplay();
        dp0 = OFF; dp1 = OFF; dp2 = OFF; dp3 = OFF;

        {
            uint8_t i;
            __bit appleVisible = TRUE;

            for (i = 0; i < snakeLen; i++) {
                uint8_t pos = (snakeHead >= i) ? (snakeHead - i) : (SNAKE_STEPS + snakeHead - i);
                uint8_t d = snakeTrack[pos].digit;
                uint8_t s = snakeTrack[pos].mask;
                CathodeBuf[d] &= ~s;

                // hide apple ONLY if the snake's body is over it
                if (appleDigit == 0 && d == 1 && s == 0x08) appleVisible = FALSE;      // D1 Bottom
                else if (appleDigit == 1 && d == 2 && s == 0x08) appleVisible = FALSE; // D2 Top
                else if (appleDigit == 2 && d == 0 && s == 0x08) appleVisible = FALSE; // D0 Bottom
            }

            // draw apple, only if the digit is completely clear of the snake
            if (appleVisible) {
                if (appleDigit == 0)      segs[H01] &= 0x7F;   // Apple 0: D1 Bottom
                else if (appleDigit == 1) segs[M10] &= 0x7F;   // Apple 1: D2 Top
                else if (appleDigit == 2) segs[H10] &= 0x7F;   // Apple 2: D0 Bottom
            }
        }

        // move snake every ~100ms
        if ((uint8_t)(colonTick - snakeTimer) >= 32) {
            snakeTimer = colonTick;

            if (++snakeHead >= SNAKE_STEPS) {
                snakeHead = 0;
                snakeLaps++;
            }

            // detect collision (eating apple)
            {
                uint8_t hd = snakeTrack[snakeHead].digit;
                uint8_t hs = snakeTrack[snakeHead].mask;

                if (appleDigit == 0 && hd == 1 && hs == 0x08) {
                    if (snakeLen < 8) snakeLen++;
                    appleDigit = 1;                     // Spawn Apple 1
                } else if (appleDigit == 1 && hd == 2 && hs == 0x08) {
                    if (snakeLen < 8) snakeLen++;
                    appleDigit = 2;                     // Spawn Apple 2
                } else if (appleDigit == 2 && hd == 0 && hs == 0x08) {
                    if (snakeLen < 8) snakeLen++;
                    appleDigit = 0;                     // Spawn Apple 0
                }
            }

            // run for 5 laps regardless of apples
            if (snakeLaps >= 5 && snakeHead == 0) {
                transitionTo(stClock);
            }
        }

        // exit to clock on any button
        if (checkAndClearS1() || checkAndClearS2() || checkAndClearLongS1() || checkAndClearLongS2()) {
            transitionTo(stClock);
        }
        break;

// ###################################################################################
// Setup / Config
// ###################################################################################

// scSet,scBeep,scDsp,scCfg

    case scSet:
        setAllDigits(txSet);
        stateSwitchWithS1(scBeep);
        stateSwitchWithS2(msClock);
        break;

    case scBeep:
        setAllDigits(txBeep);
        stateSwitchWithS1(scDsp);
        stateSwitchExtendedWithS2(msAlarmOn,txAlarm,AlarmOn);
        break;

#if OPT_TEMP_DSP | OPT_DATE_DSP | OPT_DAY_DSP
    case scDsp:
        setAllDigits(txDsp);
        stateSwitchWithS1(scCfg);
  #if OPT_TEMP_DSP
        stateSwitchExtendedWithS2(msDspTempOn,txTemp,TempOn);
  #else
        stateSwitchExtendedWithS2(msDateOn,txDate,DateOn);
  #endif


        break;
#endif

    case scCfg:
        setAllDigits(txCfg);
        stateSwitchWithS1(msExit);
#if OPT_UNITS_GROUP
        stateSwitchWithS2(msSetUnits);
#else
        stateSwitchWithS2(msFormatTime);
#endif

        break;

// ###################################################################################
// Set Menu
// ###################################################################################

// msClock,msClockHour,msClockMinute

    case msClock:
        setHourDigits(txClock);
        stateSwitchWithS1(msAlarm);
        stateSwitchWithS2(msClockHour);
        break;

    case msClockHour:
        #if DP1_IS_COLON
            dp1 = ON;
            dp2 = OFF;
        #else
            dp1 = ON;
            dp2 = ON;
        #endif
        dp3 = AM_PM;
        displayHoursFlash();
        displayMinutesOn();
        stateSwitchWithS1(msClockMinute);
        if (checkAndClearS2()){
            clockRam.hr = incrementHours(clockRam.hr);
            setupHour(clockRam.hr);
            timeChanged = TRUE;
        }
        break;

    case msClockMinute:
        displayHoursOn();
        displayMinutesFlash();
        stateSwitchWithS1(msAlarm);
        if (checkAndClearS2()){
            m = incrementMinutes(m);
            clockRam.min = m;
            timeChanged = TRUE;
        }
        break;

// msAlarm,msAlarmHour,msAlarmMinute

    case msAlarm:
        setHourDigits(txAlarm);
        stateSwitchWithS1(msChime);
        stateSwitchWithS2(msAlarmHour);
        break;

    case msAlarmHour:
        #if DP1_IS_COLON
            dp1 = ON;
            dp2 = OFF;
        #else
            dp1 = ON;
            dp2 = ON;
        #endif
        dp3 = AM_PM;
        setupHour(clockRam.almHour);
        m = clockRam.almMin;
        displayHoursFlash();
        displayMinutesOn();
        stateSwitchWithS1(msAlarmMinute);
        if (checkAndClearS2())
            clockRam.almHour = incrementHours(clockRam.almHour);
        break;

    case msAlarmMinute:
        displayHoursOn();
        displayMinutesFlash();
        stateSwitchWithS1(msChime);
        if (checkAndClearS2()){
            m = incrementMinutes(m);
            clockRam.almMin = m;
            }
        break;

// msChime,msChimeStartHour,msChimeStopHour

    case msChime:
        setHourDigits(txChime);
#if OPT_DATE_DSP
        stateSwitchWithS1(msDate);
#elif OPT_DAY_DSP
        stateSwitchWithS1(msDay);
#else
        stateSwitchWithS1(msExit);
#endif
        stateSwitchWithS2(msChimeStartHour);
        break;

    case msChimeStartHour:
        setChimeVars();
        displayHoursFlash();
        displayMinutesOn();
        stateSwitchWithS1(msChimeStopHour);
        if (checkAndClearS2())
            clockRam.chimeStartHour = incrementHours(clockRam.chimeStartHour);
        break;

    case msChimeStopHour:
        setChimeVars();
        displayHoursOn();
        displayMinutesFlash();
#if OPT_DATE_DSP
        stateSwitchWithS1(msDate);
#elif OPT_DAY_DSP
        stateSwitchWithS1(msDay);
#else
        stateSwitchWithS1(msExit);
#endif
        if (checkAndClearS2())
            clockRam.chimeStopHour = incrementHours(clockRam.chimeStopHour);
        break;

// msDate,msDateMonth,msDateDay

#if OPT_DATE_DSP
    case msDate:
        dp1 = ON;
        dp2 = OFF;
        dp3 = OFF;
        setHourDigits(txDate);
  #if OPT_DAY_DSP
        stateSwitchWithS1(msDay);
  #else
        stateSwitchWithS1(msYear);
  #endif
        stateSwitchWithS2(msDateMonth);
        break;

    case msDateMonth:
        getDateVars();
        displayHoursFlash();
        displayMinutesOn();
        stateSwitchWithS1(msDateDay);
        if (checkAndClearS2())
            h = incrementDate(h,H10);
        break;

    case msDateDay:
        getDateVars();
        displayHoursOn();
        displayMinutesFlash();
  #if OPT_DAY_DSP
        stateSwitchWithS1(msDay);
  #else
        stateSwitchWithS1(msYear);
  #endif
        if (checkAndClearS2())
            m = incrementDate(m,M10);
        break;
#endif

// msDay,msDayOfWeek

#if OPT_DAY_DSP
    case msDay:
        setHourDigits(txDay);
        stateSwitchWithS1(msYear);
        stateSwitchWithS2(msDayOfWeek);
        break;

    case msDayOfWeek:
        setDayOfWeek();
        stateSwitchWithS1(msYear);
        if (checkAndClearS2())
            clockRam.day = incrementDay(clockRam.day);
        break;
#endif

    case msYear:
        setAllDigits(txYear);
        dp0 = OFF;
        dp1 = OFF;
        dp2 = OFF;
        dp3 = OFF;
        stateSwitchWithS1(msExit);
        stateSwitchWithS2(msSetYear);
        break;

    case msSetYear:
        dp0 = OFF;
        dp1 = OFF;
        dp2 = ON;
        dp3 = OFF;
        segs[H10] = BLANK_CHAR;
        segs[H01] = BLANK_CHAR;
        if (getStateS2Flasher()) {
            segs[M10] = ledSegBT[clockRam.yr >> 4] & 0x7F;
            segs[M01] = ledSegTB[clockRam.yr & 0x0F];
        } else {
            segs[M10] = BLANK_CHAR & 0x7F;
            segs[M01] = BLANK_CHAR;
        }

        stateSwitchWithS1(msExit);
        if (checkAndClearS2()) {
            clockRam.yr = incrementYear(clockRam.yr);
            timeChanged = TRUE;
        }
        break;

// ###################################################################################
// Beep Menu
// ###################################################################################

// msAlarmOff,msAlarmOn

    case msAlarmOff:
        setMsgOff();
        AlarmOn = FALSE;
        stateSwitchExtendedWithS1(msChimeOn,txChime,ChimeOn);
        stateSwitchWithS2(msAlarmOn);
        break;

    case msAlarmOn:
        setMsgOn();
        AlarmOn = TRUE;
        stateSwitchExtendedWithS1(msChimeOn,txChime,ChimeOn);
        stateSwitchWithS2(msAlarmOff);
        break;

// ChimeOff,ChimeOn

    case msChimeOff:
        setMsgOff();
        ChimeOn = FALSE;
        stateSwitchWithS1(msExit);
        stateSwitchWithS2(msChimeOn);
        break;

    case msChimeOn:
        setMsgOn();
        ChimeOn = TRUE;
        stateSwitchWithS1(msExit);
        stateSwitchWithS2(msChimeOff);
        break;

// ###################################################################################
// Display Menu
// ###################################################################################

// msDspTempOff, msDspTempOn

#if OPT_TEMP_DSP
    case msDspTempOff:
        setMsgOff();
        TempOn = FALSE;
#if OPT_DATE_DSP
        stateSwitchExtendedWithS1(msDateOn,txDate,DateOn);
#elif OPT_DAY_DSP
        stateSwitchExtendedWithS1(msDayOn,txDay,DowOn);
#else
        stateSwitchWithS1(msRot);
#endif
        stateSwitchWithS2(msDspTempOn);
        break;

    case msDspTempOn:
        setMsgOn();
        TempOn = TRUE;
#if OPT_DATE_DSP
        stateSwitchExtendedWithS1(msDateOn,txDate,DateOn);
#elif OPT_DAY_DSP
        stateSwitchExtendedWithS1(msDayOn,txDay,DowOn);
#else
        stateSwitchWithS1(msRot);
#endif
        stateSwitchWithS2(msDspTempOff);
        break;
#endif

// msDateOff, msDateOn

#if OPT_DATE_DSP
    case msDateOff:
        setMsgOff();
        DateOn = FALSE;
  #if OPT_DAY_DSP
        stateSwitchExtendedWithS1(msDayOn,txDay,DowOn);
  #else
        stateSwitchWithS1(msRot);
  #endif
        stateSwitchWithS2(msDateOn);
        break;

    case msDateOn:
        setMsgOn();
        DateOn = TRUE;
  #if OPT_DAY_DSP
        stateSwitchExtendedWithS1(msDayOn,txDay,DowOn);
  #else
        stateSwitchWithS1(msRot);
  #endif
        stateSwitchWithS2(msDateOff);
        break;
#endif

// msDayOff, msDayOn

#if OPT_DAY_DSP
    case msDayOff:
        setMsgOff();
        DowOn = FALSE;
        stateSwitchWithS1(msRot);               // S1 moves to rotation cadence setting
        stateSwitchWithS2(msDayOn);
        break;

    case msDayOn:
        setMsgOn();
        DowOn = TRUE;
        stateSwitchWithS1(msRot);               // S1 moves to rotation cadence setting
        stateSwitchWithS2(msDayOff);
        break;
#endif

// msRot: Rotation cadence setting (rot0 .. rot3)
    case msRot:
        setAllDigits(txRot);
        dp0 = OFF;
        dp1 = OFF;
        dp2 = OFF;
        dp3 = OFF;
        segs[M01] = ledSegTB[GET_ROT_MODE()];

        stateSwitchWithS1(msDur);               // S1 moves to page duration setting

        if (checkAndClearS2()) {
            uint8_t rot = GET_ROT_MODE() + 1;
            if (rot > 3) rot = 0;
            SET_ROT_CONFIG(rot, GET_PAGE_DUR());
        }
        break;

// msDur: Page duration setting (dur1 .. dur5)
    case msDur:
        setAllDigits(txDur);
        dp0 = OFF;
        dp1 = OFF;
        dp2 = OFF;
        dp3 = OFF;
        segs[M01] = ledSegTB[GET_PAGE_DUR()];

        stateSwitchWithS1(msAni);              // S1 moves to animation setting

        if (checkAndClearS2()) {
            uint8_t dur = GET_PAGE_DUR() + 1;
            if (dur > 5) dur = 1;
            SET_ROT_CONFIG(GET_ROT_MODE(), dur);
        }
        break;

    case msAni:
        setAllDigits(txAni);
        dp0 = OFF; dp1 = OFF; dp2 = OFF; dp3 = OFF;
        segs[M01] = ledSegTB[GET_ANI_MODE()];

        stateSwitchWithS1(msExit);              // S1 saves to NVRAM and exits
        if (checkAndClearS2()) {
            uint8_t a = GET_ANI_MODE() + 1;
            if (a > 5) a = 0;
            SET_ANI_MODE(a);
        }
        break;

// ###################################################################################
// end of menu routines
// ###################################################################################

// ###################################################################################
// begin configuration routines
// ###################################################################################

#if OPT_UNITS_GROUP
    case msSetUnits:
        setAllDigits(txUnit);
        stateSwitchWithS1(msZero);
        stateSwitchExtendedWithS2(msUS,NoText2,Select_12);
        break;

    case msEU:
        setMinuteDigits(txEU);
        changeTimeFormat(FALSE);
        stateSwitchWithS1(msZero);
        stateSwitchWithS2(msUS);
        break;

    case msUS:
        setMinuteDigits(txUS);
        changeTimeFormat(TRUE);
        stateSwitchWithS1(msZero);
        stateSwitchWithS2(msEU);
        break;
#else
    case msFormatTime:
        setAllDigits(tx1224);
        stateSwitchWithS1(msTempUnits);
        stateSwitchExtendedWithS2(ms12,NoText2,Select_12);
        break;

    case ms12:
        setMinuteDigits(tx12);
        changeTimeFormat(TRUE);
        stateSwitchWithS1(msTempUnits);
        stateSwitchWithS2(ms24);
        break;

    case ms24:
        setMinuteDigits(tx24);
        changeTimeFormat(FALSE);
        stateSwitchWithS1(msTempUnits);
        stateSwitchWithS2(ms12);
        break;

    case msTempUnits:
        setAllDigits(txTemp4);
        stateSwitchWithS1(msFormatDate);
        stateSwitchExtendedWithS2(msF,NoText2,Select_FC);
        break;

    case msF:
        setMinuteDigits(txF);
        Select_FC = TRUE;
        stateSwitchWithS1(msFormatDate);
        stateSwitchWithS2(msC);
        break;

    case msC:
        setMinuteDigits(txC);
        Select_FC = FALSE;
        stateSwitchWithS1(msFormatDate);
        stateSwitchWithS2(msF);
        break;

    case msFormatDate:
        setAllDigits(txDate4);
        stateSwitchWithS1(msZero);
        stateSwitchExtendedWithS2(ms1231, NoText2, Select_MD);
        break;

    case ms1231:
        setAllDigits(tx1231);
        Select_MD = TRUE;
        DateDot2On = FALSE;
        stateSwitchWithS1(msZero);
        stateSwitchWithS2(ms3112);
        break;

    case ms3112:
        setAllDigits(tx3112);
        Select_MD = FALSE;
        DateDot2On = FALSE;
        stateSwitchWithS1(msZero);
        stateSwitchWithS2(ms3112dot);
        break;

    case ms3112dot:
        setAllDigits(tx3112);
        segs[M01] &= 0x7F;
        Select_MD = FALSE;
        DateDot2On = TRUE;
        stateSwitchWithS1(msZero);
        stateSwitchWithS2(ms1231);
        break;
#endif

    case msZero:
        setAllDigits(txZero);
        dp1 = OFF; dp2 = OFF;
        stateSwitchWithS1(msBrightness);
        stateSwitchWithS2(msSetZero);
        break;

    case msSetZero:
        dp1 = ON; dp2 = ON;
        dp0 = OFF; dp3 = OFF;
        segs[H10] = BlankZeroOn ? BLANK_CHAR : ledSegTB[0];
        segs[H01] = ledSegTB[9] & 0x7F;
        segs[M10] = ledSegBT[5] & 0x7F;
        segs[M01] = ledSegTB[9];

        stateSwitchWithS1(msBrightness);
        if (checkAndClearS2()) {
            BlankZeroOn = !BlankZeroOn;
        }
        break;

    case msBrightness:
        setAllDigits(txBrt);
#if OPT_TEMP_DSP
        stateSwitchWithS1(msTempCal);
#else
        stateSwitchWithS1(msVersion);
#endif
        stateSwitchWithS2(msBrtMax);
        break;

    case msBrtMax:
        dp1 = ON;
        dp2 = OFF;
        dp3 = OFF;
        h = clockRam.brightMax;
        m = clockRam.brightMin;
        displayHoursFlash();
        displayMinutesOn();
        stateSwitchWithS1(msBrtMin);
        if (checkAndClearS2()){
            d = incrementBrightness(h);
            clockRam.brightMax = d;
        }
        break;

    case msBrtMin:
        m = clockRam.brightMin;
        displayHoursOn();
        displayMinutesFlash();
#if OPT_TEMP_DSP
        stateSwitchWithS1(msTempCal);
#else
        stateSwitchWithS1(msVersion);
#endif
        if (checkAndClearS2()){
            d = incrementBrightness(m);
            if ( d > clockRam.brightMax ) d = 1;
            clockRam.brightMin = d;
        }
        break;

#if OPT_TEMP_DSP
    case msTempCal:
        // fix bright from previous set and go back if needed
        // until min <= max
        if (clockRam.brightMin > clockRam.brightMax){
            clockRam.brightMin = clockRam.brightMax;
            displayState = msBrtMax;
            break;
        }
        setAllDigits(txCal);
        stateSwitchWithS1(msVersion);
        stateSwitchWithS2(msSetTemp);
        break;

    case msSetTemp:
        displayTemperature();
        stateSwitchWithS1(msVersion);
        if (checkAndClearS2()){
            if (clockRam.tempOffset == 15)
                clockRam.tempOffset = -15;
            else
                clockRam.tempOffset += 1;
        }
        break;
#endif

    case msVersion:
        setAllDigits(txVer);
        dp0 = OFF;
        dp1 = OFF;
        dp2 = OFF;
        dp3 = OFF;
        stateSwitchWithS1(msExit);              // S1 steps out
        stateSwitchWithS2(msShowVersion);       // S2 opens version display
        break;

    case msShowVersion:
        dp0 = OFF;
        dp3 = OFF;
        segs[H10] = ledSegTB[FW_VER_MAJOR] & 0x7F;
        segs[H01] = ledSegTB[FW_VER_MINOR] & 0x7F;
#if FW_VER_REV < 10
        segs[M10] = ledSegBT[FW_VER_REV];
        segs[M01] = BLANK_CHAR;
#else
        segs[M10] = ledSegBT[FW_VER_REV / 10];
        segs[M01] = ledSegTB[FW_VER_REV % 10];
#endif
        stateSwitchWithS1(msExit);
        stateSwitchWithS2(msVersion);
        break;


// ###################################################################################
// end configuration routines
// ###################################################################################

// ###################################################################################
// all states that set something must exit through here
// ###################################################################################

    case msExit:
        blankDisplay();                 // go blank at end of cycle
        if (!timeChanged){
            refreshTime();              // time didn't change so refresh get current
        }
        else {                          //  time before writing everything back
            clockRam.sec = 0;           // reset seconds when changing time
            timeChanged = FALSE;
        }
        putClock();                     // save changes
        putConfigRam();                 // from any set routine...
        userTimer100 = 6;               // wait for ~.5 second
        while(userTimer100) ;           // just delay...
        checkAndClearS1();              // dump keybuf on the way out the door...
        checkAndClearS2();              // dump keybuf on the way out the door...
#ifdef S3
        checkAndClearS3();              // dump keybuf on the way out the door...
#endif
        displayState = stClock;         // goto clock display
        break;

    default:
        displayState = stClock;
    }
}

// ###################################################################################
// End state machine
// ###################################################################################

void setAllDigits(uint8_t index)
{
    segs[H10] = textDesc4[(index << 2)];
    segs[H01] = textDesc4[(index << 2)+1];
    segs[M10] = textDesc4[(index << 2)+2];
    segs[M01] = textDesc4[(index << 2)+3];
}

void setHourDigits(uint8_t index)
{
    segs[H10] = textDesc2[(index << 1)];
    segs[H01] = textDesc2[(index << 1)+1];
    segs[M10] = 0xFF;
    segs[M01] = 0xFF;
}

void setMinuteDigits(uint8_t index)
{
    segs[H10] = 0xFF;
    segs[H01] = 0xFF;
    segs[M10] = textDesc2[(index << 1)];
    segs[M01] = textDesc2[(index << 1)+1];
}

void setMsgOn()
{
    segs[M10] = textDesc2[(txOn << 1)];
    segs[M01] = textDesc2[(txOn << 1)+1];
}

void setMsgOff()
{
    segs[M10] = textDesc2[(txOff << 1)];
    segs[M01] = textDesc2[(txOff << 1)+1];
}


// Setup hour display if 12 hour format.
// Set the module level global var "h" used thoughout the state machine

void setupHour(uint8_t hour)
{
    if (Select_12){
        h = hour & 0x1F;
        AM_PM = (hour & 0x20)?1:0;
    }
    else {
        h = hour;
        AM_PM = FALSE;
    }
}

void setChimeVars()
{
    if (Select_12){
        h = clockRam.chimeStartHour & 0x1F;
        dp1 = (clockRam.chimeStartHour & 0x20)?1:0;
        m = clockRam.chimeStopHour  & 0x1F;
        dp3 = (clockRam.chimeStopHour  & 0x20)?1:0;
    }
    else {
        h = clockRam.chimeStartHour;
        dp1 = FALSE;
        m = clockRam.chimeStopHour;
        dp3 = FALSE;
    }
    dp2 = OFF;
}

void getDateVars()
{
    if (Select_MD){
        h = clockRam.mon;
        m = clockRam.date;
        dp3 = OFF;
    }
    else {
        h = clockRam.date;
        m = clockRam.mon;
        dp3 = ON;  // German / European format: DD.MM.
    }
    dp1 = ON;
}

void putDateVars()
{
    if (Select_MD){
        clockRam.mon = h;
        clockRam.date = m;
    }
    else {
        clockRam.date = h;
        clockRam.mon = m;
    }
}

void displayTemperature()
{
    h = decToBcd(actualTemp + clockRam.tempOffset);
    segs[H10] = ledSegTB[(h & 0xF0) >> 4];
    segs[H01] = ledSegTB[(h & 0x0F)];
#if DIGIT_3_FLIP
    if ( Select_FC)
        segs[M10] = 0x31;  // F & dp on
    else
        segs[M10] = 0x70;  // C & dp on
#else
    if ( Select_FC)
        segs[M10] = 0x8E;  // F only
    else
        segs[M10] = 0xC6;  // C only
#endif
    segs[M01] = 0xFF;
}

void displayDate()
{
    dp1 = ON;
    dp2 = OFF;
    dp3 = (!Select_MD && DateDot2On) ? ON : OFF;  // German / European dot on digit 4
    getDateVars();
    displayHoursOn();
    displayMinutesOn();
}

#if OPT_DAY_DSP
void setDayOfWeek()
{
  #if OPT_DAY_ALPHA
	if (getStateS2Flasher()){
		displayDayOfWeek();
	}
	else {
        segs[H10] = BLANK_CHAR;
        segs[H01] = BLANK_CHAR;
        segs[M10] = BLANK_CHAR;
    	segs[M01] = BLANK_CHAR;
	}
  #else
        dp1 = OFF;              // hold-over from date display
        m = clockRam.day;
        displayHoursOff();
        displayMinutesFlash();
		//displayDayOfWeek();
  #endif
}
#endif

#if OPT_DAY_DSP

void displayDayOfWeek()
{
    dp1 = OFF;
    dp2 = OFF;
    dp3 = OFF;
  #if OPT_DAY_ALPHA
    h = clockRam.day;               // use hours (unused right now)
    __asm
    mov     _CathodeBuf,#0xFF	    ; blank first digit
    mov     r0,#_CathodeBuf+1	    ; ram destination address
    mov     r1,_h         		    ; get day of week and make it zero origin
    dec     r1
    mov     a,r1
    add     a,r1
    add     a,r1                  ; *3 bytes per text entry
    mov     r1,a
    mov     r2,#3                 ; number of bytes to move
    mov     dptr,#_ledDOW         ; source table address
    jnb     _Select_MD,pLp1
    inc     dptr                  ; adjust offset for US DOW table
    inc     dptr                  ; adjust offset for US DOW table
    inc     dptr                  ; adjust offset for US DOW table
pLp1:
    mov     a,r1                  ; offset into code seg source
    movc    a,@a+dptr             ; get byte in code seg from @(r1+dpr)
    mov     @r0,a                 ; put byte to ram @r0
    inc     r0
    inc     r1
    djnz    r2,pLp1	              ; loop for r2 bytes
    __endasm;
  #else
    segs[H10] = BLANK_CHAR;
    segs[H01] = 0xBF;
    segs[M10] = ledSegBT[clockRam.day];
    segs[M01] = 0xBF;
  #endif
}

#endif

void blankDisplay(void)
{
    CathodeBuf[0] = 0xFF;           // turn off all segments
    CathodeBuf[1] = 0xFF;           // in all digits
    CathodeBuf[2] = 0xFF;           // to eliminate 88:88 display
    CathodeBuf[3] = 0xFF;           // at power up
}

uint8_t incrementHours(uint8_t hour)
{
    if (Select_12) hour = toFormat24(hour);     // change to 24hr format for increment
    __asm
    mov     a,r7;
    add     a,#1;
    da      a;
    mov     r7,a;
    __endasm;
    if ( hour == 0x24 ) hour = 0;
    if (Select_12) hour = toFormat12(hour);     // if mode is 12hr, convert back
    return hour;
}

uint8_t incrementMinutes(uint8_t min)
{
    __asm
    mov     a,r7;
    add     a,#1;
    da      a;
    mov     r7,a;
    __endasm;
    if ( min == 0x60 ) min = 0;
    return min;
}

uint8_t getMaxDays(uint8_t monBcd)
{
    uint8_t m = bcdToDec(monBcd);

    if (m == 2) {
        uint8_t y = bcdToDec(clockRam.yr);
        return ((y & 0x03) == 0) ? 0x29 : 0x28;
    }

    if (m == 4 || m == 6 || m == 9 || m == 11) {
        return 0x30;
    }

    return 0x31;
}

uint8_t incrementDate(uint8_t value, uint8_t position)
{
    uint8_t r;
    if (position == H10){
        if (Select_MD){
           r = incMonths(value);
           if (clockRam.date > getMaxDays(clockRam.mon)) clockRam.date = getMaxDays(clockRam.mon);
        } else {
           r = incDays(value);
        }
    } else {
        if (Select_MD) {
           r = incDays(value);
        } else {
           r = incMonths(value);
           if (clockRam.date > getMaxDays(clockRam.mon)) clockRam.date = getMaxDays(clockRam.mon);
        }
    }
    return r;
}

uint8_t incMonths(uint8_t month)
{
    __asm
    mov     a,r7;
    add     a,#1;
    da      a;
    mov     r7,a;
    __endasm;
    if ( month == 0x13 ) month = 1;
    clockRam.mon = month;
    return month;
}

uint8_t incDays(uint8_t day)
{
    uint8_t maxDay = getMaxDays(clockRam.mon);

    __asm
    mov     a,r7;
    add     a,#1;
    da      a;
    mov     r7,a;
    __endasm;

    // wrap around to 1 if it exceeds the month's maximum
    if (day > maxDay) day = 1;

    clockRam.date = day;
    return day;
}

uint8_t incrementDay(uint8_t day)
{
    __asm
    mov     a,r7;
    add     a,#1;
    da      a;
    mov     r7,a;
    __endasm;
    if ( day == 0x8 ) day = 1;
    return day;
}

uint8_t incrementBrightness(uint8_t brt)
{
    __asm
    mov     a,r7;
    add     a,#1;
    da      a;
    mov     r7,a;
    __endasm;
    if ( bcdToDec(brt) > MAX_BRIGHT ) brt = MIN_BRIGHT;
    return brt;
}

/*
void displayHours(__bit blink)
{
    maskOnOff = blink ? 0x00:0xFF;
    maskDp0   = dp0?0x7F:0xFF;
    maskDp1   = dp1?0x7F:0xFF;
    segs[H10] = (ledSegTB[(h & 0xF0) >> 4] | maskOnOff) & maskDp0;
    segs[H01] = (ledSegTB[(h & 0x0F)]      | maskOnOff) & maskDp1;
}
*/

void displayHours()
{
    __asm
_displayHoursOn:
    setb    c
    sjmp    L001                ; mask =00 when on

_displayHoursOff:
    clr     c
    sjmp    L001                ; mask =FF when off

_displayHoursFlash:
    lcall	_getStateS2Flasher  ; get CY bit for on/off
L001:
    mov     a,#0xFF
    addc    a,#0
    mov     _maskOnOff,a        ; =0 when cy set, =ff when not

    mov     a,#BLANK_CHAR
    mov     c,_dp0
    cpl     c                   ; 0x7f = turn it on
    rrc     a                   ; 0xFF = turn it off
    mov     _maskDp0,a

    mov     a,#BLANK_CHAR
    mov     c,_dp1
    cpl     c
    rrc     a
    mov     _maskDp1,a

    mov     dptr,#_ledSegTB
    mov     a,_h                ; get hours
    swap    a                   ; cheap way to shift right 4 places
    anl     a,#0x0f
    jnb     _blankZero,L0011    ; not blanking, leave
    jnz     L0011               ; blanking but not zero, leave
    mov     a,#0xFF             ; A was 0 on entry
    sjmp    L0012               ; write FF to display to blank digit
L0011:
    movc    a,@a+dptr
    orl     a,_maskOnOff
    anl     a,_maskDp0
L0012:
    mov     _CathodeBuf,a

    mov     a,#0x0f
    anl     a,_h
    movc	a,@a+dptr
    orl     a,_maskOnOff
    anl     a,_maskDp1
    mov     (_CathodeBuf + 1),a
    __endasm;
}

/*
void displayMinutes(__bit blink)
{
    maskOnOff = blink ? 0x00:0xFF;
    maskDp2   = dp2?0x7F:0xFF;
    maskDp3   = dp3?0x7F:0xFF;
    segs[M10] = (ledSegBT[(m & 0xF0) >> 4] | maskOnOff) & maskDp2;
    segs[M01] = (ledSegTB[(m & 0x0F)]      | maskOnOff) & maskDp3;
}
*/

void displayMinutes()
{
    __asm
_displayMinutesOn:
    setb    c
    sjmp    L002

_displayMinutesOff:
    clr     c
    sjmp    L002

_displayMinutesFlash:
    lcall	_getStateS2Flasher
L002:
    mov     a,#0xFF
    addc    a,#0
	mov     _maskOnOff,a        ; =0 when cy set, =ff when not

    mov     a,#0xFF
    mov     c,_dp2
    cpl     c                   ; 0x7f = turn it on
    rrc     a                   ; 0xFF = turn it off
	mov     _maskDp2,a

    mov     a,#0xFF
    mov     c,_dp3
    cpl     c
    rrc     a
	mov     _maskDp3,a

	mov	    a,_m                ; get minutes
	swap	a                   ; cheap way to shift right 4 places
	anl     a,#0x0f
	mov     dptr,#_ledSegBT     ; special digit LED pattern
	movc	a,@a+dptr
	orl 	a,_maskOnOff
	anl 	a,_maskDp2
	mov 	(_CathodeBuf+2),a

	mov     a,#0x0f
	anl     a,_m
	mov     dptr,#_ledSegTB     ; use reg ular pattern
	movc	a,@a+dptr
	orl     a,_maskOnOff
	anl     a,_maskDp3
	mov     (_CathodeBuf+3),a
    __endasm;
}


