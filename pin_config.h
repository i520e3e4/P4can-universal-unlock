#pragma once
// Waveshare ESP32-P4-WIFI6, P4 revision 3.2, C6 ESP-Hosted over SDIO.
//
// HARDWARE ADAPTATION (2026-11-09) for the "ESP32P4 WIFI6 3CAN" expansion board.
// Board silkscreen / seller pinout:
//   board CAN1 = GPIO2(TX) / GPIO3(RX)  -> X179 13/14 CHASSIS (CH)
//   board CAN2 = GPIO4(TX) / GPIO5(RX)  -> X179  9/10 BODY / VEHICLE (VEH)
//   board CAN3 = GPIO26(TX)/ GPIO27(RX) -> X179  2/3 PARTY / PT
// Upstream firmware used 20/21=BODY, 22/23=CHASSIS, 26/27=PARTY with BODY on
// board CAN1. Because the expansion board swaps CAN1/CAN2 relative to X179,
// the macros below are mapped BY BUS NAME, not by board silkscreen number, so
// the logical BODY/CHASSIS/PARTY decoders still sit on the correct vehicle bus.
// If your harness instead wires CAN1->9/10 BODY and CAN2->13/14 CHASSIS,
// swap the two TX/RX pairs below (BODY=2/3, CHASSIS=4/5).
// GPIO2-5 are free HP GPIOs (TOUCH0-3 / LP_GPIO2-5); keep the default
// USB_SERIAL_JTAG routing so GPIO2-5 are never claimed by external JTAG.
#define CAN_BODY_TX 4
#define CAN_BODY_RX 5
#define CAN_CHASSIS_TX 2
#define CAN_CHASSIS_RX 3
#define CAN_PARTY_TX 26
#define CAN_PARTY_RX 27
#define HOSTED_CLK 18
#define HOSTED_CMD 19
#define HOSTED_D0 14
#define HOSTED_D1 15
#define HOSTED_D2 16
#define HOSTED_D3 17
#define HOSTED_RESET 54
#define HOSTED_C6_BOOT 6
