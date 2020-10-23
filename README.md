# Video and audio streaming for Copiosus
Your personal, low cost, video and audio streaming infrastructure.

Run this code on a single-board computer (SBC) on your local network and display the video and audio stream in the Copiosus app.

Use it as baby phone, surveillance of your property, etc.

## How does it work?

1. Wait for a SCAN request from Copiosus
2. Respond with the current state
3. Copiosus triggers a CONNECT command
4. Send video and audio stream to Copiosus

## Prerequisites

* FFMpeg: https://github.com/FFmpeg/FFmpeg

Enable x264 encoder
```
git clone https://code.videolan.org/videolan/x264
./configure --enable-static
make
sudo make install
```

Enable OMX (hardware accelerated encoding for h264)
```
git clone git://source.ffmpeg.org/ffmpeg
./configure --extra-ldflags="-latomic" --enable-gpl --enable-libx264 --enable-omx --enable-omx-rpi
make
sudo make install
```

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

## Disable WiFi power management

Check status of Power Management:on

```sudo iwconfig wlan0```

Disable it  to prevent WiFi sleep

```sudo iwconfig wlan0 power off```

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

Mac, Raspberry Pi, camera, microphone

Camera: kuman, model number: SC15-Webcams-UK

USB mic: Gyvazla

## Troubleshooting

### No video after pressing Connect

Please verify that your password is set and correct.

### Installation of WiFi dongle

```dmesg | more```

[ 5649.299859] usb 1-1.3: USB disconnect, device number 4\
[ 5652.926139] usb 1-1.3: new high-speed USB device number 7 using dwc_otg\
[ 5653.067315] usb 1-1.3: New USB device found, idVendor=2357, idProduct=0109, bcdDevice= 2.00\
[ 5653.067336] usb 1-1.3: New USB device strings: Mfr=1, Product=2, SerialNumber=3\
[ 5653.067345] usb 1-1.3: Product: 802.11n NIC\
[ 5653.067354] usb 1-1.3: Manufacturer: Realtek\
[ 5653.067364] usb 1-1.3: SerialNumber: 00e04c000001

```lsusb```

Bus 001 Device 006: ID 045e:07f8 Microsoft Corp. Wired Keyboard 600 (model 1576)\
Bus 001 Device 005: ID 045e:00cb Microsoft Corp. Basic Optical Mouse v2.0\
Bus 001 Device 007: ID 2357:0109 TP-Link TL WN823N RTL8192EU\
Bus 001 Device 003: ID 0424:ec00 Standard Microsystems Corp. SMSC9512/9514 Fast Ethernet Adapter\
Bus 001 Device 002: ID 0424:9514 Standard Microsystems Corp. SMC9514 Hub\
Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub

https://wiki.debian.org/WiFi#USB_Devices

```
sudo apt-get install gcc-6 git build-essential
sudo apt-get install raspberrypi-kernel-headers
sudo apt-get install bc
```

```
cd /usr/src/linux-headers-4.19.75-v7+/arch
sudo ln -s arm armv7l
```

```
git clone https://github.com/jeremyb31/rtl8192eu-linux-driver.git
cd rtl8192eu-linux-driver
sudo make
sudo make install
```

```
reboot
```

* Check if module is loaded
```
lsmod
```

8192eu               1183744  0

* Remove config from /etc/network/interfaces

```
sudo apt-get install network-manager
sudo vim /etc/NetworkManager/NetworkManager.conf

# Append the following:
[device]
wifi.scan-rand-mac-address=no

/etc/init.d/network-manager restart
```

* Set Wifi Country Code: Preferences - Raspberri Pi Configuration - Localisation - Set WiFi Country...

```
sudo rasp-config - 2 - Set WiFi SSID + password
reboot
```

* Select WLAN + set password (top right corner of screen)
