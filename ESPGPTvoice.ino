#include <Arduino.h>
#include <driver/i2s.h>  // For I2S support
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <cmath>

// -----------------------------------------------------------------------------
// Global conversation / ChatGPT info
// -----------------------------------------------------------------------------
const int NCONV = 10; // Number of conversation parts to remember
String conversationHistory[NCONV];
int historyIndex = 0;

const char *characterCharacteristics =
  "You are a helpful assistant, whose responses are optimised for being heard rather than read. Keep responses brief and to the point"
  "You always use British English vocabulary and measurements. You are willing to argue with me if you think I am wrong.";
const char *voice     = "nova";
const char *voice     = "fable";
const char *GPTModel  = "gpt-4o";
const float temperature = 0.4;


// -----------------------------------------------------------------------------
// Wi-Fi and OpenAI credentials
// -----------------------------------------------------------------------------
#define WIFI_SSID     "***YOUR-SSID-HERE***"
#define WIFI_PASSWORD "***YOUR-PASSWORD-HERE***"

const char* openai_api_key  = "^^^YOUR-API-KEY-HERE***";
const char* openai_endpoint = "https://api.openai.com/v1/chat/completions";
const char* serverName      = "https://api.openai.com/v1/audio/speech";

WiFiClientSecure client;
HTTPClient http;

// -----------------------------------------------------------------------------
// Pin assignments
// -----------------------------------------------------------------------------
#define I2S_BCLK  32
#define I2S_LRC   27
#define I2S_DOUT  12   // to MAX98357A
#define I2S_SD    26   // from microphone

// SD card pins
#define SD_CS   5
#define SD_MOSI 23
#define SD_SCK  19
#define SD_MISO 18

// Button & LED
#define BUTTON_PIN 17
#define LED_PIN    33

// -----------------------------------------------------------------------------
// Audio objects & file paths
// -----------------------------------------------------------------------------
AudioGeneratorMP3 *mp3      = nullptr;
AudioFileSourceSD  *mp3File  = nullptr;
AudioOutputI2S     *audioOut = nullptr;

const char *recordingFile = "/question.wav"; // File to record audio
const char *mp3FilePath   = "/answer.mp3";   // File to play back

// WAV constants
#define SAMPLE_RATE     16000
#define BITS_PER_SAMPLE 16
#define CHANNELS        1

File recordingFileHandle;
size_t audioDataSize = 0; // size of recorded audio in bytes

// Conversation logic
bool conversationActive = false; 
bool isRecording        = false; 

// We'll measure a baseline volume each time conversation is started
float baselineVolume    = 0.0;
float margin           = 0.0;
float dynamicThreshold = 0.0;

// Increase the factor to reduce false positives
float factor           = 80.0; // margin = factor * mad

// If user is not speaking, LED is on. Once speaking, LED off
unsigned long lastTimeAboveThreshold = 0; 
unsigned long silenceDuration  = 1000; // 1s => done

// Keep track of whether the microphone I2S is currently installed
bool micInstalled = false;

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void connectToWiFi();

void installMicI2S();
void uninstallMicI2S();
void measureAmbientNoise();

void beginRecording();
void stopRecording();

String STTOpenAIAPI(const char* filePath);
String ChatGPTOpenAIAPI(String prompt);
bool   TTSOpenAIAPI(String text);
String tidyStringForJSON(String input);

void flashLED(int nflash);
void printFormatted(String input, int lineWidth);
void playMp3File(const char *filename);

void writeWavHeader(File &file);
void updateWavHeader(File &file);

// -----------------------------------------------------------------------------
// connectToWiFi()
// -----------------------------------------------------------------------------
void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nConnected to Wi-Fi.");
  client.setInsecure();
  http.setTimeout(15000);
}

// -----------------------------------------------------------------------------
// installMicI2S() - sets up I2S for mic, discards 100 reads
// -----------------------------------------------------------------------------
void installMicI2S(){
  if(micInstalled) return; // already installed

  // I2S config
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len   = 1024
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRC,
    .data_out_num = -1,
    .data_in_num  = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  // Discard 100 reads
  for(int i=0; i<100; i++){
    int16_t discardBuf[256];
    size_t discardBytes=0;
    i2s_read(I2S_NUM_0, discardBuf, sizeof(discardBuf), &discardBytes, 100);
  }

  micInstalled = true;
  Serial.println("Installed mic I2S with 100 discards.");
}

