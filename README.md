# Camera for Copiosus
Your personal, low cost, video and audio streaming infrastructure.

Run this code on a Raspberry Pi or Arduino controller on your local network and display the video and audio stream in the Copiosus app.

Use it as baby phone, surveillance of your property, etc.

## How does it work?

1. Wait for a SCAN request from Copiosus
2. Respond with the current state
3. Copiosus triggers a CONNECT command
4. Send video and audio stream to Copiosus

## Prerequisites

* FFMpeg: https://github.com/FFmpeg/FFmpeg
* SDL v2.0: https://www.libsdl.org/

## Instructions

```make```

```./cop_sender```

## Run

```./cop_sender -platform=mac|linux|win -cmd=start|list -cam=[name] -mic=[name] -pwd=[password]```

Example:

```./cop_sender -platform=linux -cmd=start -cam=/dev/video0 -mic=hw:1 -pwd="This-is-Awesome"```

## Commands

List connected devices:

* Windows: ```ffmpeg -list_devices true -f dshow -i dummy```
* Mac: ```ffmpeg -f avfoundation -list_devices true -i ""```
* Linux: ```v4l2-ctl --list-devices```

## Run when starting

```sudo vim /etc/rc.local```

Add the following line before exit 0:

```sudo /home/pi/Documents/Copiosus-camera/cop_sender -platform=linux -cmd=start -cam=/dev/video0 -mic=hw:1 -pwd="this-is-a-password" >> /home/pi/Documents/log.txt &```

## Disable all LEDs

```sudo vim /boot/config.txt```

Add the following line at the end of the file:

```
disable_camera_led=1
```

```sudo vim /etc/rc.local```

Add the following line before exit 0:

```
sudo sh -c 'echo 0 > /sys/class/leds/led0/brightness'
sudo sh -c 'echo 0 > /sys/class/leds/led1/brightness'
```

## Test Case

Copiosus: Mac

Raspberry Pi

Camera: kuman, model number: SC15-Webcams-UK

USB mic: Gyvazla
