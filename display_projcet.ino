#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "image2.h" 

// ===== TFT PIN DEFINITIONS =====
#define TFT_CS   4
#define TFT_RST  3
#define TFT_DC   2
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);


#define BACKLIGHT_PIN A1 

// ===== BUTTONS =====
#define BUTTON_UP     5
#define BUTTON_DOWN   6
#define BUTTON_SELECT 7
#define BUTTON_BACK   8

// ===== SD CARD PIN DEFINITIONS =====
#define SD_CS   10
#define ADC_PIN A0

// ===== AUDIO RECORDING VARIABLES =====
#define SAMPLE_RATE 8000
#define RECORDING_DURATION 30000  // 30 seconds

File audioFile;
bool isRecording = false;
uint32_t recordingStartTime = 0;
uint32_t samplesWritten = 0;
uint32_t lastSampleTime = 0;
const uint32_t SAMPLE_INTERVAL = 1000000 / SAMPLE_RATE; // 125 microseconds

// ===== HEART RATE SENSOR =====
MAX30105 particleSensor;
#define REPORTING_PERIOD_MS 250

float beatsPerMinute = 0;
float beatAvg = 70;
float beatSum = 0;
int beatCount = 0;
uint32_t lastBeat = 0;
bool hasStarted = false;
long filteredIR = 0;
uint32_t lastReport = 0;

// ===== MENU VARIABLES =====
int menuLevel = 0;
int cursor = 0;
int vertical_distance = 25;

// Plate input variables
char plateInput[7];
int plateIndex = 0;
const char plateChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
int plateCharIndex = 0;

// ===== WATCH MODE =====
int watchMode = 0;
unsigned long backPressStart = 0;
bool backPressedPrev = false;

// ===== CASE MANAGEMENT =====
bool caseActive = false;
char currentPlate[7] = "";
unsigned long caseStartTime = 0;
bool caseNoPlate = false;

// ===== PASSWORD VARIABLES =====
const int PASSWORD_LENGTH = 4;
int password[PASSWORD_LENGTH] = {1, 2, 3, 4};
int enteredPassword[PASSWORD_LENGTH];
int passwordIndex = 0;
bool passwordCorrect = false;
int failedAttempts = 0;
const int MAX_ATTEMPTS = 3;
bool systemLocked = false;

// ===== HEART RATE VARIABLES =====
int heartRate = 72;
unsigned long lastHeartUpdate = 0;
const unsigned long HEART_UPDATE_INTERVAL = 1000;

// ===== SERIAL COMMUNICATION =====
bool emergencySent = false;

// ===== MENU TEXT DEFINITIONS =====
const char* mainMenu[] = {"Start Case", "Close Case", "Emergency", "Settings"};
int mainMenuLength = 4;

const char* startCaseMenu[] = {"With Plate", "No Plate"};
int startCaseLength = 2;

const char* settingsMenu[] = {"Brightness", "Sound", "About"};
int settingsLength = 3;

// ===== COLORS =====
#define BACKGROUND_COLOR ST77XX_BLACK
#define TEXT_COLOR ST77XX_WHITE
#define HIGHLIGHT_COLOR ST77XX_YELLOW
#define HEADER_COLOR ST77XX_CYAN
#define ACCENT_COLOR ST77XX_BLUE

// ===== HELPER FUNCTIONS =====
bool buttonPressed(int pin) { return digitalRead(pin) == LOW; }

void drawCenteredText(const char* text, int y, int textSize, uint16_t color) {
  tft.setTextSize(textSize);
  tft.setTextColor(color);
  
  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  
  int x = (tft.width() - w) / 2;
  tft.setCursor(x, y);
  tft.print(text);
}

// ===== FIXED AUDIO RECORDING FUNCTIONS =====
void writeWavHeader(File &file, uint32_t sampleRate) {
  uint32_t dataSize = 0;
  uint32_t chunkSize = 36 + dataSize;

  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t *)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);

  file.write((const uint8_t*)"fmt ", 4);
  uint32_t subChunk1Size = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  uint16_t bitsPerSample = 16;
  uint16_t blockAlign = numChannels * bitsPerSample / 8;
  uint32_t byteRate = sampleRate * blockAlign;

  file.write((uint8_t *)&subChunk1Size, 4);
  file.write((uint8_t *)&audioFormat, 2);
  file.write((uint8_t *)&numChannels, 2);
  file.write((uint8_t *)&sampleRate, 4);
  file.write((uint8_t *)&byteRate, 4);
  file.write((uint8_t *)&blockAlign, 2);
  file.write((uint8_t *)&bitsPerSample, 2);

  file.write((const uint8_t*)"data", 4);
  file.write((uint8_t *)&dataSize, 4);
}

