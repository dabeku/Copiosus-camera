# Camera for Copiosus
Your personal, low cost surveillance infrastructure.

Run this code on a Raspberry Pi or Arduino controller and display the video and audio stream in the Copiosus app.

## What is this?

The idea is simple:

1. Wait for a SCAN request from Copiosus
2. Respond with the current state
3. Copiosus triggers a CONNECT command
4. Send video and audio stream to Copiosus

## Instructions

```make```

```./cop_sender```

## Run

```./cop_sender -platform=linux -cmd=start -cam=/dev/video0 -pwd="This-is-Awesome"```

## Commands

List connected devices:

* Windows: ```ffmpeg -list_devices true -f dshow -i dummy```
* Mac: ```ffmpeg -f avfoundation -list_devices true -i ""```
* Linux: ```v4l2-ctl --list-devices```

## Run when starting

```sudo vim /etc/rc.local```

Add the following line before exit 0:

```sudo /home/pi/Documents/Copiosus-camera/cop_sender -platform=linux -cmd=start -cam=/dev/video0 > /home/pi/Documents/log.txt &```

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

Camera: Raspberry Pi
