# ESP32S3-xiao-phototrap
ESP32S3 xiao phototrap project

<p align="center">
  <img src="images/20260511_170619.jpg" width="500"/>
</p>

In here, I will show you, how to create the phootrap step by step

first of all, you need to print the parts, the files are located in cad directory or [here](https://cad.onshape.com/documents/ead58119c3828b68d815a62f/w/7087faa25839fe28733057f0/e/a371d4849fa89fe6a0035750) on onshape
For the printing, I used PLA, because its the eazyest to print from, but if you need it for outside, I would consider using ASA

# Features
There are 3 switches on it:<br/>
Maintanance<br/>
detect<br/>
debug<br/>

**Maintanance** turns on the maintanance mode, in this mode, the esp will turn on wifi and allow you to connect to it. Then acces teh wib by typing 192.168.4.1 to the browser and you're there, here, you can modify the settings (password, ssid, camera settings), view live stream, turn on the IR LED, monitor battery level and so on

It includes 2 languages -> czech and english

**detect switch** turns on the detection, so when the pir detects a movement, if this is on, the esp will start recording automatically

**Debug switch** just turns on the led below it, which indicates, if there is a movement

**error led** lights up, when there is a critical error, usually sd card not inserted or broken, but can indicate also camera error, when this happens, the esp will also create wifi and show, what caused the error on its website

### operation
When you move in front of the phototrap while detect switch is on, it will start recording and will record til you'll be moving

When its idle, it will draw aroud 0.3mA, which will last for over a year with 10Wh battery, it will wake only if there is a movement, or when the maintanance pin is high

to download the recorded videos, you can get them straight from sd card, or download them trough the web (notice: videos are encoded in a format, that is not natively supported on mobile phoes, so use VLC player app)

you can set the motion detection sensitivity trough side of the phototrap, where there are 2 potentiometers, one for sensitivity and one for how long will the detection signal last

### Millacenous
there are 4 possible back versions -> battery/no battery; gopro mountno gopro mount

# Build process
### Now for the parts you'll need, here is a list:

**Screws and nuts**<br/>
4x M3 x 6mm<br/>
3x M2 x 4mm<br/>
2x some self-tapping screws 2mm wide 6mm long (for mounting pir)<br/>
1x M2 nut<br/>

**Electronics**<br/>
ESP32S3 XIAO sense with camera board<br/>
OV3660 camera with cable long around 21mm (OV2640 wont work reliably since it cannot enter standby mode and will take huge amounts of current -> 20mA instead of like 0.3mA, for OV5640 not tested)<br/>
2.4GHz antenna
IR LED on 20mm star cool pad with ideally 850nm<br/>
LiPol battery, specificaly 954060 3000mAh 1s/3.7v <br/>
some connector for the battery for DPS, I used JST XH 2.54mm<br/>
3x DPS switch -> SS12D00G4<br/>
3mm LED with red color<br/>
3mm LED with blue/green color<br/>
PIR sensor HC-SR501<br/>
NPN tranzistor, ideally 2N2222<br/>

**Resistors**<br/>
2x 330R<br/>
1x 470R<br/>
1x 680R<br/>
2x 1K<br/>

now for the led combination, you have to use higher power resistors (1W) and use some combination between 8 and 10 ohms, so 2R2 and 6R8 is ok

some DPS board to put it on, size doesnt matter since youll have to chop it to exact size

### PIR
First of all, we'll have to modify the pir sensor, since it takes in 4.5-20v which is not ideal, so we need to remove the 3.3v regulator and a diode and replace them with some piece of cable or solder like this:


<p align="center">
  <img src="images/IMG-20260507-WA0014.jpeg" width="350"/>
  <img src="images/IMG-20260507-WA0016.jpeg" width="350"/>
</p>

now it can accept 3.3v

### IR LED
<p align="center">
  <img src="images/ir-led.webp" width="400"/>
</p>

now for the IR led, you'll have to solder some cables one side of the led, so + and -<be/>
I think you have already noticed the ir led slot, but befor you snap it in, there is also a slot for a M2 nut, so place that there first and then pop in the IR LED

### ESP
For the esp, first, split it in separate peaces (main board, expansion board, camera and antenna)

first connect antenna to the main board

then slide the camera into the slot for the camera

then while the cammera is in the slot, connect it to the expansion board

then put the main oard to the top most slot and start sliding it in, while you're sliding it in, connect it to the expansion board

now put the antenna cable to the antenna calbe slot

secure the whole thing by screwing it in the bracket behind esp (or glue it)

### Main board
this is the trickiest part, because you have to assemble the board yourself

it doesnt really matter how you solder it on to it, except for switches, leds and the battery connector, for details check the onshape cad mentioned above<br/>
and also the board size should be around 3.2 x 3.3 cm, but trim it as needed

this is the circuit I have used:
<a href="images/Phototrap circuit.pdf">
  <img src="images/circuit preview.png" width="600">
</a>

and this is how I put it together:
<p align="center">
  <img src="images/Phototrapsomtin.png" width="350"/>
  <img src="images/20260511_151046.jpg" width="350"/>
  <img src="images/20260511_150215.jpg" width="350"/>
</p>

### Finalizing
Then you just put it all together, connect all wires according ot the schema and you're ready to upload

open the .ino file in arduino ide (first you need to put hte file to a folder with same name as is the file), istall teh esp32 extension, **enable PS ram**, select the right board and select the right flash storage size (8MB)

in the program also check the pins, so they match your build (they are GPIO, not the D#):<br/>
```
static const int PIR_PIN    = 4;
static const int MAINT_PIN  = 3;
static const int IR_LED_PIN = 2;
static const int ERROR_PIN  = 5;

// Battery measurement on GPIO1 through 1/2 voltage divider
static const int BAT_ADC_PIN = 1;
```
