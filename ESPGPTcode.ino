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


// Global variables to store conversation history
const int NCONV = 10; // Number of conversation parts to remember
String conversationHistory[NCONV];
int historyIndex = 0;

// set up some characteristics for ChatGPT assistant:


const char *characterCharacteristics = "You are a helpful assistant, whose responses are optimised for being heard rather than read. You always use British English vocabulary and measurements. You are willing to argue with me if you think I am wrong.";
const char *voice = "nova";
const char *GPTModel = "gpt-4o-mini";
const float temperature = 0.4;

//const char *characterCharacteristics = "You are Mike Merrifield, a professional astronomer and educator. Provide thoughtful, long and accurate answers with a conversational tone. Your responses are optimised for being heard rather than read. You always use British English vocabulary and measurements.";
//const char *voice = "fable";
//const char *GPTModel = "ft:gpt-4o-2024-08-06:mike:mikemerrifieldtweaked:AqixYVip";
//const float temperature = 0.4;

// Your WIFI credentials here
#define WIFI_SSID "YOUR-WIFI-SID"
#define WIFI_PASSWORD "YOUR-WIFI-PASSWIORD"

// OpenAI API key
const char* openai_api_key = "YOUR-OPENAI-API-KEY";


// OpenAI API endpoint for chat completions
const char* openai_endpoint = "https://api.openai.com/v1/chat/completions";
const char* serverName = "https://api.openai.com/v1/audio/speech";

WiFiClientSecure client;
HTTPClient http;

// I2S pin assignments for MAX98357A and Microphone
#define I2S_BCLK 32   // Bit Clock (BCLK)
#define I2S_LRC 27    // Left-Right Clock (LRC/WS)
#define I2S_DOUT 12   // Data Out (DIN) for MAX98357A
#define I2S_SD 26     // Data In (SD) for Microphone

// SD card pin assignments
#define SD_CS 5       // Chip Select (CS)
#define SD_MOSI 23    // Master Out Slave In (MOSI)
#define SD_SCK 19     // Clock (SCK)
#define SD_MISO 18    // Master In Slave Out (MISO)

// Button pin
#define BUTTON_PIN 17 // GPIO for the button
// LED pin
#define LED_PIN 33

// Audio objects
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceSD *mp3File = nullptr;
AudioOutputI2S *audioOut = nullptr;

// File paths for question and answer audio files
const char *recordingFile = "/question.wav";  // File to record audio
const char *mp3FilePath = "/answer.mp3";      // File to play back

// Boolean to check whether currently recording
bool isRecording = false;

// WAV file constants
#define SAMPLE_RATE 16000          // Lowered sample rate for better compatibility
#define BITS_PER_SAMPLE 16
#define CHANNELS 1

File recordingFileHandle;
size_t audioDataSize = 0; // To track the size of recorded audio data

//Function to set up I2S microphone
void setupI2SMicrophone() {
  // I2S configuration
  i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = -1, // Not used
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  //Serial.println("I2S Setup complete");
}

// Function to connect to WIFI
void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nConnected to Wi-Fi.");
}


// Record the WAV file question
void makeRecording() {
  recordingFileHandle = SD.open(recordingFile, FILE_WRITE);
  if (!recordingFileHandle) {
    Serial.println("Failed to open file for recording.");
    return;
  }

  writeWavHeader(recordingFileHandle); // Write placeholder WAV header
  setupI2SMicrophone();

  isRecording = true;
  audioDataSize = 0;
  const float gain = 2.0; // Amplification factor (increase or decrease as needed)

  while (isRecording) {
    size_t bytesRead;
    int16_t buffer[512]; // Use int16_t for 16-bit audio samples
    esp_err_t err = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, portMAX_DELAY);
    if (err != ESP_OK) {
      Serial.printf("I2S read failed: %d\n", err);
      break;
    }

    // Apply gain to each sample
    size_t samplesRead = bytesRead / sizeof(int16_t);
    for (size_t i = 0; i < samplesRead; i++) {
      buffer[i] = (int16_t)(buffer[i] * gain); // Apply gain and cast back to int16_t

      // Prevent clipping
      if (buffer[i] > 32767) buffer[i] = 32767;
      if (buffer[i] < -32768) buffer[i] = -32768;
    }

    // Write amplified buffer to the file
    recordingFileHandle.write((uint8_t *)buffer, bytesRead);
    audioDataSize += bytesRead;

    if (digitalRead(BUTTON_PIN) == HIGH) {
      isRecording = false;
    }
  }

  updateWavHeader(recordingFileHandle); // Update WAV header with final sizes
  recordingFileHandle.close();
  i2s_driver_uninstall(I2S_NUM_0);
  //Serial.println("Recording stopped. Saved as /question.wav");
}