void patchWavHeader(File &file) {
  if (!file || samplesWritten == 0) return;
  
  uint32_t dataSize = samplesWritten * 2;
  uint32_t chunkSize = 36 + dataSize;

  file.seek(4);
  file.write((uint8_t *)&chunkSize, 4);

  file.seek(40);
  file.write((uint8_t *)&dataSize, 4);
}

String getNextAudioFilename() {
  int idx = 1;
  String name;
  while (true) {
    name = "/audio_" + String(idx) + ".wav";
    if (!SD.exists(name)) return name;
    idx++;
    if (idx > 999) break;
  }
  return "/audio_999.wav";
}

void startAudioRecording() {
  if (isRecording) {
    Serial.println("⚠️ Already recording!");
    return;
  }
  
  String filename = getNextAudioFilename();
  Serial.print("🎙️ START RECORDING: ");
  Serial.println(filename);

  if (audioFile) {
    audioFile.close();
  }

  audioFile = SD.open(filename, FILE_WRITE);
  if (!audioFile) {
    Serial.println("❌ Failed to create audio file!");
    return;
  }

  writeWavHeader(audioFile, SAMPLE_RATE);
  audioFile.flush();

  samplesWritten = 0;
  recordingStartTime = millis();
  lastSampleTime = micros();
  isRecording = true;
  
  Serial.println("✅ Recording started - 30 seconds");
}

void stopAudioRecording() {
  if (!isRecording) return;
  
  Serial.println("🛑 Stopping recording...");
  isRecording = false;

  if (audioFile) {
    patchWavHeader(audioFile);
    audioFile.flush();
    audioFile.close();
    Serial.print("💾 Saved ");
    Serial.print(samplesWritten);
    Serial.print(" samples (");
    Serial.print((float)samplesWritten / SAMPLE_RATE, 1);
    Serial.println(" seconds)");
  }
  Serial.println("■ RECORDING STOPPED & SAVED");
}

// SIMPLIFIED AUDIO RECORDING - CALL THIS AS OFTEN AS POSSIBLE
bool recordAudioSample() {
  if (!isRecording) return false;

  // Check if recording time is up
  if (millis() - recordingStartTime >= RECORDING_DURATION) {
    stopAudioRecording();
    return false;
  }

  // Take audio sample if it's time
  uint32_t currentMicros = micros();
  if (currentMicros - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime += SAMPLE_INTERVAL;
    
    uint16_t raw = analogRead(ADC_PIN);
    int16_t sample = (int16_t)(raw - 2048) * 16;
    
    if (audioFile.write((uint8_t*)&sample, 2) == 2) {
      samplesWritten++;
      
      // Flush every 512 samples to prevent data loss
      if (samplesWritten % 512 == 0) {
        audioFile.flush();
      }
      
      return true;
    } else {
      Serial.println("❌ Write failed - stopping recording");
      stopAudioRecording();
      return false;
    }
  }
  
  return false;
}

// ===== HEART RATE SENSOR FUNCTIONS =====
void setupHeartSensor() {
  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("MAX30102 not found!");
    while (1);
  }

  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x3F);
  particleSensor.setPulseAmplitudeIR(0x3F);
  particleSensor.setPulseWidth(411);
  particleSensor.setSampleRate(200);
  particleSensor.setADCRange(16384);
  particleSensor.setFIFOAverage(4);

  Serial.println("Heart rate sensor initialized");
}

