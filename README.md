# ESP_SocketServer
Example of using ESP32 as a socket server.

This code is based on a number of examples in the Espressif ESP32 code set. It demonstrates a number of topics:
1. Use of both Cores in the ESP32
2. Use of Threads
3. Communication across Threads using xNotifyThread
4. Configuration and use of WiFi
5. Transmission of data using Sockets once WiFi connected.
6. Generation of a signal on GPIO pins 18 & 19 and reading generated signal on GPIO pins 4 & 5

To fully execute you will need to connect GPIO pin 18 to pin 04 and in 19 to pin 05.

Each signal rise and fall read causes a signal count to be incremented. 
If a socket client has connected to the server, then each increment is stored in a buffer. When the buffer is full it is transmitted to the client. The data is transmitted in ACII85 encoding, (which transmits 5 ASCII bytes per 4 bytes of data, but ensures each byte is within the printable ASCII character range). The client will need to decode the data to retreive the actual numbers, which, of course, would be incremental assuming no data loss.
