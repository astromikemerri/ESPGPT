# ESPGPT
Project to use the OpenAI APIs for speech-to-text and text-to-speech, to create an audio chat interface to ChatGPT, all running on an ESP32.

Hardware required:
<ul>
  <li> ESP32 microcontroller (in my case a mini one to keep the footprint small)</li>
  <li> INMP441 I2S microphone</li>
  <li> Max98357 I2S audio amplifier breakout board</li>
  <li> SD card module with SPI interface</li>
  <li> Pushbutton to activate recording </li>
  <li> LED to indicate ready to record and processing status</li>
  <li> 3W 8 ohm speaker</li>
</ul>
All wired up as follows:

<img src=ESPGPTtidy.jpg width=500>

And laid out something like this on a breadboard:

<img src=ESPGPTphoto.jpg>
