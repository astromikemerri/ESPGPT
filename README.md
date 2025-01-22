# ESPGPT
Project to use the OpenAI APIs for speech-to-text and text-to-speech, to create an audio chat interface to ChatGPT, all running on an ESP32 connected to your WIFI network.

Hardware required:
<ul>
  <li> ESP32 microcontroller (in my case a mini one to keep the footprint small)</li>
  <li> INMP441 I2S microphone</li>
  <li> Max98357 I2S audio amplifier breakout board</li>
  <li> SD card module with SPI interface (with an SD card in it)</li>
  <li> Pushbutton to activate recording </li>
  <li> LED to indicate ready to record and processing status</li>
  <li> 220 owm resistor to limit power to LED </li>
  <li> 3W 8 ohm speaker</li>
</ul>
All wired up as follows:

<img src=ESPGPTtidy.jpg width=500>

And laid out something like this on a breadboard:

<img src=ESPGPTphoto.jpg width=500> 

The assigned pins used on the ESP32 are all spelled out as #defines in <A href=ESPGPTcode.ino>the code</a>, but there is no reason you shouldn't reassign them if you wish.

To get this to work, you need to have an <A href= https://platform.openai.com/account/>OpenAI account</a>, to set up a form of payment in the "Billing" section and prepay for some API credits (NB. NOT FREE! It shouldn't cost much to run this small project -- all the development has only cost me around $10. And you can set limits to ensure that you do not spend more than you want).  You can then create an API key <A href=https://platform.openai.com/api-keys>here</a>, which you will need to paste into <A href=ESPGPTcode.ino>the code</a>, along with your WIFI credentials.

The libraries in the code are all fairly standard and easy to find and install; the audio library I used is https://github.com/earlephilhower/ESP8266Audio.

If all goes to plan, you should end up being able to hold a conversation <A href=ESPGPT.mov>like this</a>.

You can ring the changes on which spoken voice you use, which ChatGPT model, its "temperature" (ie, how random its answers are) and the NCONV parameter, which specifies how long a conversation the code remembers.

Have fun!
