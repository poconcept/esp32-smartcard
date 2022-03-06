== Smartcard on ESP32 ==

Here is a simple project to interact with smartcard with an ESP32.

It is not finished, because it's just a first idea for a project, and not used anymore. But the "proof of concept" it is working.

One can use the ESP32 as a master (to read a smartcard), or a slave (either to simulate a smartcard, or monitor a master<=>smartcard communication).

The idea is to use the two i2s interfaces, one in RX and one in TX. In slave mode, one need also to "simulate" the WS of the I2S interface (done here with a 74HC4040 to divide the clock)

Also, it seems that the 5mhz smartcard max speed is too much for the I2S port (at least, in slave_tx  mode and using the GPIO-matrix). Thus one can use the 74HC4040 to also divides the clock.

Only we do only the ATR and the T0 "interface" (not more high level things).

* T0 brief explanation *

When the smartcard is powered up, it sends a ATR (answer to request) to the reader. The protocol is a UART style protocol on 1 wire, 8bits + 1 even parity bit.

The ATR contains the maximum speed of the scmarcard, the supported protocols, some others infos and a CRC.

(The reader may sends a PPS to set the speed, and the smartcard responds the same PPS)

The default speed is 372 clock cycles per bytes.

After the ATR (and the PPS), the reader sends a commands and the smarcards answer. Commands look like:

 <cmd : 5 bytes, reader -> SC> <SW :1 or 2 bytes, SC -> reader> [ <Data (rx of tx, depending of the command)> <SW :1 or 2 bytes, SC -> reader> 


* Note *

- Some cards use 1.8V level logic, so a logic level shifter may be necessary.

- We use here I2S. Maybe an other possibility is to use the SPI bus, but I don't know if it is possible to do a continious reception with SPI on a ESP32.

* Wirings *

The smartcard IO is a serial IO, using only one wire. The RX io from the ESP32 is directly connected to it. We use a PNP transistor to put the line to 0v when 

* WS and 74HC4040 *

In slave mode, one has also to create a "fake" WS. Use a 74HC4040, put the clock on the clock (CP_) of the 74HC4040, and wire the faked WS to the "Q7" of the 4040, and wire the I2S_CLK input in the ESP32 to the divided clock "Q1". You may have to put MR (reset) to the ground.

Wirings (for the slave mode):

Power  SM CARD    ESP32    74HC4040
 GND     GND       GND       GND
 3.3V    VCC                 VCC
         RST       16      
	 CLK                 CP
	 IO       I2S_IO 
                  I2S_WS     Q7
                  I2S_CLK    Q1 
 GND                         MR


* Wiring with TX and RX *

We use the two i2s ports.

The TX port use a PNP transistor to put the line at ground when there is a transmission.


* Application 1: Monitor *

Here the esp32 listen the transactions between the reader and the SM-card, and sends everything in a TCP socket connection.


* Application 2:  SM relay between an ESP32 and PC/Linux  *
 smartard-reader <=> ESP32 <=> wifi <=> pc/linux <=> smartcard

A simple version listen commands sent by the reader, and send it to the pc, which ask the smartcard, and answer are sent back to the esp32 and the reader.

The smartcard reader on the PC/linux side is a cheap aliexpress reader,

A problem may occur: the reader has a timeout after the command is sent to the simulated smartcard. if the network messages takes too long, the smartacrd reader may think that there is a problem.



