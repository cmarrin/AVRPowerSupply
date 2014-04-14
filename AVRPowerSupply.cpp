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
#include "Menu.h"
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
#define Switch0 DynamicInputBit<B, 0>
#define Switch1 DynamicInputBit<B, 1>
#define Switch2 DynamicInputBit<B, 2>

const uint8_t ADCAverageCount = 16;

class MyApp;

class MyErrorReporter : public ErrorReporter {
public:
    virtual void reportError(char c, uint32_t, ErrorConditionType);
};

const char bannerString[] PROGMEM = "AVR Power Supply\n      v0.1";
const char curLimit[] PROGMEM = "Cur Limit";
const char accept[] PROGMEM = "Save? (UP=YES)";
const char accepted[] PROGMEM = "Cur Limit Set";
const uint8_t curLimitValues[] PROGMEM = { 1, 5, 10, 20, 40, 60, 80, 100 };
const uint8_t numCurLimitValues = sizeof(curLimitValues);

class MyApp : public EventListener, public Menu<MyApp>
{    
public:
    friend class MyErrorReporter;
    MyApp();
    
    // EventListener override
    virtual void handleEvent(EventType type, EventParam);
    
    void updateADC();
    void updateDisplay();
    void showPSVoltageAndCurrent(uint8_t channel, uint8_t line);
    void showPSCurrents(uint8_t line);
    void showTestVoltages(uint8_t channel0, uint8_t channel1, uint8_t line);
    
    enum class CurrentLimitArrow { None, Supply, Current };
    void showCurrentLimit(uint8_t supply, CurrentLimitArrow);
    
    void updateCurrentSensor();

    // Menu
    virtual void show(const _FlashString& s)
    {
        _displayEnabled = false;
        _lcd << TextLCDClear() << s;
    }
    
    void setCurrentLimit(uint8_t supply)
    {
        if (supply == 0) {
            _shutdownA = true;
        } else {
            _shutdownB = true;
        }
        _statusLED = true;
    }
    
    void resetCurrentLimit()
    {
        _shutdownA = false;
        _shutdownB = false;
        _statusLED = false;
    }

    static void display(MyApp* app)
    {
        app->_displayEnabled = true;
    }
    static void nextLine0(MyApp* app) { app->advanceLineDisplay(0); }
    static void nextLine1(MyApp* app) { app->advanceLineDisplay(1); }
    static void curLimit0(MyApp* app) { app->showCurrentLimit(0, CurrentLimitArrow::Supply); app->_currentLimitAdjustSupply = 0; }
    static void curLimit1(MyApp* app) { app->showCurrentLimit(1, CurrentLimitArrow::Supply); app->_currentLimitAdjustSupply = 1; }
    static void adjustCurLimit(MyApp* app) { app->showCurrentLimit(app->_currentLimitAdjustSupply, CurrentLimitArrow::Current); }
    static void showCurLimit(MyApp* app) { app->showCurrentLimit(app->_currentLimitAdjustSupply, CurrentLimitArrow::None); }
    static void incCurLimit(MyApp* app)
    {
        if (++app->_currentLimitAdjustIndex[app->_currentLimitAdjustSupply] >= numCurLimitValues) {
            app->_currentLimitAdjustIndex[app->_currentLimitAdjustSupply] = 0;
        }
    }
    static void decCurLimit(MyApp* app)
    {
        if (app->_currentLimitAdjustIndex[app->_currentLimitAdjustSupply]-- == 0) {
            app->_currentLimitAdjustIndex[app->_currentLimitAdjustSupply] = numCurLimitValues - 1;
        }
    }
    static void acceptCurLimit(MyApp* app)
    {
        app->_currentLimitIndex[0] = app->_currentLimitAdjustIndex[0];
        app->_currentLimitIndex[1] = app->_currentLimitAdjustIndex[1];
    }
    static void rejectCurLimit(MyApp* app)
    {
        app->_currentLimitAdjustIndex[0] = app->_currentLimitIndex[0];
        app->_currentLimitAdjustIndex[1] = app->_currentLimitIndex[1];
    }

private:    
    void advanceLineDisplay(uint8_t line)
    {
        _lineDisplayMode[line] = (LineDisplayMode)((uint8_t)_lineDisplayMode[line] + 1);
        if (_lineDisplayMode[line] == LineDisplayMode::Last) {
            _lineDisplayMode[line] = LineDisplayMode::PS1VA;
        }
        _needsDisplay = true;
    }
    
    uint16_t curLimitAdjustMa(uint8_t supply) const { return (uint16_t) pgm_read_byte(&curLimitValues[_currentLimitAdjustIndex[supply]]) * 10; }
    uint16_t curLimitMa(uint8_t supply) const { return (uint16_t) pgm_read_byte(&curLimitValues[_currentLimitIndex[supply]]) * 10; }


