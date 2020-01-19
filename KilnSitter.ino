#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <Adafruit_I2CDevice.h>
#include <Adafruit_I2CRegister.h>
#include "Adafruit_MCP9600.h"
#include "KilnSitterRecord.h"

#define MAX_PROGS 5
#define MAX_PROGRAM_STAGES 10
#define KILN_CHECK_INTERVAL 300
#define KILN_ON (digitalWrite(SSRPIN, HIGH))
#define KILN_OFF (digitalWrite(SSRPIN, LOW))
#define KILN_TEMP ((int)mcp.readThermocouple())
#define KILN_TEMP_LIMIT 510
#define STAGE_ELAPSED_TIME (int)((millis() - tsStageStart) / 60000)
#define HEAT ('H')
#define SOAK ('S')
#define COOL ('C')
#define MANUAL ('M')
#define OFF ('O')
#define DEBOUNCE_DELAY 30

// Thermocouple I2C address
#define TC_I2C_ADDRESS (0x67)

// GPIO pin Definitions
// output
#define SSRPIN A0 // solid state relay 
//input
#define BUTOK A1 // switch 1
#define BUTUP A2 // switch 2
#define BUTDN A3 // switch 3
//#define SDDPIN 4  // SD Card detect (true = detected) -- not used

// SPI
#define CSPIN 10
#define MOSIPIN 11
#define MOSOPIN 12
#define SCKPIN 13

// Initialise LCD object
LiquidCrystal_I2C lcd(0x27, 20, 4); // set the LCD address to 0x27 for a 16 chars and 2 line display

// Initialise thermocouple sensor
Adafruit_MCP9600 mcp;

// Kiln Program
char* ksPrograms[] = {"KSPROG01.txt",
                      "KSPROG02.txt",
                      "KSPROG03.txt",
                      "KSPROG04.txt",
                      "KSPROG05.txt"
                     };
uint8_t ksCurrentProgram = 0;
KilnSitterRecord ksProgram[MAX_PROGRAM_STAGES];
uint8_t programStages = 0;
uint8_t currentStage = 0;
long lastTimerTimestamp = 0;
long tsStageStart = 0;
int tStageStartTemp = 0;

void setup() {

  //Serial.begin(9600);
  //while(!Serial){};
  
  initGpio();

  initLcd();

  if (!initProgram()) {
    exception("Prog Load");
  }

  if (!validProgram()) {
    exception("Prog Invalid");
  }

  if (!initThermo()) {
    exception("Thermo");
  }

  lcd.setCursor(5, 0);
  lcd.print("SET PROGRAM");

  showProgram();

  // select program
  int8_t butPress = 0;
  while(butPress != BUTOK){
    butPress = scanButtons();
    Serial.println(butPress);
    switch(butPress) {
      case BUTUP: if(ksCurrentProgram > 0){
                    ksCurrentProgram--;
                    showProgram();
                  }
                  break;
      case BUTDN: if(ksCurrentProgram < (MAX_PROGS-1)){
                    ksCurrentProgram++;
                    showProgram(); 
                  }
                  break;
      default: break;
    }  
  }

  // start program
  lcd.setCursor(2, 0);
  lcd.print("START ** PROGRAM");
  while(scanButtons() != BUTOK){ 
    KILN_OFF;
  };

  lcd.setCursor(0, 0);
  lcd.print("     *RUNNING!*     ");

  // Start timers
  lastTimerTimestamp = millis();
  tsStageStart = millis();
  tStageStartTemp = KILN_TEMP;
}

void loop() {
  // respond to buttons if in manual pinMode
  if(ksProgram[currentStage].type == MANUAL) {
    uint8_t button = scanButtons();
    if(button == BUTUP){
      if(ksProgram[currentStage].targetTemp < KILN_TEMP_LIMIT) {
        ksProgram[currentStage].targetTemp += 10;
      }
    }
    else if(button == BUTDN) {
      if(ksProgram[currentStage].targetTemp > 10){
        ksProgram[currentStage].targetTemp -= 10;
      }
    }
  }

  if(millis() > (KILN_CHECK_INTERVAL + lastTimerTimestamp)){
    lastTimerTimestamp = millis();
    
    // switch kiln on/off
    switchKiln();

    // advance to next program stage if criteria met
    checkStage();
  
    // Display current status
    displayStatus();
  }
}

void initGpio(void) {
  // gpio pin setup
  // output
  pinMode(SSRPIN, OUTPUT);
  digitalWrite(SSRPIN, LOW);
  // input
  pinMode(BUTUP, INPUT);
  pinMode(BUTDN, INPUT);
  pinMode(BUTOK, INPUT);
  //pinMode(SDDPIN, INPUT);
}

void initLcd(void) {
  // setup lcd
  lcd.init();
  lcd.backlight();
  lcd.clear();
}

bool initSd(void) {
  return SD.begin(CSPIN);
}

bool initThermo(void) {
  // setup mcp9600 thermocouple interface
  // Initialise the driver with I2C_ADDRESS and the default I2C bus.
  if (mcp.begin(TC_I2C_ADDRESS)) {
    mcp.setADCresolution(MCP9600_ADCRESOLUTION_18);
    mcp.setThermocoupleType(MCP9600_TYPE_S);
    mcp.setFilterCoefficient(3);
    mcp.enable(true);
    return true;
  }
  else {
    return false;
  }
}

