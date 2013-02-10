/*
 * Wearlink
 *
 * A fibretronic keypad to usb multimedia key code translator
 *
 * (c) 2013 Willem Dijkstra
 *
 * Based V-USB "EasyLogger", (c) 2006 OBJECTIVE DEVELOPMENT Software GmbH.
 */

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

#include "usbdrv.h"
#include "oddebug.h"

/*
Pin assignment:
PB4 = analog input (ADC2)
PB0, PB2 = USB data lines
*/

#define BIT_LED 3

#define UTIL_BIN4(x)        (uchar)((0##x & 01000)/64 + (0##x & 0100)/16 + (0##x & 010)/4 + (0##x & 1))
#define UTIL_BIN8(hi, lo)   (uchar)(UTIL_BIN4(hi) * 16 + UTIL_BIN4(lo))

static uchar    reportBuffer[2];    /* buffer for HID reports */
static uchar    idleRate;           /* in 4 ms units */

static uchar    adcPending;

static uchar    mmKey;

const PROGMEM char usbHidReportDescriptor[USB_CFG_HID_REPORT_DESCRIPTOR_LENGTH] = { /* USB report descriptor */
    0x05, 0x01,                    // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,                    // USAGE (Keyboard)
    0xa1, 0x01,                    // COLLECTION (Application)

    0x95, 0x01,                    //   REPORT_COUNT(1)
    0x75, 0x08,                    //   REPORT_SIZE(8)
    0x15, 0x00,                    //   LOGICAL_MINIMUM(0)
    0x25, 0xff,                    //   LOGICAL_MAXIMUM(1)
    0x05, 0x0C,                    //   USAGE_PAGE (Consumer)
    0x19, 0x00,                    //   USAGE_MINIMUM (0)
    0x29, 0xff,                    //   USAGE_MAXIMUM (0xff)
    0x81, 0x00,                    //   INPUT (Data, Array); Media Keys

    0x95, 0x01,                    //   REPORT_COUNT (1)
    0x75, 0x08,                    //   REPORT_SIZE (8)
    0x05, 0x07,                    //   USAGE_PAGE (Keyboard)
    0x25, 0x65,                    //   LOGICAL_MAXIMUM (101)
    0x19, 0x00,                    //   USAGE_MINIMUM (Reserved (no event indicated))
    0x29, 0x65,                    //   USAGE_MAXIMUM (Keyboard Application)
    0x81, 0x00,                    //   INPUT (Data,Ary,Abs)
    0xc0                           // END_COLLECTION
};
/* We use a simplifed keyboard report descriptor with 2 entries: one for
 * multimedia keys, and one for normal keys. We do not support modifiers or
 * status leds.
 */

/*
   Vcc is tied to the ADC pin using a 20k resistor. From there we connect to
   the coat keys and finally to ground. The different coat keys make a
   resistive divider at the ADC node. This table calculates the expected ADC
   measurement given the known resistance and an expected deviation of +-9%.

   #+ORGTBL: SEND keymarks orgtbl-to-generic :lstart "{ " :lend " }," :sep ", " :skip 2 :skipcols (3 4 5 6)
   | ! | Key           |       Rk |  Vadc |     Adc | Adc 8bit |    A-D% | A+D% |
   |---+---------------+----------+-------+---------+----------+---------+------|
   | # | KEY_NONE      | 10000000 | 4.990 |    1022 |      256 |     233 |  255 |
   | # | KEY_VOLUMEDEC |    10000 | 1.667 |     341 |       85 |      77 |   93 |
   | # | KEY_REWIND    |    20000 | 2.500 |     512 |      128 |     116 |  140 |
   | # | KEY_PLAY      |    46200 | 3.489 |     715 |      179 |     163 |  195 |
   | # | KEY_FORWARD   |    31600 | 3.062 |     627 |      157 |     143 |  171 |
   | # | KEY_VOLUMEINC |     5600 | 1.094 |     224 |       56 |      51 |   61 |
   |---+---------------+----------+-------+---------+----------+---------+------|
   | $ |               | Rl=20000 | Vcc=5 | bits=10 |  rbits=8 | dev=.09 |      |
   #+TBLFM: $4=$Vcc-($Rl/($Rk+$Rl))*$Vcc;%.3f::$5=$Vadc/($Vcc/2^$bits);%.0f::$6=$Adc/(2^($bits-$rbits));%.0f::$7=max($6-($dev*$6),0);%.0f::$8=min($6+($dev*$6),255);%0.f
*/