// -----------------------------------------------------------------------------
// uninstallMicI2S() - uninstalls microphone I2S
// -----------------------------------------------------------------------------
void uninstallMicI2S(){
  if(!micInstalled) return;
  i2s_driver_uninstall(I2S_NUM_0);
  micInstalled = false;
  Serial.println("Uninstalled mic I2S driver.");
}

// -----------------------------------------------------------------------------
// measureAmbientNoise()
//  1) install I2S
//  2) wait 2s for mic to settle
//  3) measure for 1s
//  4) compute baseline
//  5) uninstall
// -----------------------------------------------------------------------------
void measureAmbientNoise() {
  Serial.println("Measuring ambient noise...");
  installMicI2S(); // sets up I2S & discards 100 reads

  // let mic settle ~2s
  delay(2000);

  unsigned long startTime     = millis();
  const unsigned long measureDuration = 1000; // 1s
  std::vector<long> volumes;

  while ((millis() - startTime) < measureDuration) {
    int16_t buffer[256];
    size_t bytesRead = 0;
    if (i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, 100) == ESP_OK) {
      size_t samplesRead = bytesRead / sizeof(int16_t);
      if (samplesRead > 0) {
        long sumAbs = 0;
        for (size_t i = 0; i < samplesRead; i++) {
          sumAbs += abs(buffer[i]);
        }
        long avgVolume = sumAbs / samplesRead;
        volumes.push_back(avgVolume);
      }
    }
  }

  // done measuring
  uninstallMicI2S();

  if(volumes.empty()){
    baselineVolume    = 0;
    margin           = 0;
    dynamicThreshold = 0;
    Serial.println("No data => baseline=0, threshold=0");
    return;
  }

  long total = 0;
  for(auto v : volumes){
    total += v;
  }
  float avg = float(total) / float(volumes.size());

  float mad = 0.;
  for(auto v : volumes){
    mad = mad + fabs(v - avg);
  }
  mad = mad / float(volumes.size());
  
  /*
  float largestDiff = 0;
  for(auto v : volumes){
    float diff = fabs(v - avg);
    if(diff > largestDiff){
      largestDiff = diff;
    }
  }
  */

  baselineVolume    = avg;
  //margin           = factor * mad;
  // let's try hardwiring for now
  margin = 1.*factor;
  dynamicThreshold = baselineVolume + margin;

  Serial.print("Ambient noise measurement complete: baseline=");
  Serial.print(baselineVolume);
  Serial.print(", MAD=");
  Serial.print(mad);
  Serial.print(", margin=");
  Serial.print(margin);
  Serial.print(", threshold=");
  Serial.println(dynamicThreshold);
}

// -----------------------------------------------------------------------------
// beginRecording()
// -----------------------------------------------------------------------------
void beginRecording() {
  recordingFileHandle = SD.open(recordingFile, FILE_WRITE);
  if(!recordingFileHandle){
    Serial.println("Failed to open file for recording.");
    return;
  }
  writeWavHeader(recordingFileHandle);
  audioDataSize = 0;
  isRecording   = true;
  Serial.println("** Started recording **");
}

// -----------------------------------------------------------------------------
// stopRecording()
// -----------------------------------------------------------------------------
void stopRecording() {
  isRecording = false;
  Serial.println("** Stopping recording **");
  flashLED(1);

  updateWavHeader(recordingFileHandle);
  recordingFileHandle.close();

  // Uninstall mic so TTS can re-use I2S for the speaker
  uninstallMicI2S();

  // STT->ChatGPT->TTS->playback
  String transcript = STTOpenAIAPI(recordingFile);
  Serial.println("Question ------------------------------------------");
  Serial.println(transcript);
  flashLED(2);

  String chatResponse = ChatGPTOpenAIAPI(transcript);
  Serial.println("Answer --------------------------------------------");
  printFormatted(chatResponse, 120);
  Serial.println(" ");

  String cleanChatResponse = tidyStringForJSON(chatResponse);
  flashLED(3);
  if(TTSOpenAIAPI(cleanChatResponse)) {
    playMp3File(mp3FilePath);
  } else {
    Serial.println("Failed to process text.");
  }

  // Blink LED
  flashLED(4);

  // If conversation is still active after TTS, reinstall mic so we can keep listening
  if(conversationActive){
    installMicI2S();
  }
}