// Function to create the WAV file header
void writeWavHeader(File &file) {
  uint8_t header[44] = {
      'R', 'I', 'F', 'F',  // ChunkID
      0, 0, 0, 0,          // ChunkSize (placeholder, updated later)
      'W', 'A', 'V', 'E',  // Format
      'f', 'm', 't', ' ',  // Subchunk1ID
      16, 0, 0, 0,         // Subchunk1Size (16 for PCM)
      1, 0,                // AudioFormat (1 for PCM)
      CHANNELS, 0,         // NumChannels
      (uint8_t)(SAMPLE_RATE & 0xFF), (uint8_t)((SAMPLE_RATE >> 8) & 0xFF),
      (uint8_t)((SAMPLE_RATE >> 16) & 0xFF), (uint8_t)((SAMPLE_RATE >> 24) & 0xFF),  // SampleRate
      (uint8_t)((SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8) & 0xFF),
      (uint8_t)(((SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8) >> 8) & 0xFF),
      (uint8_t)(((SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8) >> 16) & 0xFF),
      (uint8_t)(((SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8) >> 24) & 0xFF),  // ByteRate
      (uint8_t)((CHANNELS * BITS_PER_SAMPLE / 8) & 0xFF),
      (uint8_t)(((CHANNELS * BITS_PER_SAMPLE / 8) >> 8) & 0xFF),  // BlockAlign
      BITS_PER_SAMPLE, 0,       // BitsPerSample
      'd', 'a', 't', 'a',       // Subchunk2ID
      0, 0, 0, 0                // Subchunk2Size (placeholder, updated later)
  };

  file.write(header, sizeof(header));
}


// Function to update the WAV header with the correct file size once the length of recording is known
void updateWavHeader(File &file) {
  uint32_t fileSize = audioDataSize + 44 - 8; // File size excluding "RIFF" and size fields
  uint32_t dataChunkSize = audioDataSize;    // Size of the "data" chunk

  // Update ChunkSize
  file.seek(4);
  file.write((uint8_t *)&fileSize, 4);

  // Update Subchunk2Size (data size)
  file.seek(40);
  file.write((uint8_t *)&dataChunkSize, 4);
}

// Function to call OpenAI speech-to-text API, returning the text as a string of the audio file given as an argument
String STTOpenAIAPI(const char* filePath) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi not connected!");
    return "";
  }

  // Open the WAV file from the SD card
  File wavFile = SD.open(filePath);
  if (!wavFile) {
    Serial.println("Error: Failed to open the WAV file. Check the file path.");
    return "";
  }

  size_t fileSize = wavFile.size();
  if (fileSize == 0) {
    Serial.println("Error: WAV file is empty.");
    wavFile.close();
    return "";
  }
  //Serial.printf("WAV file opened successfully. Size: %u bytes\n", fileSize);

  WiFiClientSecure client;
  client.setInsecure(); // Disable certificate validation for testing purposes

  if (!client.connect("api.openai.com", 443)) {
    Serial.println("Error: Connection to OpenAI API failed.");
    wavFile.close();
    return "";
  }

  // Define the multipart boundary
  String boundary = "----ESP32Boundary";

  // Build the HTTP POST headers
  client.print("POST /v1/audio/transcriptions HTTP/1.1\r\n");
  client.print("Host: api.openai.com\r\n");
  client.print("Authorization: Bearer " + String(openai_api_key) + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");

  // Calculate the Content-Length
  String modelPart = "--" + boundary + "\r\n"
                     "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                     "whisper-1\r\n";
  String filePartStart = "--" + boundary + "\r\n"
                         "Content-Disposition: form-data; name=\"file\"; filename=\"question.wav\"\r\n"
                         "Content-Type: audio/wav\r\n\r\n";
  String filePartEnd = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = modelPart.length() + filePartStart.length() + fileSize + filePartEnd.length();
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("\r\n");

  // Send the body
  client.print(modelPart); // Send the model parameter
  client.print(filePartStart); // Send the start of the file part

  // Send the WAV file in chunks
  const size_t chunkSize = 1024; // 1KB buffer
  uint8_t buffer[chunkSize];
  while (wavFile.available()) {
    size_t bytesRead = wavFile.read(buffer, chunkSize);
    client.write(buffer, bytesRead);
  }

  // Close the WAV file
  wavFile.close();

  client.print(filePartEnd); // Send the end of the file part

  // Read the response
  String response = "";
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break; // Headers end
  }
  while (client.available()) {
    response += char(client.read());
  }

  // Parse the JSON response to extract the text field
  //StaticJsonDocument<4096> doc;
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.print("JSON parsing error: ");
    Serial.println(error.c_str());
    return "";
  }

  String text = doc["text"].as<String>();
  //Serial.println("STT extracted text");
  //Serial.println(text);
  return tidyStringForJSON(text);
}

