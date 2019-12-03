# Transmiting Datas via Sockets

 FreeRTOS task running under the ESPRESSIF ide to simulate reading data from and external source and sending to a client.
 The Program:
 1. Connects to WiFi: (SSID and PASSWORD are set-up in the monitor.)
 2. Outputs IP address to Monitor so that it is known.
 3. Opens a socket on Port 23 (I know Port 23 is usally Telnet, but any port will suffice), and waits for a client to connect.
  4. A Parallel thread begins generating integer data points. Each data point, once generated, is sent to a data compilation thread.
  5. The Data compilation thread uses a double buffering technique to receive data, store in buffer and when buffer is full send it to a Data Transmission thread on the WiFi core.
  6. The Data Transmission thread, checks if a client is connected. If not, the dta is discarded. If a client is connected, the data is converted to ACII85 byters (ensures all bytes are printable), and transmits the data to the client.