// -----------------------------------------------------------------------------
// monitorMicrophone()
//   - If not recording => LED=ON, ensure mic is installed, read volume
//   - If volume>threshold => beginRecording => LED=OFF
//   - If recording => store data, if silent => stopRecording
// -----------------------------------------------------------------------------
void monitorMicrophone(){
  // If we're not recording => LED on while waiting
  if(!isRecording){
    digitalWrite(LED_PIN, HIGH);

    // Make sure mic is installed
    if(!micInstalled){
      installMicI2S();
    }
  }

  // read a chunk
  int16_t buffer[256];
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, 100);
  if(err != ESP_OK){
    return;
  }
  size_t samplesRead = bytesRead / sizeof(int16_t);
  if(samplesRead == 0){
    return;
  }

  long sumAbs = 0;
  for(size_t i=0; i<samplesRead; i++){
    sumAbs += abs(buffer[i]);
  }
  long avgVolume = sumAbs / samplesRead;
  bool aboveThresh = (avgVolume > dynamicThreshold);

  static unsigned long lastTimeBelow = 0;
  unsigned long now = millis();

  // If not recording & above threshold => start
  if(!isRecording && aboveThresh){
    digitalWrite(LED_PIN, LOW);
    beginRecording();
  }

  // If recording => store data
  if(isRecording){
    // optional gain
    const float gain=1.0;
    for(size_t i=0; i<samplesRead; i++){
      float val = buffer[i]*gain;
      if(val>32767) val=32767;
      if(val<-32768) val=-32768;
      buffer[i]=(int16_t)val;
    }

    // write
    recordingFileHandle.write((uint8_t*)buffer, bytesRead);
    audioDataSize += bytesRead;

    // if above threshold => reset silence timer
    if(aboveThresh){
      lastTimeBelow=0;
    } else {
      // track how long we've been below
      if(lastTimeBelow==0){
        lastTimeBelow=now;
      } else if((now - lastTimeBelow)>silenceDuration){
        stopRecording();
      }
    }
  }
}

// -----------------------------------------------------------------------------
// STTOpenAIAPI()
// -----------------------------------------------------------------------------
String STTOpenAIAPI(const char* filePath){
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("Wi-Fi not connected!");
    return "";
  }

  File wavFile=SD.open(filePath);
  if(!wavFile){
    Serial.println("Error: Failed to open WAV file.");
    return "";
  }
  size_t fileSize=wavFile.size();
  if(fileSize==0){
    Serial.println("Error: WAV file is empty.");
    wavFile.close();
    return "";
  }

  WiFiClientSecure sttClient;
  sttClient.setInsecure();
  if(!sttClient.connect("api.openai.com", 443)){
    Serial.println("Error: Connection to OpenAI STT failed.");
    wavFile.close();
    return "";
  }

  String boundary="----ESP32Boundary";
  sttClient.print("POST /v1/audio/transcriptions HTTP/1.1\r\n");
  sttClient.print("Host: api.openai.com\r\n");
  sttClient.print("Authorization: Bearer "+String(openai_api_key)+"\r\n");
  sttClient.print("Content-Type: multipart/form-data; boundary="+boundary+"\r\n");

  String modelPart= "--"+boundary+"\r\n"
                    "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                    "whisper-1\r\n";
  String filePartStart= "--"+boundary+"\r\n"
                        "Content-Disposition: form-data; name=\"file\"; filename=\"question.wav\"\r\n"
                        "Content-Type: audio/wav\r\n\r\n";
  String filePartEnd= "\r\n--"+boundary+"--\r\n";

  size_t contentLength=modelPart.length()+filePartStart.length()+fileSize+filePartEnd.length();
  sttClient.print("Content-Length: "+String(contentLength)+"\r\n\r\n");

  // send body
  sttClient.print(modelPart);
  sttClient.print(filePartStart);

  const size_t chunkSize=1024;
  uint8_t tempBuf[chunkSize];
  while(wavFile.available()){
    size_t rd=wavFile.read(tempBuf, chunkSize);
    sttClient.write(tempBuf, rd);
  }
  wavFile.close();
  sttClient.print(filePartEnd);

  // read headers
  while(sttClient.connected()){
    String line=sttClient.readStringUntil('\n');
    if(line=="\r") break;
  }

  // read body
  String response;
  while(sttClient.available()){
    response+=(char)sttClient.read();
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error=deserializeJson(doc, response);
  if(error){
    Serial.print("JSON parsing error: ");
    Serial.println(error.c_str());
    return "";
  }

  if(doc["error"].is<JsonObject>()){
    String errMsg=doc["error"]["message"].as<String>();
    Serial.print("OpenAI Whisper error: ");
    Serial.println(errMsg);
    return "";
  }

  if(doc["text"].isNull()){
    Serial.println("STT API returned null or empty text.");
    return "";
  }

  String text=doc["text"].as<String>();
  return tidyStringForJSON(text);
}