// Function to call OpenAI ChatGPT API, returning the string that ChatGPT generates as a response to the prompt string given as an argument
String ChatGPTOpenAIAPI(String prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi not connected!");
    return "";
  }

  HTTPClient http;

  // Prepare the OpenAI API request
  http.begin(openai_endpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + openai_api_key);

  // Make sure there are no unsutable characters in the string to be passed
  String escapedPrompt = tidyStringForJSON(prompt);

  // Construct the JSON payload
  DynamicJsonDocument doc(8000*NCONV); // Adjust size as needed
  JsonArray messages = doc.createNestedArray("messages");

  // Add system message
  JsonObject systemMessage = messages.createNestedObject();
  systemMessage["role"] = "system";
  systemMessage["content"] = characterCharacteristics;

  // Add fixed initial message
  //JsonObject fixedMessage = messages.createNestedObject();
  //fixedMessage["role"] = "user";
  //fixedMessage["content"] = preamblePrompt;

  // Add conversation history
  for (int i = 0; i < NCONV; i++) {
    if (conversationHistory[i] != "") {
      JsonObject userMessage = messages.createNestedObject();
      userMessage["role"] = "user";
      userMessage["content"] = tidyStringForJSON(conversationHistory[i]);
    }
  }

  // Add the current prompt
  JsonObject userPrompt = messages.createNestedObject();
  userPrompt["role"] = "user";
  userPrompt["content"] = escapedPrompt;

  doc["model"] = GPTModel;
  doc["max_tokens"] = 1000;
  doc["temperature"] = temperature;

  // Serialize JSON payload
  String payload;
  serializeJson(doc, payload);

  // Set timeout
  http.setTimeout(15000); // 15 seconds

  // Send POST request
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
  

    String response = http.getString();

    // Parse JSON response
    StaticJsonDocument<2048> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);
    if (error) {
      Serial.print("JSON deserialization error: ");
      Serial.println(error.c_str());
      return "";
    }

    const char* content = responseDoc["choices"][0]["message"]["content"];
    if (content) {
      // Update conversation history (keeping last NCONV exchanges)
      conversationHistory[historyIndex] = prompt;
      historyIndex = (historyIndex + 1) % NCONV;
      conversationHistory[historyIndex] = String(content);
      historyIndex = (historyIndex + 1) % NCONV;

    // tidy up the JSON document
    doc.clear();  // Removes all elements from the doc
    doc.shrinkToFit();  // Optionally shrink capacity to fit size


      return String(content);
    }
  } else {
    Serial.print("HTTP request failed. Response code: ");
    Serial.println(httpResponseCode);
  }

  return "";
}