/* Codes to mark multimedia key presses, see usb.org's HID-usage-tables
 * document, chapter 15.
 */
#define KEY_NONE            0x00
#define KEY_VOLUMEINC       0xe9
#define KEY_VOLUMEDEC       0xea
#define KEY_REWIND          0xb4
#define KEY_FORWARD         0xb3
#define KEY_PLAY            0xb0

#define NUMKEYS             6

struct keymark {
    uchar key;
    uchar low;
    uchar high;
};

const struct keymark keymark[NUMKEYS] = {
/* BEGIN RECEIVE ORGTBL keymarks */
{ KEY_NONE, 233, 255 },
{ KEY_VOLUMEDEC, 77, 93 },
{ KEY_REWIND, 116, 140 },
{ KEY_PLAY, 163, 195 },
{ KEY_FORWARD, 143, 171 },
{ KEY_VOLUMEINC, 51, 61 },
/* END RECEIVE ORGTBL keymarks */
};


#define KEY_1       30
#define KEY_2       31
#define KEY_3       32
#define KEY_4       33
#define KEY_5       34
#define KEY_6       35
#define KEY_7       36
#define KEY_8       37
#define KEY_9       38
#define KEY_0       39
#define KEY_RETURN  40

/* ------------------------------------------------------------------------- */

static void buildReport(void)
{
    reportBuffer[0] = mmKey;
    mmKey = 0;
    reportBuffer[1] = 0;

    PORTB ^= 1 << BIT_LED;
}

static void evaluateADC(unsigned int value)
{
    uchar   i;
    uchar   v = (value >> 2) & 0xff;

    for (i = 0; i < NUMKEYS; i++) {
        if ((keymark[i].low <= v) && (v <= keymark[i].high))
            mmKey = keymark[i].key;
    }
}

static void adcPoll(void)
{
    if(adcPending && !(ADCSRA & (1 << ADSC))){
        adcPending = 0;
        evaluateADC(ADC);
    }
}

static void timerPoll(void)
{
    static uchar timerCnt;

    if(TIFR & (1 << TOV1)){
        TIFR = (1 << TOV1); /* clear overflow */
        if(++timerCnt >= 63){       /* ~ 1 second interval */
            timerCnt = 0;
            adcPending = 1;
            ADCSRA |= (1 << ADSC);  /* start next conversion */
//            PORTB ^= 1 << BIT_LED;
        }
    }
}

/* ------------------------------------------------------------------------- */

static void timerInit(void)
{
    TCCR1 = 0x0b;           /* select clock: 16.5M/1k -> overflow rate = 16.5M/256k = 62.94 Hz */
}

static void adcInit(void)
{
    ADMUX = _BV(MUX1); /* Vref=Vcc, no AREF, no left right adjust, measure ADC2 */
    ADCSRA = UTIL_BIN8(1000, 0111); /* enable ADC, not free running, interrupt disable, rate = 1/128 */
}

/* ------------------------------------------------------------------------- */
/* ------------------------ interface to USB driver ------------------------ */
/* ------------------------------------------------------------------------- */