void readHeartSensor() {
  long irValue = particleSensor.getIR();
  filteredIR = (filteredIR * 0.85) + (irValue * 0.15);

  if (filteredIR < 50000) {
    heartRate = 70;
    return;
  }

  if (checkForBeat(filteredIR)) {
    uint32_t now = millis();
    uint32_t delta = now - lastBeat;

    if (delta > 300) {
      lastBeat = now;
      beatsPerMinute = 60.0 / (delta / 1000.0);

      if (beatsPerMinute >= 50 && beatsPerMinute <= 180) {
        beatSum += beatsPerMinute;
        beatCount++;
        beatAvg = beatSum / beatCount;
        hasStarted = true;
        heartRate = (hasStarted ? beatAvg : 70) - 10;
        if (heartRate < 50) heartRate = 50;
      }
    }
  }

  if (millis() - lastReport > REPORTING_PERIOD_MS) {
    lastReport = millis();
  }
}

// ===== CASE MANAGEMENT FUNCTIONS =====
void startCaseWithPlate(const char* plate) {
  caseActive = true;
  strncpy(currentPlate, plate, sizeof(currentPlate)-1);
  currentPlate[sizeof(currentPlate)-1] = '\0';
  caseNoPlate = false;
  caseStartTime = millis();
  sendCaseStartWithPlate(plate);
}

void startCaseNoPlate() {
  caseActive = true;
  strcpy(currentPlate, "NONE");
  caseNoPlate = true;
  caseStartTime = millis();
  sendCaseStartNoPlate();
}

void closeCase() {
  if(caseActive) {
    if(caseNoPlate) {
      sendCaseCloseNoPlate();
    } else {
      sendCaseCloseWithPlate(currentPlate);
    }
    caseActive = false;
    strcpy(currentPlate, "");
    caseNoPlate = false;
  }
}

bool canStartCase() {
  return !caseActive;
}

// ===== PASSWORD FUNCTIONS =====
void resetPassword() {
  passwordIndex = 0;
  for(int i = 0; i < PASSWORD_LENGTH; i++) {
    enteredPassword[i] = 0;
  }
}

bool checkPassword() {
  for(int i = 0; i < PASSWORD_LENGTH; i++) {
    if(enteredPassword[i] != password[i]) {
      return false;
    }
  }
  return true;
}

int getButtonNumber(bool up, bool down, bool select, bool back) {
  if(up) return 1;
  if(down) return 2;
  if(select) return 3;
  if(back) return 4;
  return 0;
}

// ===== DRAW FUNCTIONS =====
void drawPasswordScreen() {
  tft.fillScreen(BACKGROUND_COLOR);
  tft.fillRect(0, 0, tft.width(), 40, ACCENT_COLOR);
  drawCenteredText("PASSWORD", 10, 2, TEXT_COLOR);
  
  tft.setTextSize(2);
  tft.setTextColor(TEXT_COLOR);
  drawCenteredText("Enter 4-digit code", 50, 1, TEXT_COLOR);
  
  tft.setTextSize(3);
  tft.setTextColor(HIGHLIGHT_COLOR);
  String displayStr = "";
  for(int i = 0; i < PASSWORD_LENGTH; i++) {
    if(i < passwordIndex) {
      displayStr += "* ";
    } else {
      displayStr += "_ ";
    }
  }
  drawCenteredText(displayStr.c_str(), 70, 3, HIGHLIGHT_COLOR);
  
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  drawCenteredText("UP=1 DOWN=2 SELECT=3 BACK=4", 110, 1, ST77XX_GREEN);
  drawCenteredText("Long press BACK to    cancel", 140, 1, ST77XX_GREEN);
  
  tft.setTextColor(ST77XX_CYAN);
  String progress = String(passwordIndex) + "/4 - Attempt: " + String(failedAttempts + 1) + "/3";
  drawCenteredText(progress.c_str(), 130, 1, ST77XX_CYAN);
}

void drawLockedScreen() {
  tft.fillScreen(ST77XX_RED);
  tft.setTextColor(ST77XX_WHITE);
  drawCenteredText("SYSTEM", 20, 2, ST77XX_WHITE);
  drawCenteredText("LOCKED", 40, 2, ST77XX_WHITE);
  drawCenteredText("Too many failed", 70, 1, ST77XX_WHITE);
  drawCenteredText("password attempts", 80, 1, ST77XX_WHITE);
  drawCenteredText("Contact Administrator", 100, 1, ST77XX_YELLOW);
  drawCenteredText("Alert sent to HQ", 115, 1, ST77XX_YELLOW);
}

