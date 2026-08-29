## Project Background

This project documents our effort to observe and capture **CAN bus arbitration** at the physical signal level.

**Initial Attempts (BeagleBone Black + ESP32-D):**
We began by establishing communication between a BeagleBone Black and an ESP32-D, defining delay parameters to make both boards transmit simultaneously. However, none of these trials successfully produced observable arbitration. The architectural differences between the two boards resulted in highly unstable jitter, making it clear that using identical hardware platforms would be a more reliable approach.

**Intermediate Attempt (ESP32-C6 to ESP32-C6):**
We then established communication between two identical ESP32-C6 boards, which successfully produced arbitration. Software-level logs confirmed that the node transmitting the higher-ID frame correctly backed off during bus contention. However, this behavior could not be verified on a logic analyzer, since the MCP2515 CAN controller and its transceiver were integrated into a single, non-accessible module with no available test points for signal probing.

**Final Solution (SN65 Transceiver + Separate CAN Controller):**
To overcome this limitation, we replaced the integrated module with a separate SN65 transceiver and CAN controller setup. This configuration allowed us to both observe arbitration in the terminal output and, more importantly, capture it directly as a physical signal on the logic analyzer.

## Conclusion

After an extended period of iterative testing and hardware refinement, we successfully completed this project. We believe the process — including the failed attempts and the reasoning behind each hardware transition — provides valuable insight for others working on CAN bus arbitration, and we hope this repository serves as a useful reference for the community.

