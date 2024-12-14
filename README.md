# Secure E-Voting System

**Authors**: Josh Bardwick, Noah Hathout, Cole Knutsen, Trieu Tran  
**Date**: 2024-11-07  

## Overview

This project implements a secure and distributed electronic voting system using fob devices that communicate via Near Field Communication (NFC) simulated through Infrared (IR). The system dynamically handles device connections, disconnections, and failovers, ensuring secure and reliable voting. A coordinator device is elected among the fobs to manage the voting process, authenticate voters, and transmit data securely to a central database hosted on a Raspberry Pi. A web interface provides real-time updates on voting results. This project is currently a proof on concept on the ESP32, using the ESP-IDF framework.

---

## Features

### Voter Authentication and Vote Casting
- Utilizes NFC (via IR) to authenticate voters and transmit their unique IDs and votes to the system.  

### Dynamic Coordinator Election and Failover
- A distributed system dynamically elects a coordinator using leader election algorithms.  
- Automatically elects a new coordinator upon failure of the current coordinator without interrupting the voting process.  

### Secure Vote Transmission
- Votes and voter IDs are transmitted from fobs to the coordinator, which securely relays them to the database server.  
- The coordinator sends confirmation back to the voting device to ensure successful vote transmission.  

### Centralized Data Logging and Web Interface
- A Raspberry Pi server logs each vote in a TingoDB database.  
- A web interface provides real-time polling results and allows administrators to reset the database with a button click.  

### Device Status Indication
- LEDs indicate device states:  
  - **Red**: Non-leader  
  - **Blue**: Leader  
  - **Green**: Timeout  

---

## System Design

### Core Components
1. **Hardware Integration**
   - ESP32 devices simulate NFC communication via IR.  
   - Buttons allow voters to initiate the voting process.  

2. **Network Communication**
   - Devices connect via Wi-Fi, forming a distributed network.  
   - UDP messages handle communication between devices and the server.  

3. **Coordinator Election**
   - Leader election algorithms ensure seamless selection of a coordinator.  
   - Failover mechanisms ensure continuous operation during coordinator failure.  

4. **Database and Web Server**
   - The Raspberry Pi server runs Node.js and TingoDB, logging votes and providing a web interface for real-time results.  

5. **Concurrency and Synchronization**
   - FreeRTOS tasks manage IR communication, button presses, network communication, and leader election.  
   - Mutexes protect shared data across tasks.  

---

### Task Breakdown

1. **Initialization (`app_main`)**  
   - Sets up mutexes, semaphores, UART, MCPWM for IR communication, GPIOs for buttons and LEDs, Wi-Fi connectivity, and timers for leader election and keep-alive signals.  

2. **FreeRTOS Tasks**  
   - **Button Task** (`button_task`)  
     - Detects button presses and determines the voted ID. Initiates vote transmission when the timer expires.  

   - **IR Receive Task** (`ir_rx_task`)  
     - Listens for incoming IR messages. Handles coordinator signals and starts leader election if needed.  

   - **Leader Election Task** (`bully_algorithm_task`)  
     - Participates in the Bully Algorithm for leader election and handles related messages (`MSG_ELECTION`, `MSG_ALIVE`, `MSG_VICTORY`, `MSG_KEEP_ALIVE`).  

   - **UDP Receive Task** (`recv_task`)  
     - Listens for UDP messages and processes votes, leader updates, and keep-alive signals.  

   - **ID Task** (`id_task`)  
     - Blinks the onboard LED to display the device's ID.  

   - **IR Transmitting Task** (`continuous_ir_transmit_task`)  
     - If the device is the coordinator, continuously sends coordinator signals via IR.  

3. **Vote Transmission**
   - **Non-Coordinator Device**: Sends `MSG_VOTE` to the coordinator via UDP.  
   - **Coordinator Device**:  
     - Processes `MSG_VOTE`, updates vote counts, and sends an acknowledgment to the voting device.  
     - Forwards vote data to the server via UDP.  

4. **Server (`store_votes.js`)**
   - Listens for incoming votes via UDP.  
   - Logs votes to the database at `mydb/votes`.  
   - Hosts a web interface at `http://192.168.1.103:8080` for viewing real-time polling results.  

---

## Visuals

### Circuit Diagram
![Circuit Diagram](Quest4-circuit-diagram.jpeg)

---

## Potential Improvements

1. **Hardware Interrupts**  
   - Replace polling-based IR reception with hardware interrupts for better real-time performance.  

2. **Network Optimization**  
   - Enhance UDP message reliability to minimize vote transmission delays.  

3. **Data Integrity**  
   - Add robust error-checking for IR and network transmissions.  

4. **Security Enhancements**  
   - Implement encryption for IR and UDP data transmission to safeguard voting data.  

---

## Results

The system successfully implements a secure and distributed voting process. Each fob connects to the network, participates in coordinator elections, and submits authenticated votes for "Red," "Blue," or "Green." The elected coordinator receives, logs, and transmits votes to the central database. LED indicators provide device status, while the web server displays real-time vote tallies and coordinator updates.  

---

## Challenges

1. **Distributed Coordination**  
   - Ensuring seamless leader election and failover mechanisms required careful synchronization.  

2. **IR Communication Reliability**  
   - Calibrating signal thresholds and timing to ensure accurate IR transmissions in noisy environments.  

3. **Security and Data Integrity**  
   - Designing secure and reliable communication protocols while maintaining system performance.  

---

## Demonstrations

- [Video Demo: Report](https://drive.google.com/file/d/1l6wGfmdgtvITa7rTuvHjnD1I_g9CTXzG/view?usp=sharing)  
- [Video Demo: Design](https://drive.google.com/file/d/14ioolii2PttAG8Jn80Jt-KU8r46mR65U/view?usp=sharing)  