// ===== SERIAL COMMUNICATION FUNCTIONS =====
void sendEmergencySignal() {
  Serial.println("EMERGENCY: Officer needs immediate assistance!");
  emergencySent = true;
  startAudioRecording();
}

void sendCaseStartNoPlate() {
  Serial.println("CASE_START: No plate - Case initiated without vehicle identification");
}

void sendCaseStartWithPlate(const char* plate) {
  Serial.print("CASE_START: Plate ");
  Serial.print(plate);
  Serial.println(" - Case initiated with vehicle identification");
}

void sendCaseCloseNoPlate() {
  Serial.println("CASE_CLOSE: No plate - Case completed and closed");
}

void sendCaseCloseWithPlate(const char* plate) {
  Serial.print("CASE_CLOSE: Plate ");
  Serial.print(plate);
  Serial.println(" - Case completed and closed");
}

void sendSecurityAlert() {
  Serial.println("SECURITY_ALERT: User entered incorrect password 3 times - System locked");
}

void drawbackground(){
  tft.drawRGBBitmap(0, 0, epd_bitmap_image6, 128, 160);
}

void drawNormalMode() {
  tft.fillScreen(ST77XX_BLACK);
  drawbackground(); 

  int batX = tft.width() - 30, batY = 8;
  tft.drawRect(batX, batY, 20, 10, ST77XX_WHITE);
  tft.fillRect(batX+1, batY+1, 15, 8, ST77XX_GREEN);
  tft.fillRect(batX+20, batY+2, 2, 6, ST77XX_WHITE);

  drawCenteredText("12:45", 45, 4, ST77XX_WHITE);
  drawCenteredText("Mon, 26 Nov", 85, 1, ST77XX_BLACK);

  int hrX = 10, hrY = 110;
  tft.fillTriangle(hrX+6, hrY+9, hrX+22, hrY+9, hrX+14, hrY+26, ST77XX_RED);
  tft.fillCircle(hrX+10, hrY+6, 6, ST77XX_RED);
  tft.fillCircle(hrX+18, hrY+6, 6, ST77XX_RED);
  
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(hrX + 32, hrY + 5);
  tft.print(heartRate);
  tft.print(" bpm");
}

void drawMainMenu() {
  tft.fillScreen(BACKGROUND_COLOR);
  tft.fillRect(0, 0, tft.width(), 25, ACCENT_COLOR);
  drawCenteredText("TACTICAL", 5, 2, TEXT_COLOR);

  for (int i = 0; i < mainMenuLength; i++) {
    int yPos = 35 + i * vertical_distance;
    if (i == cursor) {
      tft.fillRect(5, yPos-3, tft.width()-10, 22, HIGHLIGHT_COLOR);
      drawCenteredText(mainMenu[i], yPos, 2, BACKGROUND_COLOR);
    } else {
      drawCenteredText(mainMenu[i], yPos, 2, TEXT_COLOR);
    }
  }
  
  tft.setTextSize(1);
  if(caseActive) {
    tft.setTextColor(ST77XX_YELLOW);
    if(caseNoPlate) {
      drawCenteredText("CASE ACTIVE: No Plate", 150, 1, ST77XX_YELLOW);
    } else {
      String caseStatus = "CASE ACTIVE: " + String(currentPlate);
      drawCenteredText(caseStatus.c_str(), 150, 1, ST77XX_YELLOW);
    }
  } else {
    tft.setTextColor(ST77XX_GREEN);
    drawCenteredText("NO ACTIVE CASE", 150, 1, ST77XX_GREEN);
  }
}

void drawStartCaseMenu() {
  tft.fillScreen(BACKGROUND_COLOR);
  tft.fillRect(0, 0, tft.width(), 25, ACCENT_COLOR);
  drawCenteredText("START CASE", 5, 2, BACKGROUND_COLOR);

  for (int i = 0; i < startCaseLength; i++) {
    int yPos = 35 + i * vertical_distance;
    if (i == cursor) {
      tft.fillRect(5, yPos-3, tft.width()-10, 22, HIGHLIGHT_COLOR);
      drawCenteredText(startCaseMenu[i], yPos, 2, BACKGROUND_COLOR);
    } else {
      drawCenteredText(startCaseMenu[i], yPos, 2, TEXT_COLOR);
    }
  }
}

