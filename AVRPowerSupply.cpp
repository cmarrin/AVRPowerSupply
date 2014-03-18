//
//  AVRPowerSupply.cpp
//
//  Created by Chris Marrin on 3/9/2014.
//

#include "m8r.h"
#include "System.h"
#include "EventListener.h"
#include "DeviceStream.h"
#include "INA219.h"
#include "TextLCD.h"
#include "Timer0.h"
#include "TimerEventMgr.h"

//
//  AVR Based Power Supply
//
// This is for a power supply using a pair of MIC2941s. The regulators are manually
// controlled, one by a 10 turn potentiometer and the other can be switched between
// 3.3 and 5v. There is an INA129 current sensor connected to the output of each.
// The INA129's are controlled through SPI connected to Port C4 (SDA) and C5 (SCL).
// The current sensor for the variable supply is device 0 and the switchable supply
// is device 1. The shutdown pin of the MIC2941s are connected to pin D6 (variable)
// and D7 (switchable). The supply board also has 4 analog inputs connected to 
// the AVR analog pins 0-3 (C0 - C3). A HD44780 compatible 2x16 text LCD is connected
// as follows:
//
//  RS      - Port B4
//  Enable  - Port B3
//  D0      - Port D5
//  D1      - Port D4
//  D2      - Port D3
//  D3      - Port D2
//
// A status LED is on port B5
//
// 

using namespace m8r;

#define StatusLED OutputBit<Port<B>, 5>
#define LCDRS OutputBit<Port<B>, 4>
#define LCDEnable OutputBit<Port<B>, 3>
#define LCDD0 OutputBit<Port<D>, 5>
#define LCDD1 OutputBit<Port<D>, 4>
#define LCDD2 OutputBit<Port<D>, 3>
#define LCDD3 OutputBit<Port<D>, 2>
#define ShutdownA OutputBit<Port<D>, 6>
#define ShutdownB OutputBit<Port<D>, 7>

class MyApp;

class MyErrorReporter : public ErrorReporter {
public:
    virtual void reportError(char c, uint16_t, ErrorConditionType);
};

class MyApp : public EventListener
{    
public:
    MyApp();
    
    // EventListener override
    virtual void handleEvent(EventType type, EventParam);
    
    StatusLED _LEDPort;
    ShutdownA _shutdownA;
    ShutdownB _shutdownB;
    DeviceStream<TextLCD<16, 2, LCD_DEFAULT, LCDRS, NullOutputBit, LCDEnable, LCDD0, LCDD1, LCDD2, LCDD3> > _lcd;
    TimerEventMgr<Timer0, TimerClockDIV64> _timerEventMgr;
    RepeatingTimerEvent<1000> _timerEvent;
    INA219 _current0, _current1;
    uint16_t _busMilliVolts0, _busMilliVolts1;
    uint16_t _shuntMicroAmps0, _shuntMicroAmps1;
    bool _captureCurrentValues;

    MyErrorReporter _errorReporter;
};

MyApp g_app;

MyApp::MyApp()
    : _current0(0x40)
    , _current1(0x41)
    , _captureCurrentValues(true)
{
    sei();
    _shutdownA = false;
    _shutdownA = false;
    System::startEventTimer(&_timerEvent);
    _current0.setCalibration(INA219::Range16V, 40, 330);
    _current1.setCalibration(INA219::Range16V, 40, 330);
}

void
MyApp::handleEvent(EventType type, EventParam param)
{
    switch(type)
    {
        case EV_IDLE:
        if (_captureCurrentValues) {
            _busMilliVolts0 = _current0.busMilliVolts();
            _busMilliVolts1 = _current1.busMilliVolts();
            _shuntMicroAmps0 = _current0.shuntMicroAmps();
            _shuntMicroAmps1 = _current1.shuntMicroAmps();
            _lcd.device().setCursor(0, 0);
            _lcd << FS("V0=") << _current0.busMilliVolts() << FS(" ") << FS("I0=") << _current0.shuntMicroAmps();
            _lcd.device().setCursor(0, 1);
            _lcd << FS("V1=") << _current1.busMilliVolts() << FS(" ") << FS("I1=") << _current1.shuntMicroAmps();
            _captureCurrentValues = false;
        }
        break;
        case EV_EVENT_TIMER:
            _captureCurrentValues = true;
            break;
        default:
            break;
    }
}

void
MyErrorReporter::reportError(char c, uint16_t code, ErrorConditionType type)
{
    switch(type) {
        case ErrorConditionNote: g_app._lcd << "Note:"; break;
        case ErrorConditionWarning: g_app._lcd << "Warn:"; break;
        case ErrorConditionFatal: g_app._lcd << "Fatl:"; break;
    }
    g_app._lcd << code;
    if (type == ErrorConditionFatal)
        while (1) ;
    
    System::msDelay<1000>();
}

