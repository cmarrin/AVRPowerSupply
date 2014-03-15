//
//  AVRPowerSupply.cpp
//
//  Created by Chris Marrin on 3/9/2014.
//

#include "m8r.h"
#include "System.h"
#include "EventListener.h"
#include "DeviceStream.h"
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

class MyApp : public EventListener {    
public:
    MyApp();
    
    // EventListener override
    virtual void handleEvent(EventType type, EventParam);
    
    StatusLED _LEDPort;
    DeviceStream<TextLCD<16, 2, LCD_DEFAULT, LCDRS, NullOutputBit, LCDEnable, LCDD0, LCDD1, LCDD2, LCDD3> > _lcd;
    TimerEventMgr<Timer0, TimerClockDIV64> m_timerEventMgr;
    RepeatingTimerEvent<1000> _timerEvent;
};

MyApp g_app;

MyApp::MyApp()
{
    sei();
    System::startEventTimer(&_timerEvent);
    _lcd << FS("Hello World");
}

void
MyApp::handleEvent(EventType type, EventParam param)
{
    switch(type)
    {
        case EV_IDLE:
        break;
        case EV_EVENT_TIMER:
            _LEDPort = !_LEDPort;
            break;
        default:
            break;
    }
}