void drawSettingsMenu() {
  tft.fillScreen(BACKGROUND_COLOR);
  tft.fillRect(0, 0, tft.width(), 25, ACCENT_COLOR);
  drawCenteredText("SETTINGS", 5, 2, BACKGROUND_COLOR);

  for (int i = 0; i < settingsLength; i++) {
    int yPos = 35 + i * vertical_distance;
    if (i == cursor) {
      tft.fillRect(5, yPos-3, tft.width()-10, 22, HIGHLIGHT_COLOR);
      drawCenteredText(settingsMenu[i], yPos, 2, BACKGROUND_COLOR);
    } else {
      drawCenteredText(settingsMenu[i], yPos, 2, TEXT_COLOR);
    }
  }
}

void drawPlateInput() {
  tft.fillScreen(BACKGROUND_COLOR);
  tft.fillRect(0, 0, tft.width(), 25, ACCENT_COLOR);
  drawCenteredText("PLATE", 5, 2, TEXT_COLOR);

  tft.setTextSize(2);
  tft.setTextColor(TEXT_COLOR);
  drawCenteredText("Select Character", 35, 1, TEXT_COLOR);
  
  tft.fillRect(tft.width()/2 - 15, 50, 30, 25, ST77XX_BLUE);
  tft.setTextColor(HIGHLIGHT_COLOR);
  drawCenteredText(String(plateChars[plateCharIndex]).c_str(), 55, 2, HIGHLIGHT_COLOR);

  tft.setTextColor(TEXT_COLOR);
  drawCenteredText("Plate Number:", 85, 1, TEXT_COLOR);
  
  tft.drawRect(20, 100, tft.width()-40, 25, TEXT_COLOR);
  tft.fillRect(21, 101, tft.width()-42, 23, BACKGROUND_COLOR);
  
  if (strlen(plateInput) > 0) {
    drawCenteredText(plateInput, 105, 2, HIGHLIGHT_COLOR);
  } else {
    drawCenteredText("[Empty]", 105, 1, ST77XX_WHITE);
  }

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  drawCenteredText("UP/DOWN: Change char", 130, 1, ST77XX_GREEN);
  drawCenteredText("SELECT: Add char", 140, 1, ST77XX_GREEN);
  
  if (plateIndex == 6) {
    tft.setTextColor(ST77XX_YELLOW);
    drawCenteredText("BACK: Confirm Plate", 150, 1, ST77XX_YELLOW);
  } else {
    tft.setTextColor(ST77XX_GREEN);
    drawCenteredText("BACK: Cancel/Return", 150, 1, ST77XX_GREEN);
  }
}

void drawEmergencyScreen() {
  tft.fillScreen(ST77XX_RED);
  tft.setTextColor(TEXT_COLOR);
  
  int centerX = tft.width() / 2;
  tft.fillTriangle(centerX, 20, centerX-20, 60, centerX+20, 60, TEXT_COLOR);
  tft.fillRect(centerX-3, 70, 6, 15, TEXT_COLOR);
  
  drawCenteredText("EMERGENCY!", 95, 2, TEXT_COLOR);
  tft.setTextSize(1);
  
  if (isRecording) {
    int elapsed = (millis() - recordingStartTime) / 1000;
    int remaining = 30 - elapsed;
    String recordingInfo = "Recording: " + String(remaining) + "s left";
    drawCenteredText(recordingInfo.c_str(), 115, 1, ST77XX_YELLOW);
  } else {
    drawCenteredText("Signal sent to HQ...", 115, 1, TEXT_COLOR);
  }
  
  drawCenteredText("Press BACK to return", 140, 1, TEXT_COLOR);
}