// -----------------------------------------------------------------------------
// ChatGPTOpenAIAPI()
// -----------------------------------------------------------------------------
String ChatGPTOpenAIAPI(String prompt){
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("Wi-Fi not connected!");
    return "";
  }

  HTTPClient localHttp;
  localHttp.begin(openai_endpoint);
  localHttp.addHeader("Content-Type", "application/json");
  localHttp.addHeader("Authorization", String("Bearer ")+openai_api_key);

  String escapedPrompt=tidyStringForJSON(prompt);
  DynamicJsonDocument doc(8000*NCONV);
  JsonArray messages=doc.createNestedArray("messages");

  // system role
  {
    JsonObject systemMessage = messages.createNestedObject();
    systemMessage["role"]    = "system";
    systemMessage["content"] = characterCharacteristics;
  }

  // conversation history
  for(int i=0; i<NCONV; i++){
    if(conversationHistory[i]!=""){
      JsonObject userMsg=messages.createNestedObject();
      userMsg["role"]="user";
      userMsg["content"]=tidyStringForJSON(conversationHistory[i]);
    }
  }

  // current prompt
  {
    JsonObject userPrompt=messages.createNestedObject();
    userPrompt["role"]="user";
    userPrompt["content"]=escapedPrompt;
  }

  doc["model"]=GPTModel;
  doc["max_tokens"]=1000;
  doc["temperature"]=temperature;

  String payload;
  serializeJson(doc, payload);

  localHttp.setTimeout(15000);
  int httpResponseCode=localHttp.POST(payload);

  if(httpResponseCode>0){
    String response=localHttp.getString();
    StaticJsonDocument<2048> responseDoc;
    DeserializationError err=deserializeJson(responseDoc, response);
    if(err){
      Serial.print("JSON deserialization error: ");
      Serial.println(err.c_str());
      return "";
    }
    const char* content=responseDoc["choices"][0]["message"]["content"];
    if(content){
      // update conversation history
      conversationHistory[historyIndex]=prompt;
      historyIndex=(historyIndex+1)%NCONV;
      conversationHistory[historyIndex]=String(content);
      historyIndex=(historyIndex+1)%NCONV;

      doc.clear();
      doc.shrinkToFit();
      return String(content);
    }
  } else{
    Serial.print("HTTP request failed. Response code: ");
    Serial.println(httpResponseCode);
  }

  return "";
}

// -----------------------------------------------------------------------------
// TTSOpenAIAPI()
// -----------------------------------------------------------------------------
bool TTSOpenAIAPI(String text){
  http.begin(client, serverName);
  http.addHeader("Content-Type","application/json");
  http.addHeader("Authorization",String("Bearer ")+openai_api_key);

  String cleantext=tidyStringForJSON(text);
  String payload="{\"model\":\"tts-1-hd\",\"input\":\""+cleantext+"\",\"voice\":\""+voice+"\",\"speed\":\"1.0\",\"response_format\":\"mp3\"}";

  int httpResponseCode=http.POST(payload);

  if(httpResponseCode==200){
    File outputFile=SD.open(mp3FilePath, FILE_WRITE);
    if(!outputFile){
      Serial.println("Failed to open file for writing.");
      http.end();
      return false;
    }
    http.writeToStream(&outputFile);
    outputFile.close();
    http.end();
    return true;
  } else{
    Serial.printf("HTTP POST failed with code %d\n",httpResponseCode);
    String respBody=http.getString();
    Serial.println("Response body: "+respBody);
    http.end();
    return false;
  }
}