    MyErrorReporter _errorReporter;

    StatusLED _statusLED;
    ShutdownA _shutdownA;
    ShutdownB _shutdownB;
    DeviceStream<TextLCD<16, 2, LCD_DEFAULT, LCDRS, NullOutputBit, LCDEnable, LCDD0, LCDD1, LCDD2, LCDD3> > _lcd;
    TimerEventMgr<Timer0, TimerClockDIV64> _timerEventMgr;
    RepeatingTimerEvent _timerEvent;
    
    INA219 _currentSensor[2];
    int16_t _busMilliVolts[2];
    int16_t _shuntMilliAmps[2];
    bool _captureSensorValues;
    
    bool _needsDisplay = true;
    bool _displayEnabled = false;

    ADC _adc;
    uint16_t _adcAccumulator[4];
    uint16_t _adcVoltage[4];
    uint8_t _adcCurrentChannel;
    uint8_t _adcCurrentSamples;
    bool _captureADCValue;
    
    uint8_t _currentLimitIndex[2];
    uint8_t _currentLimitAdjustIndex[2];
    uint8_t _currentLimitAdjustSupply = 0;
    uint8_t _overCurrentCount[2] = { 0, 0 };
    
    enum class LineDisplayMode { PS1VA, PS2VA, PS12A, V1V2, V3V4, Last };

    LineDisplayMode _lineDisplayMode[2];

    ButtonSet<Switch0, Switch1, Switch2> _buttons;
};

typedef Menu<MyApp> MyMenu;
const MenuOpType g_menuOps[] = {
    MyMenu::Show(bannerString), MyMenu::Pause(2000),
    
    MyMenu::State( 0), MyMenu::XEQ(MyApp::display), MyMenu::Buttons(), 1, 2, 3,         // Normal display
    MyMenu::State( 1), MyMenu::XEQ(MyApp::nextLine0), MyMenu::Goto(0),                  // Show next display for line 0
    MyMenu::State( 2), MyMenu::XEQ(MyApp::nextLine1), MyMenu::Goto(0),                  // Show next display for line 1
    MyMenu::State( 3), MyMenu::Show(curLimit), MyMenu::Goto(4),                         // Show cur limit
    MyMenu::State( 4), MyMenu::XEQ(MyApp::curLimit0), MyMenu::Buttons(), 5, 6, 0,       // Set cur limit adjust to supply A
    MyMenu::State( 5), MyMenu::XEQ(MyApp::curLimit1), MyMenu::Buttons(), 4, 6, 0,       // Set cur limit adjust to supply B
    MyMenu::State( 6), MyMenu::XEQ(MyApp::adjustCurLimit), MyMenu::Buttons(), 7, 8, 9,  // Start cur limit adjust
    MyMenu::State( 7), MyMenu::XEQ(MyApp::incCurLimit), MyMenu::Goto(6),                // inc cur limit
    MyMenu::State( 8), MyMenu::XEQ(MyApp::decCurLimit), MyMenu::Goto(6),                // dec cur limit
    MyMenu::State( 9), MyMenu::Show(accept), MyMenu::XEQ(MyApp::showCurLimit),          // Ask to accept new cur limit settings
                       MyMenu::Buttons(), 10, 11, 11,
    MyMenu::State(10), MyMenu::Show(accepted), MyMenu::XEQ(MyApp::acceptCurLimit),      // Accept new cur limit settings
                       MyMenu::Pause(2000), MyMenu::Goto(0),
    MyMenu::State(11), MyMenu::XEQ(MyApp::rejectCurLimit), MyMenu::Goto(0),             // Reject new cur limit settings
    MyMenu::End()
};

MyApp g_app;

MyApp::MyApp()
    : Menu(&_buttons, g_menuOps, this)
    , _timerEvent(100)
    , _captureSensorValues(true)
    , _adc(0, ADC_PS_DIV128, ADC_REF_AVCC)
    , _adcCurrentChannel(0)
    , _adcCurrentSamples(0)
    , _captureADCValue(false)
    , _currentLimitIndex{ numCurLimitValues - 1, numCurLimitValues - 1 }
    , _currentLimitAdjustIndex{ numCurLimitValues - 1, numCurLimitValues - 1 }
    , _lineDisplayMode{ LineDisplayMode::PS1VA, LineDisplayMode::PS2VA }
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
    _lcd << TextLCDSetLine(line) << static_cast<char>(channel + 'A') << ':';
    _lcd << FixedPoint8_8(_busMilliVolts[channel], 1000).toString(2) << FS("v ");
    _lcd << FixedPoint8_8(_shuntMilliAmps[channel], 10).toString(1) << FS("ma");
}