void drawSubMenu(const char* title, const char* message1, const char* message2 = "", const char* message3 = "") {
  tft.fillScreen(BACKGROUND_COLOR);
  tft.fillRect(0, 0, tft.width(), 40, ACCENT_COLOR);
  tft.setTextSize(2);
  tft.setTextColor(BACKGROUND_COLOR);
  drawCenteredText(title, 5, 2, TEXT_COLOR);
  
  tft.setTextColor(HIGHLIGHT_COLOR);
  if (strlen(message1) > 0) {
    drawCenteredText(message1, 50, 1, HIGHLIGHT_COLOR);
  }
  
  tft.setTextColor(TEXT_COLOR);
  if (strlen(message2) > 0) {
    drawCenteredText(message2, 90, 1, TEXT_COLOR);
  }
  if (strlen(message3) > 0) {
    drawCenteredText(message3, 120, 1, TEXT_COLOR);
  }
  
  tft.setTextColor(ST77XX_GREEN);
  drawCenteredText("Press BACK to return", 140, 1, ST77XX_GREEN);
}

void drawCaseActiveWarning() {
  tft.fillScreen(ST77XX_RED);
  tft.setTextColor(ST77XX_WHITE);
  drawCenteredText("CASE ", 10, 2, ST77XX_WHITE);
  drawCenteredText("ACTIVE! ", 30, 2, ST77XX_WHITE);
  drawCenteredText("Close current case", 70, 1, ST77XX_WHITE);
  drawCenteredText("to start new one", 80, 1, ST77XX_WHITE);
  if(caseNoPlate) {
    drawCenteredText("Current: No Plate", 100, 1, ST77XX_YELLOW);
  } else {
    String currentCase = "Current: " + String(currentPlate);
    drawCenteredText(currentCase.c_str(), 100, 1, ST77XX_YELLOW);
  }
  drawCenteredText("Press BACK to return", 120, 1, ST77XX_GREEN);
}


int pwmChannel = 0;
int pwmFreq = 5000;
int pwmResolution = 8;   // 0–255 brightness



// ===== SETUP =====
void setup() {


  // for the PMW signal for the birhgtness of the screen 
#if defined(ESP32)
  ledcSetup(pwmChannel, pwmFreq, pwmResolution);
  ledcAttachPin(BACKLIGHT_PIN, pwmChannel);
#else


  pinMode(BACKLIGHT_PIN, OUTPUT);
#endif



  Serial.begin(115200);
  
  Wire.begin(21, 22);
  Wire.setClock(400000);
  
  Serial.println("🔍 Initializing SD Card...");
  if (!SD.begin(SD_CS)) {
    Serial.println("❌ SD Card initialization failed!");
  } else {
    Serial.println("✅ SD Card initialized successfully");
  }

  setupHeartSensor();
  
  tft.initR(INITR_BLACKTAB); 
  tft.setRotation(2);
  tft.fillScreen(BACKGROUND_COLOR);

  setBrightness(50) ;          // set brightness of the scren to 100 for better filmming 



  pinMode(BUTTON_UP, INPUT_PULLUP);
  pinMode(BUTTON_DOWN, INPUT_PULLUP);
  pinMode(BUTTON_SELECT, INPUT_PULLUP);
  pinMode(BUTTON_BACK, INPUT_PULLUP);

  analogReadResolution(12);

  drawNormalMode();
  
  Serial.println("SYSTEM: Tactical Watch initialized and ready");
}