// -----------------------------------------------------------------------------
// tidyStringForJSON()
// -----------------------------------------------------------------------------
String tidyStringForJSON(String input){
  String out;
  for(unsigned int i=0; i<input.length(); i++){
    char c=input.charAt(i);
    switch(c){
      case '\"': out+="\\\""; break;
      case '\\': out+="\\\\"; break;
      case '\b': out+="\\b";  break;
      case '\f': out+="\\f";  break;
      case '\n': out+="\\n";  break;
      case '\r': out+="\\r";  break;
      case '\t': out+="\\t";  break;
      default:
        if(c<0x20 || c>0x7E){
          char buf[7];
          sprintf(buf,"\\u%04X", c & 0xFF);
          out+=buf;
        } else{
          out+=c;
        }
        break;
    }
  }
  return out;
}

// -----------------------------------------------------------------------------
// flashLED()
// -----------------------------------------------------------------------------
void flashLED(int nflash){
  for(int i=0; i<nflash; i++){
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
  }
}

// -----------------------------------------------------------------------------
// printFormatted()
// -----------------------------------------------------------------------------
void printFormatted(String input,int lineWidth){
  int len=input.length();
  int pos=0;
  while(pos<len){
    int nextLineBreak=input.indexOf('\n',pos);
    int nextCarriageReturn=input.indexOf('\r',pos);
    int nextBreak=-1;
    if(nextLineBreak!=-1 && nextCarriageReturn!=-1){
      nextBreak=min(nextLineBreak,nextCarriageReturn);
    } else if(nextLineBreak!=-1){
      nextBreak=nextLineBreak;
    } else if(nextCarriageReturn!=-1){
      nextBreak=nextCarriageReturn;
    }

    if(nextBreak!=-1 && nextBreak<pos+lineWidth){
      String seg=input.substring(pos,nextBreak);
      Serial.println(seg);
      pos=nextBreak+1;
    } else{
      int nextPos=pos+lineWidth;
      if(nextPos>=len){
        Serial.println(input.substring(pos));
        break;
      }
      int lastSpace=input.lastIndexOf(' ', nextPos);
      if(lastSpace<=pos){
        Serial.println(input.substring(pos,nextPos));
        pos=nextPos;
      } else{
        Serial.println(input.substring(pos,lastSpace));
        pos=lastSpace+1;
      }
    }
  }
}

static void printLineSegment(String segment,int lineWidth){
  int segPos=0;
  while(segPos<segment.length()){
    int nextPos=segPos+lineWidth;
    if(nextPos>=segment.length()){
      Serial.print(segment.substring(segPos));
      break;
    }
    int lastSpace=segment.lastIndexOf(' ',nextPos);
    if(lastSpace<=segPos){
      Serial.println(segment.substring(segPos,nextPos));
      segPos=nextPos;
    } else{
      Serial.println(segment.substring(segPos,lastSpace));
      segPos=lastSpace+1;
    }
  }
  Serial.println();
}

// -----------------------------------------------------------------------------
// playMp3File()
// -----------------------------------------------------------------------------
void playMp3File(const char *filename){
  if(!SD.exists(filename)){
    Serial.printf("File %s not found on SD card!\n", filename);
    return;
  }

  // Here we set up a new AudioOutputI2S for the speaker. 
  // micI2S is uninstalled in stopRecording, so no conflict
  mp3File=new AudioFileSourceSD(filename);
  if(!mp3File){
    Serial.println("Failed to open MP3 file source!");
    return;
  }

  mp3=new AudioGeneratorMP3();
  if(!mp3){
    Serial.println("Failed to initialize MP3 decoder!");
    delete mp3File;
    return;
  }

  audioOut=new AudioOutputI2S();
  audioOut->SetPinout(I2S_BCLK,I2S_LRC,I2S_DOUT);
  audioOut->SetGain(1.0);

  if(!mp3->begin(mp3File,audioOut)){
    Serial.println("Failed to start MP3 playback!");
    delete mp3;
    delete mp3File;
    delete audioOut;
    return;
  }

  while(mp3->isRunning()){
    if(!mp3->loop()){
      mp3->stop();
      break;
    }
    // If user presses button, skip playback
    if(digitalRead(BUTTON_PIN)==LOW){
      Serial.println("** Playback interrupted by button **");
      mp3->stop();
      break;
    }
  }
  delay(500);

  delete mp3;
  delete mp3File;
  delete audioOut;
  mp3=nullptr;
  mp3File=nullptr;
  audioOut=nullptr;
}

