# SDR IP Gadget

This repository implements a simple(ish) daemon which implements a daemon, attempting to perform a high performance interface to transfer IIO buffers via Ethernet using UDP.

It was specifically developed for the Analog Adalm Pluto inspired Pluto+, however could be adapted for other devices utilizing Linux's IIO interface.

The daemon listens on two UDP ports. One for control and another for data.

The control port provides basic services to start / stop streaming on the data port.

Inbound datagrams are received and un-packaged on the data port, reassembled and queued for transmit via the DAC DMA with the help of its IIO interface.

ADC DMA transfers arriving via the IIO interface are broken into datagrams and queued for transmission over the data port.

## Building for testing

Typically this application will be built by buildroot as part of the rootfs build, however for testing it may be useful to build it outside of buildroot, while using the compiler and sysroot prepared by buildroot. Allowing the binary to be pushed to and run on the target.

```
cmake .. -DCMAKE_TOOLCHAIN_FILE=/media/user/Data1/plutosdr-fw/buildroot/output/host/share/buildroot/toolchainfile.cmake -DGENERATE_STATS=ON
```
