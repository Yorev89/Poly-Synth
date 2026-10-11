// secrets.example.h - template for the web controller's private settings.
//
// Copy this file to "secrets.h" in the same folder as esp32_WEB_Controller.ino
// and put your own values in it. secrets.h is listed in .gitignore and must
// NOT be uploaded to GitHub - that is the point of keeping it separate.
//
// If secrets.h is missing, the sketch builds with the values below. They are
// PUBLIC (anyone can read them here), so change them for any synth you use.
#pragma once

// Wi-Fi access point created by the synth
#define WIFI_SSID      "STM32_Synth"
#define WIFI_PASSWORD  "change-me-please"   // at least 8 characters

// Volume passcode used until one is set from the web page (4 digits).
// Once changed on the page, the saved passcode is used instead.
#define DEFAULT_VOLUME_PASSCODE "1234"