// -----------------------------------------------------------------------------
// WAV file header utilities
// -----------------------------------------------------------------------------
void writeWavHeader(File &file){
  uint8_t header[44]={
    'R','I','F','F',
    0,0,0,0,
    'W','A','V','E',
    'f','m','t',' ',
    16,0,0,0,
    1,0,
    CHANNELS,0,
    (uint8_t)(SAMPLE_RATE & 0xFF), (uint8_t)((SAMPLE_RATE>>8)&0xFF),
    (uint8_t)((SAMPLE_RATE>>16)&0xFF),(uint8_t)((SAMPLE_RATE>>24)&0xFF),
    (uint8_t)((SAMPLE_RATE*CHANNELS*BITS_PER_SAMPLE/8)&0xFF),
    (uint8_t)(((SAMPLE_RATE*CHANNELS*BITS_PER_SAMPLE/8)>>8)&0xFF),
    (uint8_t)(((SAMPLE_RATE*CHANNELS*BITS_PER_SAMPLE/8)>>16)&0xFF),
    (uint8_t)(((SAMPLE_RATE*CHANNELS*BITS_PER_SAMPLE/8)>>24)&0xFF),
    (uint8_t)((CHANNELS*BITS_PER_SAMPLE/8)&0xFF),
    (uint8_t)(((CHANNELS*BITS_PER_SAMPLE/8)>>8)&0xFF),
    BITS_PER_SAMPLE,0,
    'd','a','t','a',
    0,0,0,0
  };
  file.write(header,sizeof(header));
}

void updateWavHeader(File &file){
  uint32_t fileSize=audioDataSize+44-8;
  uint32_t dataChunkSize=audioDataSize;

  file.seek(4);
  file.write((uint8_t*)&fileSize,4);

  file.seek(40);
  file.write((uint8_t*)&dataChunkSize,4);
}

// -----------------------------------------------------------------------------
// setup()
// -----------------------------------------------------------------------------
void setup(){
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN,INPUT_PULLUP);
  pinMode(LED_PIN,OUTPUT);

  SPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  if(!SD.begin(SD_CS)){
    Serial.println("SD card initialization failed!");
    while(true);
  }

  connectToWiFi();

  // measure ambient noise right away
  measureAmbientNoise();

  // Quick test: greet politely
  String chatResponse=ChatGPTOpenAIAPI("Greet me politely");
  Serial.println("Response:");
  Serial.println(chatResponse);

  String cleanChatResponse=tidyStringForJSON(chatResponse);
  if(TTSOpenAIAPI(cleanChatResponse)){
    playMp3File(mp3FilePath);
  } else{
    Serial.println("Failed TTS greeting");
  }

  // Start conversation mode by default
  conversationActive=true;
  Serial.println("Now defaulting to conversation mode.");

}

// -----------------------------------------------------------------------------
// loop()
// -----------------------------------------------------------------------------
void loop(){
  static bool prevButtonState=HIGH;
  bool currButtonState=digitalRead(BUTTON_PIN);

  if(prevButtonState==HIGH && currButtonState==LOW){
    conversationActive=!conversationActive;
    if(conversationActive){
      Serial.println("Conversation mode ACTIVE");
      measureAmbientNoise();
    } else{
      Serial.println("Conversation mode ENDED");
      // forcibly stop if recording
      if(isRecording){
        isRecording=false;
        updateWavHeader(recordingFileHandle);
        recordingFileHandle.close();
        uninstallMicI2S();
        Serial.println("Recording forcibly closed due to conversation end.");
      }
      digitalWrite(LED_PIN,LOW);
    }
    delay(50);
  }
  prevButtonState=currButtonState;

  if(conversationActive){
    monitorMicrophone();
  }
}
