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
 * Pin assignment:
 * PB4 = analog input (ADC2)
 * PB0, PB2 = USB data lines
*/

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

    /* Add dummy empty byte to ensure that our multimedia key is sent to the
     * active application instead of the main system. Without this extra empty
     * key press, my phone start the general multimedia application instead of
     * allowing me to control the active application on the screen. */

    0x95, 0x01,                    //   REPORT_COUNT(1)
    0x75, 0x08,                    //   REPORT_SIZE(8)
    0x15, 0x00,                    //   LOGICAL_MINIMUM(0)
    0x25, 0x65,                    //   LOGICAL_MAXIMUM(101)
    0x05, 0x07,                    //   USAGE_PAGE (Keyboard)
    0x19, 0x00,                    //   USAGE_MINIMUM (0)
    0x29, 0x65,                    //   USAGE_MAXIMUM (101)
    0x81, 0x00,                    //   INPUT (Data, Array) normal keys

    0xc0                           // END_COLLECTION
};
/* We use a simplifed keyboard report descriptor with 1 entry for multimedia
 * keys. We do not support modifiers, status leds or normal keys.
 */

/*
   Vcc is tied to the ADC pin using a 20k resistor. From there we connect to
   the coat keys and finally to ground. The different coat keys make a
   resistive divider at the ADC node. This table calculates the expected ADC
   measurement given the known resistance and an expected deviation of +-9%.

   #+ORGTBL: SEND keymarks orgtbl-to-generic :lstart "   { " :lend " }," :sep ", " :skip 2 :skipcols (3 4 5 6)
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
#define KEY_PLAY            0xcd

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

/* ------------------------------------------------------------------------- */

static void buildReport(void)
{
    reportBuffer[0] = mmKey;
    reportBuffer[1] = 0;
}

static void evaluateADC(unsigned int value)
{
    uchar   i;
    uchar   v = (value >> 2) & 0xff;

    for (i = 0; i < NUMKEYS; i++) {
        if ((keymark[i].low <= v) && (v <= keymark[i].high)) {
            mmKey = keymark[i].key;
            break;
        }
    }
}

static void adcPoll(void)
{
    if(adcPending && !(ADCSRA & _BV(ADSC))){
        adcPending = 0;
        evaluateADC(ADC);
    }
}

static void timerPoll(void)
{
    static uchar timerCnt;

    if(TIFR & _BV(TOV1)){
        TIFR = _BV(TOV1);           /* clear overflow */
        if(++timerCnt >= 15){       /* ~ 1/4 s interval */
            timerCnt = 0;
            adcPending = 1;
            ADCSRA |= _BV(ADSC);    /* start next conversion */
        }
    }
}

static void timerInit(void)
{
    /* Select prescale as 1k; overflow rate is then 16M5 / (1k * 256) = 64.45 Hz */
    TCCR1 = _BV(CS13) | _BV(CS11) | _BV(CS10);
}

static void adcInit(void)
{
    /* Vref=Vcc, no AREF, no left right adjust, measure ADC2 */
    ADMUX = _BV(MUX1);
    /* Enable ADC, no interrupt, no free run, prescale = clock/128 */
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
}

uchar	usbFunctionSetup(uchar data[8])
{
    usbRequest_t    *rq = (void *)data;

    usbMsgPtr = reportBuffer;
    if ((rq->bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS) {
        /* class request type */
        if (rq->bRequest == USBRQ_HID_GET_REPORT) {
            /* wValue: ReportType (highbyte), ReportID (lowbyte) */
            /* we only have one report type, so don't look at wValue */
            buildReport();
            return sizeof(reportBuffer);
        } else if (rq->bRequest == USBRQ_HID_GET_IDLE) {
            usbMsgPtr = &idleRate;
            return 1;
        } else if (rq->bRequest == USBRQ_HID_SET_IDLE) {
            idleRate = rq->wValue.bytes[1];
        }
    } else {
        /* no vendor specific requests implemented */
    }
    return 0;
}

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

int main(void)
{
    uchar   i;
    uchar   sendClear = 0;
    uchar   calibrationValue;

    calibrationValue = eeprom_read_byte(0); /* calibration value from last time */
    if (calibrationValue != 0xff){
        OSCCAL = calibrationValue;
    }
    odDebugInit();
    usbDeviceDisconnect();

    for (i = 0;i < 20; i++){  /* 300 ms disconnect */
        _delay_ms(15);
    }

    usbDeviceConnect();
    wdt_enable(WDTO_1S);

    /* Reduce power by disabling unused subsystems: */
    PRR = _BV(PRTIM0) | _BV(PRUSI);

    timerInit();
    adcInit();
    usbInit();

    sei();
    for (;;){    /* main event loop */
        wdt_reset();
        usbPoll();

        /* First round will fire for an mmKey, and second round for a sendClear */
        if (usbInterruptIsReady()) {
            if (mmKey || sendClear) {
                buildReport();
                mmKey = 0;
                sendClear ^= 1;
                usbSetInterrupt(reportBuffer, sizeof(reportBuffer));
            }
        }
        timerPoll();
        adcPoll();
    }
    return 0;
}
