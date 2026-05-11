# ESP32S3-xiao-phototrap
ESP32S3 xiao phototrap project

In here, I will show you, how to create the phootrap step by step

first of all, you need to print the parts, the files are located in cad directory or [here](https://cad.onshape.com/documents/ead58119c3828b68d815a62f/w/7087faa25839fe28733057f0/e/a371d4849fa89fe6a0035750) on onshape
For the printing, I used PLA, because its the eazyest to print from, but if you need it for outside, I would consider using ASA

### Now for the parts you'll need, here is a list:

**Screws and nuts**<br/>
4x M3 x 6mm<br/>
3x M2 x 4mm<br/>
2x some self-tapping screws 2mm wide 6mm long (for mounting pir)<br/>
1x M2 nut<br/>

**Electronics**<br/>
ESP32S3 XIAO sense with camera board<br/>
OV3660 camera with cable long around 21mm (OV2640 wont work reliably since it cannot enter standby mode and will take huge amounts of current -> 20mA instead of like 0.3mA, for OV5640 not tested)<br/>
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
