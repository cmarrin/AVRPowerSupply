//
//  AVRPowerSupply.cpp
//
//  Created by Chris Marrin on 3/9/2014.
//

#include "m8r.h"

#include "ADC.h"
#include "Button.h"
#include "DeviceStream.h"
#include "EventListener.h"
#include "FixedPoint.h"
#include "INA219.h"
#include "System.h"
#include "TextLCD.h"
#include "Timer0.h"
#include "TimerEventMgr.h"

//
//  AVR Based Power Supply
//
// This is for a power supply using a pair of MIC2941s. The regulators are manually
// controlled, one by a 10 turn potentiometer and the other can be switched between
// 3.3 and 5v. There is an INA129 current sensor connected to the output of each.
// The INA129's are controlled through I2C connected to Port C4 (SDA) and C5 (SCL).
// The current sensor for the variable supply is device 0 and the switchable supply
// is device 1. The shutdown pin of the MIC2941s are connected to pin D6 (variable)
// and D7 (switchable). The supply board also has 4 analog inputs connected to 
// the AVR analog pins 0-3 (C0 - C3). A HD44780 compatible 2x16 text LCD is also on
// the board. Finally there are 3 momentary on switches to ground.
//
// Connection:
//
// LCD:
//  RS      - Port B4
//  Enable  - Port B3
//  D0      - Port D5
//  D1      - Port D4
//  D2      - Port D3
//  D3      - Port D2
//
// Status LED       - port B5
// SDA              - Port C4
// SCL              - Port C5
// ShutdownA (var)  - Port D6
// ShutdownA (sw)   - Port D7
// Analog In        - Port C0 - C3
// Switches         - Port B0 - B2
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
#define Switch0 InputBit<Port<B>, 0>
#define Switch1 InputBit<Port<B>, 1>
#define Switch2 InputBit<Port<B>, 2>

const uint8_t ADCAverageCount = 16;

class MyApp;

class MyErrorReporter : public ErrorReporter {
public:
    virtual void reportError(char c, uint16_t, ErrorConditionType);
};

class MyApp : public EventListener
{    
public:
    friend class MyErrorReporter;
    MyApp();
    
    // EventListener override
    virtual void handleEvent(EventType type, EventParam);
    
    void updateADC();
    void updateDisplay();
    void showPSVoltageAndCurrent(uint8_t channel, uint8_t line);
    void showTestVoltages(uint8_t channel0, uint8_t channel1, uint8_t line);
    
    void updateCurrentSensor();

private:    
    StatusLED _statusLED;
    ShutdownA _shutdownA;
    ShutdownB _shutdownB;
    DeviceStream<TextLCD<16, 2, LCD_DEFAULT, LCDRS, NullOutputBit, LCDEnable, LCDD0, LCDD1, LCDD2, LCDD3> > _lcd;
    TimerEventMgr<Timer0, TimerClockDIV64> _timerEventMgr;
    RepeatingTimerEvent<100> _timerEvent;
    Button<Switch0> _button0;
    Button<Switch1> _button1;
    Button<Switch2> _button2;
    
    INA219 _currentSensor[2];
    int16_t _busMilliVolts[2];
    int16_t _shuntMilliAmps[2];
    bool _captureSensorValues;
    
    bool _needsDisplay;

    ADC _adc;
    uint16_t _adcAccumulator[4];
    uint16_t _adcVoltage[4];
    uint8_t _adcCurrentChannel;
    uint8_t _adcCurrentSamples;
    bool _captureADCValue;

    MyErrorReporter _errorReporter;
};

MyApp g_app;

