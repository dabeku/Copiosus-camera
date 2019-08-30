# Camera for Copiosus
Your personal, low cost surveillance infrastructure.

Run this code on a Raspberry Pi or Arduino controller and display the video and audio stream in the Copiosus app.

## What is this?

The idea is simple:

1. Wait for a SCAN request from Copiosus
2. Respond with the current state
3. Copiosus triggers a CONNECT command
4. Send video and audio stream to Copiosus

## Test Case

Copiosus: Mac

Camera: Raspberry Pi