void MyApp::showPSCurrents(uint8_t line)
{
    _lcd << TextLCDSetLine(line);
    _lcd << FS("A:") << FixedPoint8_8(_shuntMilliAmps[0], 10).toString(1) << FS("ma");
    _lcd << FS(" B:") << FixedPoint8_8(_shuntMilliAmps[1], 10).toString(1) << FS("ma");
}

void MyApp::showTestVoltages(uint8_t channel0, uint8_t channel1, uint8_t line)
{
    _lcd << TextLCDSetLine(line);
    _lcd << static_cast<char>('a' + channel0) << ':' << FixedPoint8_8(_adcVoltage[channel0], 1000).toString(2) << FS("v ");
    _lcd << static_cast<char>('a' + channel1) << ':' << FixedPoint8_8(_adcVoltage[channel1], 1000).toString(2) << FS("v");
}

void MyApp::showCurrentLimit(uint8_t supply, CurrentLimitArrow arrow)
{
    resetCurrentLimit();
    _displayEnabled = false;
    _lcd << TextLCDClearLine(1)
         << ((arrow == CurrentLimitArrow::Supply) ? '\x7e' : ' ')
         << static_cast<char>('A' + supply) << ':' << curLimitAdjustMa(supply) << FS("ma")
         << ((arrow == CurrentLimitArrow::Current) ? '\x7f' : ' ');
}

void MyApp::updateDisplay()
{
    if (!_displayEnabled || !_needsDisplay) {
        return;
    }
    _lcd << TextLCDClear();
    _needsDisplay = false;
    
    for (uint8_t i = 0; i < 2; ++i) {
        switch(_lineDisplayMode[i]) {
            case LineDisplayMode::PS1VA: showPSVoltageAndCurrent(0, i); break;
            case LineDisplayMode::PS2VA: showPSVoltageAndCurrent(1, i); break;
            case LineDisplayMode::PS12A: showPSCurrents(i); break;
            case LineDisplayMode::V1V2: showTestVoltages(0, 1, i); break;
            case LineDisplayMode::V3V4: showTestVoltages(2, 3, i); break;
            default: break;
        }
    }
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
        if (_shuntMilliAmps[i] > (int16_t) curLimitMa(i) * 10) {
            if (_overCurrentCount[i]++ > 3) {
                setCurrentLimit(i);
            }
        } else {
            _overCurrentCount[i] = 0;
        }
    }
}

void MyApp::handleEvent(EventType type, EventParam param)
{
    Menu::handleEvent(type, param);
    
    switch(type) {
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
            break;
//            ButtonBase* button = reinterpret_cast<ButtonBase*>(param);
//            if (button == &_button0) {
//                _lineDisplayMode[0] = (LineDisplayMode)((uint8_t)_lineDisplayMode[0] + 1);
//                if (_lineDisplayMode[0] == LDMLast) {
//                    _lineDisplayMode[0] = LDMPS1VA;
//                }
//                _needsDisplay = true;
//            }
//            if (button == &_button1) {
//                _lineDisplayMode[1] = (LineDisplayMode)((uint8_t)_lineDisplayMode[1] + 1);
//                if (_lineDisplayMode[1] == LDMLast) {
//                    _lineDisplayMode[1] = LDMPS1VA;
//                }
//                _needsDisplay = true;
//            }
//            break;
//        }
        default:
            break;
    }
}

static char* toHex(char* buf, uint32_t u)
{
    buf += 10;
    *--buf = '\0';
    if (u == 0) {
        *--buf = '0';
        *--buf = '0';
    } else {
        while (u) {
            uint8_t c = u & 0xf;
            *--buf = (c > 9) ? (c + 'a' - 10) : (c + '0');
            u >>= 4;
            c = u & 0xf;
            *--buf = (c > 9) ? (c + 'a' - 10) : (c + '0');
            u >>= 4;
        }
    }
    *--buf = 'x';
    *--buf = '0';
    return buf;
}

void
MyErrorReporter::reportError(char c, uint32_t code, ErrorConditionType type)
{
    g_app._lcd << TextLCDClear();
    switch(type) {
        case ErrorConditionNote: g_app._lcd << "Note:"; break;
        case ErrorConditionWarning: g_app._lcd << "Warn:"; break;
        case ErrorConditionFatal: g_app._lcd << "Fatl:"; break;
    }
    char buf[12];
    char* p = toHex(buf, code);
    g_app._lcd << p;
    if (type == ErrorConditionFatal)
        while (1) ;
    
    System::msDelay<1000>();
}