uchar	usbFunctionSetup(uchar data[8])
{
    usbRequest_t    *rq = (void *)data;

    usbMsgPtr = reportBuffer;
    if((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS){    /* class request type */
        if(rq->bRequest == USBRQ_HID_GET_REPORT){  /* wValue: ReportType (highbyte), ReportID (lowbyte) */
            /* we only have one report type, so don't look at wValue */
            buildReport();
            return sizeof(reportBuffer);
        } else if(rq->bRequest == USBRQ_HID_GET_IDLE){
            usbMsgPtr = &idleRate;
            return 1;
        } else if(rq->bRequest == USBRQ_HID_SET_IDLE){
            idleRate = rq->wValue.bytes[1];
        }
    } else {
        /* no vendor specific requests implemented */
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* ------------------------ Oscillator Calibration ------------------------- */
/* ------------------------------------------------------------------------- */

/* Calibrate the RC oscillator to 8.25 MHz. The core clock of 16.5 MHz is
 * derived from the 66 MHz peripheral clock by dividing. Our timing reference
 * is the Start Of Frame signal (a single SE0 bit) available immediately after
 * a USB RESET. We first do a binary search for the OSCCAL value and then
 * optimize this value with a neighboorhod search.
 * This algorithm may also be used to calibrate the RC oscillator directly to
 * 12 MHz (no PLL involved, can therefore be used on almost ALL AVRs), but this
 * is wide outside the spec for the OSCCAL value and the required precision for
 * the 12 MHz clock! Use the RC oscillator calibrated to 12 MHz for
 * experimental purposes only!
 */
static void calibrateOscillator(void)
{
    uchar       step = 128;
    uchar       trialValue = 0, optimumValue;
    int         x, optimumDev, targetValue = (unsigned)(1499 * (double)F_CPU / 10.5e6 + 0.5);

    /* do a binary search: */
    do{
        OSCCAL = trialValue + step;
        x = usbMeasureFrameLength();    /* proportional to current real frequency */
        if(x < targetValue)             /* frequency still too low */
            trialValue += step;
        step >>= 1;
    } while (step > 0);
    /* We have a precision of +/- 1 for optimum OSCCAL here */
    /* now do a neighborhood search for optimum value */
    optimumValue = trialValue;
    optimumDev = x; /* this is certainly far away from optimum */
    for (OSCCAL = trialValue - 1; OSCCAL <= trialValue + 1; OSCCAL++){
        x = usbMeasureFrameLength() - targetValue;
        if (x < 0)
            x = -x;
        if (x < optimumDev){
            optimumDev = x;
            optimumValue = OSCCAL;
        }
    }
    OSCCAL = optimumValue;
}
/*
Note: This calibration algorithm may try OSCCAL values of up to 192 even if
the optimum value is far below 192. It may therefore exceed the allowed clock
frequency of the CPU in low voltage designs!
You may replace this search algorithm with any other algorithm you like if
you have additional constraints such as a maximum CPU clock.
For version 5.x RC oscillators (those with a split range of 2x128 steps, e.g.
ATTiny25, ATTiny45, ATTiny85), it may be useful to search for the optimum in
both regions.
*/

void    usbEventResetReady(void)
{
    /* Disable interrupts during oscillator calibration since
     * usbMeasureFrameLength() counts CPU cycles.
     */
    cli();
    calibrateOscillator();
    sei();
    eeprom_write_byte(0, OSCCAL);   /* store the calibrated value in EEPROM */
}

/* ------------------------------------------------------------------------- */
/* --------------------------------- main ---------------------------------- */
/* ------------------------------------------------------------------------- */

int main(void)
{
uchar   i;
uchar   calibrationValue;

    calibrationValue = eeprom_read_byte(0); /* calibration value from last time */
    if (calibrationValue != 0xff){
        OSCCAL = calibrationValue;
    }
    odDebugInit();
    usbDeviceDisconnect();
    for (i=0;i<20;i++){  /* 300 ms disconnect */
        _delay_ms(15);
    }
    usbDeviceConnect();
    wdt_enable(WDTO_1S);
    timerInit();
    adcInit();
    usbInit();

    DDRB |= _BV(BIT_LED);

    sei();
    for (;;){    /* main event loop */
        wdt_reset();
        usbPoll();
        if (usbInterruptIsReady() && mmKey != 0){ /* we can send another key */
            buildReport();
            usbSetInterrupt(reportBuffer, sizeof(reportBuffer));
        }
        timerPoll();
        adcPoll();
    }
    return 0;
}
