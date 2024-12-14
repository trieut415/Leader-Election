# Secure E-Voting System

**Date**: November 7, 2024  

---

## Overview

The **Secure E-Voting System** is a distributed electronic voting platform designed to ensure secure, reliable, and dynamic voting processes. Utilizing Near Field Communication (NFC) simulated via Infrared (IR), the system enables voter authentication, vote transmission, and real-time result reporting. A dynamically elected coordinator manages vote collection and securely logs data to a central database hosted on a Raspberry Pi. The project demonstrates resilience through automatic leader election and failover mechanisms, ensuring uninterrupted operation in case of coordinator failure.

---

## Key Features

### 1. Voter Authentication and Secure Vote Transmission
- NFC (via IR) authenticates voters and transmits unique voter IDs and their votes securely to the system.
- The coordinator relays votes to a database server and confirms successful submissions to the originating fob.

### 2. Dynamic Coordinator Election and Failover
- A distributed system elects a coordinator using a leader election algorithm.
- Automatic failover ensures a new coordinator takes over seamlessly in case of failure.

### 3. Centralized Data Logging and Web Interface
- Votes are logged in a TingoDB database on a Raspberry Pi server.
- A real-time web interface provides:
  - Poll results with timestamps and voter IDs.
  - Live vote tallies for Red, Blue, and Green.
  - Admin functionality to reset the database.

### 4. Device Status Indication
- LED Indicators:
  - **Red**: Non-leader state.
  - **Blue**: Leader state.
  - **Green**: Timeout or error state.

### 5. Distributed Resilience
- Fobs operate on a distributed network, ensuring reliability and minimal downtime during coordinator transitions.

---

## System Design

### Core Components
1. **Fob Devices**
   - ESP32-based devices simulate NFC with IR for vote transmission.
   - Buttons allow voters to initiate the voting process.
   - LEDs indicate device state and voting confirmation.

2. **Network Communication**
   - Fobs communicate via Wi-Fi using UDP for vote transmission and leader election.

3. **Coordinator Election**
   - Implements the Bully Algorithm for dynamic leader election.
   - Manages keep-alive signals to detect and recover from coordinator failures.

4. **Database and Web Server**
   - Raspberry Pi runs Node.js and TingoDB for data storage.
   - Hosts a web interface for real-time polling results and admin actions.

---

### Task Breakdown

#### 1. Initialization
- Sets up Wi-Fi, IR communication, buttons, LEDs, and FreeRTOS tasks for concurrency.

#### 2. FreeRTOS Tasks
- **IR Transmit/Receive Tasks**:
  - Handle NFC (IR) communication for vote and leader election signals.
- **Vote Task**:
  - Processes and transmits voter ID and vote to the coordinator.
- **Leader Election Task**:
  - Implements leader election using messages like `MSG_ELECTION` and `MSG_VICTORY`.
- **Coordinator Task**:
  - Receives and logs votes, updates the web server, and sends confirmations.

#### 3. Web Server
- Logs votes with timestamps and IDs.
- Displays real-time vote tallies for each option (Red, Blue, Green).
- Allows administrators to reset the database via a web interface.

---

## Results and Achievements

### Achievements
- Successfully implemented secure voting with real-time authentication and result reporting.
- Demonstrated resilience with dynamic coordinator election and seamless failover.
- Provided a functional web interface for real-time vote visualization and database management.

### Challenges
1. **Leader Election**:
   - Ensuring smooth transitions and consistent state synchronization during failovers.
2. **IR Signal Calibration**:
   - Achieving reliable communication in noisy or reflective environments.
3. **Security**:
   - Designing secure communication protocols while maintaining performance.

---

## Demonstrations

- [Video Demo: System Overview](https://drive.google.com/file/d/1l6wGfmdgtvITa7rTuvHjnD1I_g9CTXzG/view?usp=sharing)  
- [Video Demo: Design Details](https://drive.google.com/file/d/14ioolii2PttAG8Jn80Jt-KU8r46mR65U/view?usp=sharing)  

---

## Investigative Questions

### 1. Potential Hacks and Mitigations
#### A. **Man-in-the-Middle Attack**  
   - **Threat**: An attacker intercepts IR or UDP communication to alter or replay votes.  
   - **Mitigation**: Use encryption for IR and UDP data transmission. Implement message authentication codes (MACs) to validate message integrity.

#### B. **Coordinator Compromise**  
   - **Threat**: A malicious coordinator manipulates votes or fails to transmit them.  
   - **Mitigation**: Regular keep-alive signals and failover mechanisms ensure compromised coordinators are replaced automatically.

#### C. **Replay Attacks**  
   - **Threat**: An attacker resends intercepted votes to skew results.  
   - **Mitigation**: Include timestamps and unique message IDs in transmissions. Discard duplicate messages at the server.

---

## Potential Improvements

1. **Hardware Enhancements**:
   - Replace polling-based IR reception with hardware interrupts for real-time responsiveness.

2. **Network Optimization**:
   - Implement TCP instead of UDP for more reliable transmissions, or use acknowledgment-based mechanisms.

3. **Scalability**:
   - Optimize leader election for larger distributed systems.

4. **User Experience**:
   - Add visual feedback on the web interface for active coordinator status and failover events.

5. **Enhanced Security**:
   - Incorporate end-to-end encryption for all communications and implement secure boot for firmware protection.

---

## Conclusion

The **Secure E-Voting System** demonstrates the feasibility of a secure and distributed voting process. By integrating NFC (via IR), dynamic leader election, and real-time data visualization, the system ensures robust and transparent operations. With further refinements in security and scalability, this platform could serve as a model for future electronic voting systems.