bool initProgram(void) {
  lcd.setCursor(1, 1);
  if (initSd()) {
    return readProgram();
  }
  else {
    return false;
  }
}

bool readProgram(void) {
  char Buffer[KS_RECORD_STRING_BUFFER_SIZE];
  int  progStageIndex = 0;
  int  bufferposition = 0;
  File programFile = SD.open(ksPrograms[ksCurrentProgram], FILE_READ);

  if (!programFile) {
    return false;
  }

  while (programFile.available() > 0) {
    char character = programFile.read();
    if (bufferposition < KS_RECORD_STRING_BUFFER_SIZE) {
      Buffer[bufferposition++] = character;
      if ((character == '\n' || character == '\r')) {
        //terminate the buffer
        Buffer[bufferposition - 1] = 0;
        // do not process empty or partial lines
        if (Buffer[0] != 0) {
          ksProgram[progStageIndex].PopulateRecord(Buffer);
          bufferposition = 0;
          if (++progStageIndex > (MAX_PROGRAM_STAGES - 1)) {
            break;
          }
        }
      }
    }
    else {
      return false;
    }
  }
  // close the file:
  programFile.close();
  programStages = progStageIndex - 1;
  currentStage = 0;
  return true;;
}

bool validProgram(void) {
  return (ksProgram[programStages].type == OFF);
}

int8_t scanButtons(void){
  int8_t button = -1;
  for (button = 15; button < 18; button++){
      if(digitalRead(button) == HIGH) {
        do {
          delay(DEBOUNCE_DELAY);
        } while (digitalRead(button) == HIGH); 
        break;
      }
  }
  return button;
}

void showProgram(void){
  // Show selected program
  lcd.setCursor(4, 1);
  lcd.print(ksPrograms[ksCurrentProgram]);
}

void switchKiln(void) {
  if (KILN_TEMP < getTempForTime()) { // Kiln too cool for this time in stage (undershooting slope line)
    KILN_ON;
  }
  else {
    KILN_OFF; // Kiln at or too hot for this time in stage (overshooting slope line)
  }
}

int getTempForTime(void) {
  switch (ksProgram[currentStage].type) {
    case HEAT: return ksProgram[currentStage].slope * STAGE_ELAPSED_TIME;
      break;
    case MANUAL:
    case SOAK: return ksProgram[currentStage].targetTemp;
      break;
    case COOL: return tStageStartTemp - (ksProgram[currentStage].slope * STAGE_ELAPSED_TIME);
      break;
    default: return 0;
  }
}

void checkStage(void) {
  switch (ksProgram[currentStage].type) {
    case OFF: KILN_OFF; // Cooling stage
      break;
    case HEAT: if (KILN_TEMP >= ksProgram[currentStage].targetTemp) {
        advanceStage();
      }
    case SOAK: if (KILN_TEMP >= ksProgram[currentStage].targetTemp && STAGE_ELAPSED_TIME > ksProgram[currentStage].duration) {
        advanceStage();
      }
      break;
    case COOL: if (KILN_TEMP <= ksProgram[currentStage].targetTemp) {
        advanceStage();
      }
      break;
    case MANUAL: break;
    default: KILN_OFF;
  }
}

void advanceStage(void) {
  currentStage++;
  tsStageStart = millis();
  tStageStartTemp = KILN_TEMP;
}

void displayStatus(void) {
  //line 1
  lcd.setCursor(0, 0);
  lcd.print(">");
  lcd.print(String(ksProgram[currentStage].type));
  lcd.setCursor(17, 0);
  lcd.print(type2Symbol(ksProgram[currentStage].type));
  lcd.print(formatValue(ksProgram[currentStage].slope, -1));
  // line 3
  lcd.setCursor(0, 2);
  lcd.print("S:");
  lcd.print(formatValue(currentStage + 1, -1));
  lcd.print("/");
  lcd.print(formatValue(programStages + 1, -1));
  lcd.setCursor(7, 2);
  lcd.print("D:");
  lcd.print(formatValue(ksProgram[currentStage].duration, 3));
  lcd.setCursor(13, 2);
  lcd.print("Tt:");
  lcd.print(formatValue(ksProgram[currentStage].targetTemp, 3));
  // line 4
  lcd.setCursor(0, 3);
  lcd.print("E:");
  lcd.print(formatValue(STAGE_ELAPSED_TIME, 3));
  lcd.setCursor(6, 3);
  lcd.print("Ts:");
  lcd.print(formatValue(getTempForTime(), 3));
  lcd.setCursor(13, 3);
  lcd.print("Ta:");
  lcd.print(formatValue(KILN_TEMP, 3));
}

char * type2Symbol(char type) {
  switch (type) {
    case HEAT: return "+";
      break;
    case SOAK: return "=";
      break;
    case COOL: return "-";
      break;
    case OFF: return "*";
      break;
    default: return "?";
      break;
  }
}

char * formatValue(int value, int sigFig) {
  static char data[4];
  if (sigFig == -1) {
    sprintf(data, "%d", value);
  }
  else {
    sprintf(data, "%3d", value);
  }
  return data;
}

void exception(char* message) {
  KILN_OFF;
  lcd.setCursor(0, 3);
  lcd.print("ERROR:");
  lcd.setCursor(7, 3);
  lcd.print(message);
  while (true) {
    delay(300);
  }
}