// ===== LOOP =====
void loop() {
  unsigned long currentTime = millis();
  
  // CRITICAL: Audio recording has highest priority - call it multiple times per loop
  for(int i = 0; i < 10; i++) {
    recordAudioSample();
  }
  
  // Then update heart rate sensor
  readHeartSensor();
  
  // Then handle display updates
  if (watchMode == 0 && currentTime - lastHeartUpdate >= HEART_UPDATE_INTERVAL && !systemLocked) {
    lastHeartUpdate = currentTime;
    
    int hrX = 10, hrY = 110;
    tft.drawRGBBitmap(0, 95, epd_bitmap_image6_part, 128, 65);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_BLACK);
    tft.setCursor(hrX + 32, hrY + 5);
    tft.print(heartRate);
    tft.print(" bpm");
  }

  bool upPressed = buttonPressed(BUTTON_UP);
  bool downPressed = buttonPressed(BUTTON_DOWN);
  bool selectPressed = buttonPressed(BUTTON_SELECT);
  bool backPressed = buttonPressed(BUTTON_BACK);

  // BACK BUTTON HANDLING
  if(backPressed && !backPressedPrev) {
    backPressStart = currentTime;
  }

  if(!backPressed && backPressedPrev) {
    unsigned long held = currentTime - backPressStart;
    
    if(held >= 2000) {
      if(systemLocked) {
        drawLockedScreen();
      } else if(watchMode == 2) {
        watchMode = 0;
        resetPassword();
        drawNormalMode();
      } else if(watchMode == 1) {
        watchMode = 0;
        drawNormalMode();
      } else if(watchMode == 0) {
        watchMode = 2;
        resetPassword();
        drawPasswordScreen();
      }
    } else {
      if(systemLocked) {
        // Ignore
      } 
      else if(watchMode == 2) {
        if(passwordIndex < PASSWORD_LENGTH) {
          enteredPassword[passwordIndex++] = 4;
          drawPasswordScreen();
          
          if(passwordIndex == PASSWORD_LENGTH) {
            if(checkPassword()) {
              failedAttempts = 0;
              watchMode = 1;
              menuLevel = 0;
              cursor = 0;
              drawMainMenu();
            } else {
              failedAttempts++;
              if(failedAttempts >= MAX_ATTEMPTS) {
                systemLocked = true;
                sendSecurityAlert();
                drawLockedScreen();
              } else {
                tft.fillScreen(ST77XX_RED);
                drawCenteredText("WRONG", 20, 2, ST77XX_WHITE);
                drawCenteredText("PASSWARD!!", 40, 2, ST77XX_WHITE);
                String attemptText = "Attempt " + String(failedAttempts) + "/3";
                drawCenteredText(attemptText.c_str(), 80, 1, ST77XX_WHITE);
                drawCenteredText("Long press BACK", 100, 1, ST77XX_WHITE);
                drawCenteredText("to try again", 110, 1, ST77XX_WHITE);
                delay(2000);
                resetPassword();
                drawPasswordScreen();
              }
            }
          }
        }
      } 
      else if(watchMode == 1) {
        handleBackButtonInTacticalMode();
      }
    }
  }
  backPressedPrev = backPressed;

  // PASSWORD ENTRY
  if(watchMode == 2 && !systemLocked) {
    int buttonNum = getButtonNumber(upPressed, downPressed, selectPressed, false);
    
    if(buttonNum > 0 && passwordIndex < PASSWORD_LENGTH) {
      enteredPassword[passwordIndex++] = buttonNum;
      drawPasswordScreen();
      
      if(passwordIndex == PASSWORD_LENGTH) {
        if(checkPassword()) {
          failedAttempts = 0;
          watchMode = 1;
          menuLevel = 0;
          cursor = 0;
          drawMainMenu();
        } else {
          failedAttempts++;
          if(failedAttempts >= MAX_ATTEMPTS) {
            systemLocked = true;
            sendSecurityAlert();
            drawLockedScreen();
          } else {
            tft.fillScreen(ST77XX_RED);
            drawCenteredText("WRONG", 20, 2, ST77XX_WHITE);
            drawCenteredText("PASSWARD!!", 40, 2, ST77XX_WHITE);
            String attemptText = "Attempt " + String(failedAttempts) + "/3";
            drawCenteredText(attemptText.c_str(), 80, 1, ST77XX_WHITE);
            drawCenteredText("Long press BACK", 100, 1, ST77XX_WHITE);
            drawCenteredText("to try again", 110, 1, ST77XX_WHITE);
            delay(2000);
            resetPassword();
            drawPasswordScreen();
          }
        }
      }
      delay(300);
    }
    return;
  }

  // SYSTEM LOCKED MODE
  if(systemLocked) {
    if(watchMode != 0) {
      watchMode = 0;
      drawLockedScreen();
    }
    return;
  }

  if(watchMode == 0) return;

  // TACTICAL MENU NAVIGATION
  if(menuLevel <= 3) {

    if(upPressed){
      switch(menuLevel){
        case 0: cursor = (cursor-1+mainMenuLength)%mainMenuLength; drawMainMenu(); break;
        case 1: cursor = (cursor-1+startCaseLength)%startCaseLength; drawStartCaseMenu(); break;
        case 2: cursor = (cursor-1+settingsLength)%settingsLength; drawSettingsMenu(); break;
        case 3: plateCharIndex--; if(plateCharIndex<0) plateCharIndex=strlen(plateChars)-1; drawPlateInput(); break;
      }
      delay(150);
    }

    if(downPressed){
      switch(menuLevel){
        case 0: cursor = (cursor+1)%mainMenuLength; drawMainMenu(); break;
        case 1: cursor = (cursor+1)%startCaseLength; drawStartCaseMenu(); break;
        case 2: cursor = (cursor+1)%settingsLength; drawSettingsMenu(); break;
        case 3: plateCharIndex++; if(plateCharIndex>=strlen(plateChars)) plateCharIndex=0; drawPlateInput(); break;
      }
      delay(150);
    }

    if(selectPressed){
      switch(menuLevel){
        case 0:
          if(cursor==0) {
            if(canStartCase()) {
              menuLevel=1; cursor=0; drawStartCaseMenu();
            } else {
              menuLevel = 5;
              drawCaseActiveWarning();
            }
          }
          else if(cursor==1) {
            if(caseActive) {
              closeCase();
              menuLevel = 5;
              drawSubMenu("CLOSED", "Case completed", "System ready for", "new assignment");
            } else {
              menuLevel = 5;
              drawSubMenu("NO ACTIVE CASE", "No case to close ", "Start a case first", "");
            }
          }
          else if(cursor==2){ 
            menuLevel=4; 
            sendEmergencySignal();
            drawEmergencyScreen(); 
          }
          else if(cursor==3){ menuLevel=2; cursor=0; drawSettingsMenu(); }
          break;

        case 1:
          if(cursor==0){ 
            if(canStartCase()) {
              menuLevel=3; 
              plateIndex=0; 
              plateCharIndex=0; 
              memset(plateInput,0,sizeof(plateInput)); 
              drawPlateInput(); 
            } else {
              menuLevel = 5;
              drawCaseActiveWarning();
            }
          }
          else if(cursor==1){ 
            if(canStartCase()) {
              menuLevel=5;
              startCaseNoPlate();
              drawSubMenu("STARTED", "No Plate", "Case logged in system", "Ready for operation");
            } else {
              menuLevel = 5;
              drawCaseActiveWarning();
            }
          }
          break;

        case 2:
          if(cursor==0){ menuLevel=6; drawSubMenu("BRIGHTNESS", "Adjust Level", "Use UP/DOWN buttons", "Current: Medium"); }
          else if(cursor==1){ menuLevel=7; drawSubMenu("SOUND SETTINGS", "Buzzer: ON", "Press SELECT to toggle", "Status: Enabled"); }
          else if(cursor==2){ menuLevel=8; drawSubMenu("ABOUT", "Tactical Watch", "Version 1.0", "Emergency Response System"); }
          break;

        case 3:
          if(plateIndex < 6){
            plateInput[plateIndex++] = plateChars[plateCharIndex];
            plateCharIndex=0;
            drawPlateInput();
          }
          break;
      }
      delay(150);
    }
  }
}

// Handle back button in tactical mode
void handleBackButtonInTacticalMode() {
  switch(menuLevel) {
    case 0:
      break;

    case 1:
      menuLevel = 0;
      cursor = 0;
      drawMainMenu();
      break;

    case 2:
      menuLevel = 0;
      cursor = 0;
      drawMainMenu();
      break;

    case 3:
      if (plateIndex == 6) {
        startCaseWithPlate(plateInput);
        menuLevel = 9;
        drawSubMenu("CASE STARTED", "Plate Registered", plateInput, "Ready for operation");
      } else {
        menuLevel = 1;
        cursor = 0;
        drawStartCaseMenu();
      }
      break;

    case 4: // Emergency
    case 5: // Case Closed / No Active Case / Case Started (no plate)
    case 6: // Brightness
    case 7: // Sound
    case 8: // About
    case 9: // Case started with plate
      menuLevel = 0;
      cursor = 0;
      drawMainMenu();
      break;
  }
  
}



void setBrightness(uint8_t value) {
#if defined(ESP32)
  ledcWrite(pwmChannel, value);   // ESP32 PWM
#else
  analogWrite(BACKLIGHT_PIN, value);  // Arduino PWM
#endif
}