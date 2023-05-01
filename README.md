# hp config for linux
Configure HP printers on your linux desktop via USB

Some HP printers use HTTP over USB to provide a universal settings panel on different platforms.
On windows, the driver comes with an application that wraps a web browser and allows you to navigate the control panel of the printer via USB

I could not find this on hplip, so I implemented the same functionality myself.
It will open an http server you can connect to and set up your printer.

It is very rough, I wanted to finish asap, since I just needed to configure my printer's wifi settings.
I did not test this on other printers than my HP LaserJet M15, the USB descriptors are hardcoded, and the build script is just made for me.


If you are interested, feel free to test it, run it on your system or send an issue or PR my way.

# build

It depends on libusb-1.0, libfmt a c++ compiler that can churn c++20 and gnu make.
The makefile is pretty basic.

    make clean && make

# Use

Connect your printer via USB, check the vendor and product IDs in lsusb and see if they match the VID and PID in the code (03f0:be2a). If they don't your printer may be incompatible, but it may be worth a test.

Run the tool
    
    ./usb_http
    
and you will see some output, ending in:

    Now listening on http://localhost:8818/

Ctrl+click the link, or go to it in your web browser (Only Firefox 109 tested).