// Function to remove control characters and other non-desirable elements in a string
String tidyStringForJSON(String input) {
  String output = "";
  for (unsigned int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);
    switch (c) {
      case '\"':
        output += "\\\""; // Escape double quote
        break;
      case '\\':
        output += "\\\\"; // Escape backslash
        break;
      case '\b':
        output += "\\b"; // Escape backspace
        break;
      case '\f':
        output += "\\f"; // Escape form feed
        break;
      case '\n':
        output += "\\n"; // Escape newline
        break;
      case '\r':
        output += "\\r"; // Escape carriage return
        break;
      case '\t':
        output += "\\t"; // Escape tab
        break;
      default:
        // Check for control characters
        if (c < 0x20 || c > 0x7E) {
          char buffer[7];
          sprintf(buffer, "\\u%04X", c); // Unicode escape sequence
          output += buffer;
        } else {
          output += c; // Append normal character
        }
        break;
    }
  }
  return output;
}

// Function to call OpenAI text-to-speech, daving as an audio file the string given as an argument
bool TTSOpenAIAPI(String text) {
  http.begin(client, serverName);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + openai_api_key);

  // Make sure there are no nasty characters in the string
  String cleantext = tidyStringForJSON(text);
  // Create JSON payload
  String payload = "{\"model\":\"tts-1-hd\",\"input\":\"" + cleantext + "\",\"voice\":\"" + voice + "\",\"speed\":\"1.0\",\"response_format\":\"mp3\"}";

  
  int httpResponseCode = http.POST(payload);

  if (httpResponseCode == 200) {
  
    File outputFile = SD.open("/answer.mp3", FILE_WRITE);
    if (!outputFile) {
      Serial.println("Failed to open file for writing.");
      http.end();
      return false;
    }

      int bytesWritten = http.writeToStream(&outputFile);
      outputFile.close();
  

    http.end();
    return true;
  } else {
    Serial.printf("HTTP POST failed with code %d\n", httpResponseCode);
    String responseBody = http.getString();
    Serial.println("Response body: " + responseBody);
    http.end();
    return false;
  }

}

// Function to flash LED nflash times to let the user know something is happening
void flashLED(int nflash) {
 for (int i = 0; i < nflash; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(100);
    digitalWrite(LED_PIN, LOW);
    delay(100);
 }
}

// Function to pretty-print ChatGPT output to the serial monitor
void printFormatted(String input, int lineWidth) {
  int len = input.length();
  int pos = 0;
  
  while (pos < len) {
    // Find the next newline or carriage return
    int nextLineBreak = input.indexOf('\n', pos);
    int nextCarriageReturn = input.indexOf('\r', pos);
    int nextBreak = -1;

    // Determine the closest line break (if any)
    if (nextLineBreak != -1 && nextCarriageReturn != -1) {
      nextBreak = min(nextLineBreak, nextCarriageReturn);
    } else if (nextLineBreak != -1) {
      nextBreak = nextLineBreak;
    } else if (nextCarriageReturn != -1) {
      nextBreak = nextCarriageReturn;
    }

    // If there's a line break, process up to that point
    if (nextBreak != -1 && nextBreak < pos + lineWidth) {
      String segment = input.substring(pos, nextBreak);
      printLineSegment(segment, lineWidth);
      pos = nextBreak + 1; // Move past the line break
    } else {
      // No immediate line break, process normally
      int nextPos = pos + lineWidth;
      if (nextPos >= len) {
        Serial.println(input.substring(pos));
        break;
      }

      int lastSpace = input.lastIndexOf(' ', nextPos);
      if (lastSpace <= pos) {
        // No space found, print the full lineWidth
        Serial.println(input.substring(pos, nextPos));
        pos = nextPos;
      } else {
        // Break at the last space
        Serial.println(input.substring(pos, lastSpace));
        pos = lastSpace + 1; // Move past the space
      }
    }
  }
}

void printLineSegment(String segment, int lineWidth) {
  int segPos = 0;
  while (segPos < segment.length()) {
    int nextPos = segPos + lineWidth;
    if (nextPos >= segment.length()) {
      Serial.print(segment.substring(segPos)); // Use `print` to avoid extra blank lines
      break;
    }
    int lastSpace = segment.lastIndexOf(' ', nextPos);
    if (lastSpace <= segPos) {
      Serial.println(segment.substring(segPos, nextPos));
      segPos = nextPos;
    } else {
      Serial.println(segment.substring(segPos, lastSpace));
      segPos = lastSpace + 1;
    }
  }
  Serial.println(); // Only add a line break after the entire segment
}


