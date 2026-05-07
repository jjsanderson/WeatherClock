# WeatherClock

Mostly vibe-coded worryware for a clock-like devices which fetches weather data from OpenWeatherMap and indicates current (near future?) conditions using a 360° feedback servo. MCU is a Wemos D1 Mini (ESP8266-based) and the servo is a Parallax Feedback 360° Servo. The code is written in C++ using the Arduino framework and PlatformIO for development.

## Note to Amelia

If you fork this repository you'll get your own copy of the code; pull that repository down to your local machine (use Github Desktop), then open the project in VS Code. You might like to start by building some test code to move the servo correctly.

Don't forget to copy `include/config.h.template` to `include/config.h` and fill in your own WiFi credentials and OpenWeatherMap API key. You can get an API key by signing up for a free account at https://openweathermap.org/api.

## Note to anyone else

This is not the code you're looking for. It's a tutorial/learning project we're working on, and likely not suitable for anyone else to use as an example.