MyApp::MyApp()
    : _captureSensorValues(true)
    , _adc(0, ADC_PS_DIV128, ADC_REF_AVCC)
    , _adcCurrentChannel(0)
    , _adcCurrentSamples(0)
    , _captureADCValue(false)
{
    _currentSensor[0].setAddress(0x40);
    _currentSensor[1].setAddress(0x41);
    _adc.setEnabled(true);

    sei();
    _shutdownA = false;
    _shutdownB = false;
    System::startEventTimer(&_timerEvent);
    _currentSensor[0].setConfiguration(INA219::Range16V);
    _currentSensor[1].setConfiguration(INA219::Range16V);

    _adc.startConversion();
}

void MyApp::showPSVoltageAndCurrent(uint8_t channel, uint8_t line)
{
    char buf[9];
    _lcd << TextLCDSetLine(line) << static_cast<char>(channel + 'A') << ':'
         << FixedPoint8_8(_busMilliVolts[channel], 1000).toString(buf, 2) << FS("v ")
         << FixedPoint8_8(_shuntMilliAmps[channel], 10).toString(buf, 1) << FS("ma");
}

void MyApp::showTestVoltages(uint8_t channel0, uint8_t channel1, uint8_t line)
{
    char buf[9];
    _lcd << TextLCDSetLine(line)
         << static_cast<char>('a' + channel0) << ':' << FixedPoint8_8(_adcVoltage[channel0], 1000).toString(buf, 2) << FS("v ")
         << static_cast<char>('a' + channel1) << ':' << FixedPoint8_8(_adcVoltage[channel1], 1000).toString(buf, 2) << FS("v");
}

void MyApp::updateDisplay()
{
    if (!_needsDisplay) {
        return;
    }
    _lcd << TextLCDClear();
    _needsDisplay = false;
    showPSVoltageAndCurrent(0, 0);
    showPSVoltageAndCurrent(1, 1);
//    showTestVoltages(0, 1, 0);
//    showTestVoltages(2, 3, 1);
}

void MyApp::updateADC()
{
    _adcAccumulator[_adcCurrentChannel++] += _adc.lastConversion10Bit();
    if (_adcCurrentChannel >= 4) {
        _adcCurrentChannel = 0;
        if (++_adcCurrentSamples >= ADCAverageCount) {
            _adcCurrentSamples = 0;
            for (uint8_t i = 0; i < 4; ++i) {
                uint32_t v = (_adcAccumulator[i] + ADCAverageCount / 2) / ADCAverageCount;
                v *= 5000;
                v /= 1024;
                _adcVoltage[i] = v;
                _adcAccumulator[i] = 0;
            }
        }
    }
}

void MyApp::updateCurrentSensor()
{
    for (uint8_t i = 0; i < 2; ++i) {
        int16_t value = _currentSensor[i].busMilliVolts();
        if (value != _busMilliVolts[i]) {
            _busMilliVolts[i] = value;
            _needsDisplay = true;
        }
        int32_t v = _currentSensor[i].shuntMilliVolts();
        if (v < 0) {
            v = 0;
        }
        v = (v * 100 + 165) / 330;
        if (v != _shuntMilliAmps[i]) {
            _shuntMilliAmps[i] = v;
            _needsDisplay = true;
        }
    }
}

void MyApp::handleEvent(EventType type, EventParam param)
{
    switch(type)
    {
        case EV_IDLE:
        if (_captureSensorValues) {
            _captureSensorValues = false;
            updateCurrentSensor();
        }
        if (_captureADCValue) {
            _captureADCValue = false;
            updateADC();
            _adc.setChannel(_adcCurrentChannel);
            _adc.startConversion();
        }
        updateDisplay();
        break;
        case EV_ADC:
            _captureADCValue = true;
            break;
        case EV_EVENT_TIMER:
            if (param == &_timerEvent) {
                _captureSensorValues = true;
            }
            break;
        case EV_BUTTON_DOWN:
        case EV_BUTTON_UP: {
            bool down = type == EV_BUTTON_DOWN;
            ButtonBase* button = reinterpret_cast<ButtonBase*>(param);
            if (button == &_button0) {
                _statusLED = down;
            }
            break;
        }
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