// Function to play the MP£ file created by text-to-speech
void playMp3File(const char *filename) {
  // Check if the file exists
  if (!SD.exists(filename)) {
    Serial.printf("File %s not found on SD card!\n", filename);
    return;
  }

  // Open the MP3 file
  //Serial.printf("Playing MP3 file: %s\n", filename);
  mp3File = new AudioFileSourceSD(filename);
  if (!mp3File) {
    Serial.println("Failed to open MP3 file source!");
    return;
  }

  // Initialize MP3 decoder and I2S output
  mp3 = new AudioGeneratorMP3();
  if (!mp3) {
    Serial.println("Failed to initialize MP3 decoder!");
    delete mp3File;
    return;
  }

  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audioOut->SetGain(1.0); // Set playback volume (adjust as needed)

  if (!mp3->begin(mp3File, audioOut)) {
    Serial.println("Failed to start MP3 playback!");
    delete mp3;
    delete mp3File;
    delete audioOut;
    return;
  }

  // Playback loop (with option to cut it short with a quick button press)
  while (mp3->isRunning() && digitalRead(BUTTON_PIN) == HIGH) {
    if (!mp3->loop()) {
      mp3->stop();
      break;
    }
  }
  delay(1000);


  // Clean up now we are done
  delete mp3;
  delete mp3File;
  delete audioOut;
  mp3 = nullptr;
  mp3File = nullptr;
  audioOut = nullptr;
}

void setup() {
  String chatResponse;
  String cleanChatResponse;
  Serial.begin(115200);
  delay(1000);

  // Initialize button
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Iniialize LED
  pinMode(LED_PIN, OUTPUT);


  // Initialize SD card with explicit SPI pins
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card initialization failed!");
    while (true); // Halt execution
  }

  // Initialize I2S output for playback
  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audioOut->SetGain(1.0); // Adjust output volume

// Connect to Wi-Fi
  connectToWiFi();

// Make connection insecure (bypass SSL certificate validation)
  client.setInsecure();

  // Increase HTTP client timeout
  http.setTimeout(15000); // increased timeout to 15 seconds

//Let's do an initiai "hello" call to show that ChatGPT is awake
chatResponse = ChatGPTOpenAIAPI("Greet me politely");
    Serial.println("Response:");
    Serial.println(chatResponse);
//get rid of any carriage returns, and other troublesom characters 
    cleanChatResponse = tidyStringForJSON(chatResponse);

  //  Serial.println("Sending text to OpenAI API...");
    if (TTSOpenAIAPI(cleanChatResponse)) {
  //   Serial.println("Audio saved to SD card.");
    } else {
      Serial.println("Failed to process text.");
    }

    playMp3File(mp3FilePath);

}


void loop() {
  String transcript;
  String chatResponse;
  String cleanChatResponse;
  // Wait for the button to be pressed and light LED to show we are waiting
  digitalWrite(LED_PIN, HIGH);
  while (digitalRead(BUTTON_PIN) == HIGH) {}
    //Serial.println("Button pressed: Starting recording...");

    //switch off the light
    digitalWrite(LED_PIN, LOW);
    // Start recording the .wav file (this function handles waiting for button release)
    makeRecording();


      // Send transcription request to Google VTT
      transcript = STTOpenAIAPI("/question.wav");
      Serial.println("Question ------------------------------------------");
      Serial.println(transcript);
      flashLED(1);
    chatResponse = ChatGPTOpenAIAPI(transcript);
    Serial.println("Answer --------------------------------------------");
    printFormatted(chatResponse, 120);
    Serial.println(" ");
//get rid of any carriage returns, and other troublesom characters 
    cleanChatResponse = tidyStringForJSON(chatResponse);
    flashLED(2);
    if (TTSOpenAIAPI(cleanChatResponse)) {
      playMp3File(mp3FilePath);
    } else {
      Serial.println("Failed to process text.");
    }



  http.end(); // Free HTTPClient resources  

  
}