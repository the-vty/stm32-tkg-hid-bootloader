/*
__ __| |           |  /_) |     ___|             |           |
  |   __ \   _ \  ' /  | |  / |      _ \ __ \   |      _` | __ \   __|
  |   | | |  __/  . \  |   <  |   |  __/ |   |  |     (   | |   |\__ \
 _|  _| |_|\___| _|\_\_|_|\_\\____|\___|_|  _| _____|\__,_|_.__/ ____/
  -----------------------------------------------------------------------------
* TKG HID Bootloader - USB HID bootloader for STM32F10X family MCU
  Copyright (C) 2019 by The KikGen labs.
  LICENCE CREATIVE COMMONS - Attribution-NonCommercial 4.0 International (CC BY-NC 4.0)

  This file is part of the TKG-HID Bootloader distribution
  https://github.com/TheKikGen/stm32-tkg-hid-bootloader
  Copyright (c) 2020 TheKikGen Labs team.

  Initial inspiration and parts of this project comes from the STM32 HID Bootloader,
  from Bruno Freitas and Vassilis Serasidis <avrsite@yahoo.gr>,
	Modified January 2019, 	by Michel Stempin <michel.stempin@wanadoo.fr>

  -----------------------------------------------------------------------------
  Disclaimer.

  This work is licensed under the Creative Commons Attribution-NonCommercial 4.0 International License.
  To view a copy of this license, visit http://creativecommons.org/licenses/by-nc/4.0/
  or send a letter to Creative Commons, PO Box 1866, Mountain View, CA 94042, USA.

  NON COMMERCIAL - PERSONAL USE ONLY : You may not use the material for pure
  commercial closed code solution without the licensor permission.

  You are free to copy and redistribute the material in any medium or format,
  adapt, transform, and build upon the material.

  You must give appropriate credit, a link to the github site
  https://github.com/TheKikGen/stm32-tkg-hid-bootloader , provide a link to the license,
  and indicate if changes were made. You may do so in any reasonable manner,
  but not in any way that suggests the licensor endorses you or your use.

  You may not apply legal terms or technological measures that legally restrict
  others from doing anything the license permits.

  You do not have to comply with the license for elements of the material
  in the public domain or where your use is permitted by an applicable exception
  or limitation.

  No warranties are given. The license may not give you all of the permissions
  necessary for your intended use.  This program is distributed in the hope that
  it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

*/

#include <stm32f10x.h>
#include <string.h>
#include <stdbool.h>
#include "config.h"
#include "common.h"
#include "flash.h"
#include "usb.h"
#include "hid.h"

#define VID 0x09,0x12		         // idVendor 0x1209
#define PID 0xBA,0xBE		         // idProduct 0xBEFA
#define FIRMWARE_VERSION 0x00,3	 // Firmware version.

/* This should be <= MAX_EP_NUM defined in usb.h */
#define EP_NUM 			2

/* Maximum packet size. Must be a multiple of 1024 (128 USB packets)  */
#define MAX_PACKET_SIZE		8

// Bootloader command sign
static const uint8_t CMD_SIGN[MAX_PACKET_SIZE] = {'B', 'T', 'L', 'D', 'C', 'M', 'D', 2};

/* Buffer table offsset in PMA memory */
#define BTABLE_OFFSET		(0x00)

/* EP0  */
/* RX/TX buffer base address */
#define ENDP0_RXADDR		(0x18)
#define ENDP0_TXADDR		(0x58)

/* EP1  */
/* TX buffer base address */
#define ENDP1_TXADDR		(0x100)

/* Bootloader flashing state  */
volatile BootloaderState_t BootloaderState ;

/* Simulation mode. Do all but flashing pages */
volatile bool Simul;

/* page size  of flash memory */
static volatile uint16_t PageSize;

/* Flash page buffer */
static uint8_t PageData[2048];

/* USB buffer */
static uint8_t USB_Buffer[MAX_BUFFER_SIZE];

/* Current page number (starts right after bootloader's end) */
static volatile uint8_t CurrentPage;

// First page of user code (after the bootloader)
static volatile uint8_t FirstUserPage;

// If a page offset is passed with the start command
static volatile uint8_t PageOffset;

/* Byte offset in flash page */
static volatile uint16_t CurrentPageBytesOffset;

/* Bootloader CMD 0 padding */
static volatile uint16_t CurrentPacketOffset;

/* USB Descriptors */
static const uint8_t USB_DeviceDescriptor[] = {
	0x12,			// bLength
	0x01,			// bDescriptorType (Device)
	0x10, 0x01,		// bcdUSB 1.10
	0x00,			// bDeviceClass (Use class information in the Interface Descriptors)
	0x00,			// bDeviceSubClass
	0x00,			// bDeviceProtocol
	MAX_PACKET_SIZE,	// bMaxPacketSize0 8
	VID,		  // idVendor (2 bytes)
	PID,		  // idProduct (2 bytes)
	FIRMWARE_VERSION,		// bcdDevice . Version.
	0x01,			// iManufacturer (String Index)
	0x02,			// iProduct (String Index)
	0x00,			// iSerialNumber (String Index)
	0x01 			// bNumConfigurations 1
};

static const uint8_t USB_ConfigurationDescriptor[] = {
	0x09,			// bLength
	0x02,			// bDescriptorType (Configuration)
	0x22, 0x00,		// wTotalLength 34
	0x01,			// bNumInterfaces 1
	0x01,			// bConfigurationValue
	0x00,			// iConfiguration (String Index)
	0xC0,			// bmAttributes Self Powered
	0x32,			// bMaxPower 100mA

	0x09,			// bLength
	0x04,			// bDescriptorType (Interface)
	0x00,			// bInterfaceNumber 0
	0x00,			// bAlternateSetting
	0x01,			// bNumEndpoints 1
	0x03,			// bInterfaceClass
	0x00,			// bInterfaceSubClass
	0x00,			// bInterfaceProtocol
	0x00,			// iInterface (String Index)

	0x09,			// bLength
	0x21,			// bDescriptorType (HID)
	0x11, 0x01,		// bcdHID 1.11
	0x00,			// bCountryCode
	0x01,			// bNumDescriptors
	0x22,			// bDescriptorType[0] (HID)
	0x20, 0x00,		// wDescriptorLength[0] 32

	0x07,			// bLength
	0x05,			// bDescriptorType (Endpoint)
	0x81,			// bEndpointAddress (IN/D2H)
	0x03,			// bmAttributes (Interrupt)
	MAX_PACKET_SIZE, 0x00,	// wMaxPacketSize 8
	0x05 			// bInterval 5 (2^(5-1)=16 micro-frames)
};

static const uint8_t USB_ReportDescriptor[32] = {
	0x06, 0x00, 0xFF,	// Usage Page (Vendor Defined 0xFF00)
	0x09, 0x01,		// Usage (0x01)
	0xA1, 0x01,		// Collection (Application)
	0x09, 0x02,		// 	Usage (0x02)
	0x15, 0x00,		// 	Logical Minimum (0)
	0x25, 0xFF,		// 	Logical Maximum (255)
	0x75, 0x08,		// 	Report Size (8)
	0x95, 0x08,		// 	Report Count (8)
	0x81, 0x02,		// 	Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
	0x09, 0x03,		// 	Usage (0x03)
	0x15, 0x00,		// 	Logical Minimum (0)
	0x25, 0xFF,		// 	Logical Maximum (255)
	0x75, 0x08,		// 	Report Size (8)
	0x95, 0x40,		// 	Report Count (64)
	0x91, 0x02,		// 	Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
	0xC0 			// End Collection
};


static void HIDUSB_GetDescriptor(USB_SetupPacket *setup_packet)
{
	uint16_t *descriptor = 0;
	uint16_t length = 0;

	switch (setup_packet->wValue.H) {
	case USB_DEVICE_DESC_TYPE:
		descriptor =  (uint16_t *) USB_DeviceDescriptor;
		length = sizeof (USB_DeviceDescriptor);
		break;

	case USB_CFG_DESC_TYPE:
		descriptor = (uint16_t *) USB_ConfigurationDescriptor;
		length =  sizeof (USB_ConfigurationDescriptor);
		break;

	case USB_REPORT_DESC_TYPE:
		descriptor = (uint16_t *) USB_ReportDescriptor;
		length =  sizeof (USB_ReportDescriptor);
		break;

	default:
		break;
	}
	if (length > setup_packet->wLength) {
		length = setup_packet->wLength;
	}
	USB_SendData(0, descriptor, length);
}

// Return a command code if a valid bootloader command
static BTLCommand_t HIDUSB_PacketIsCommand(void) {
	uint8_t i;
	for (i = 0; i < sizeof (CMD_SIGN)-1 ; i++) {
		if (USB_Buffer[i] != CMD_SIGN[i]) return CMD_NOT_A_CMD;
 	}
	uint8_t cmd = USB_Buffer[i]; // Save command Id
	i += (MAX_PACKET_SIZE + 1);   // Pass the eventual data packet
	for ( ; i < MAX_BUFFER_SIZE ; i++) {
		if ( USB_Buffer[i] ) return CMD_NOT_A_CMD;
	}
	return cmd;
}
////////////////////////////////////////////////////////////////////////////////
// USB ISR !!!  DATA HANDLER
////////////////////////////////////////////////////////////////////////////////
// We receive a MAX_PACKET_SIZE data packet here
static void HIDUSB_HandleData(uint8_t *data)
{
	bool writePage = false;

	memcpy(USB_Buffer + CurrentPacketOffset, data, MAX_PACKET_SIZE);
	CurrentPacketOffset += MAX_PACKET_SIZE;

	if ( CurrentPacketOffset < MAX_BUFFER_SIZE ) return;
	CurrentPacketOffset = 0;
	uint8_t cmd  = HIDUSB_PacketIsCommand();

	if ( BootloaderState == BTL_WAITING ) {

		// Send some info about the MCU to the CLI
		if ( cmd == CMD_INFO ) {
				SLEEP_M(100);
				uint8_t commandData[MAX_PACKET_SIZE];
				memset(commandData, 0, MAX_PACKET_SIZE);
				uint16_t m = MCU_REPORT_FLASH_MEMORY;
				uint32_t a = FLASH_BASE_ADDR + CurrentPage * PageSize ;
				memcpy(commandData,(void*)&m,sizeof(uint16_t));
				memcpy((void*)(commandData + sizeof(uint16_t)), (void*)&a,sizeof(uint32_t) );
				commandData[7] = CMD_INFO ;
				USB_SendData(ENDP1, (uint16_t *)commandData,MAX_PACKET_SIZE);
				return;
		}

		// Simulation mode with an ACK reply to secure the mode
		if ( cmd == CMD_SIMUL ) {
				Simul = true;
				USB_SendData(ENDP1, (uint16_t *) CMD_SIGN, MAX_PACKET_SIZE);
				return;
		}

		// It is possible to pass a page offset to the CLI to change the flash address.
		// Default address is 0x FLASH ADRESS +  0x1000  (2 HD pages/ 4 MD pages)
		// For example, to Flash at FLASH ADRESS + 0x2000 (4 HD pages / 8 MD pages), you need so
		// to add an offset of 2 HD pages or 4 MD pages. The bootloader will make
		// a copy of the firmware first page at the usual flash address to allow the
		// correct loading of vector table, whatever the offset is.
		if ( cmd == CMD_PAGE_OFFSET ) {
				uint8_t offset = USB_Buffer[MAX_PACKET_SIZE];
				if ( offset != 0 ) {
					uint32_t a = FLASH_BASE_ADDR + (CurrentPage + offset) * PageSize;
					if (IS_VALID_FLASH_ADDRESS(a) ) {
							PageOffset = offset;
							CurrentPage += PageOffset;
					}
				}
				return;
		}

	}

	if ( cmd == CMD_START ) {
			BootloaderState = BTL_STARTED;
			return;
	}

	if ( cmd == CMD_END ) {
		if (CurrentPageBytesOffset) writePage = true;
		BootloaderState = BTL_END;
	}
	else if (BootloaderState == BTL_STARTED) {
		memcpy(PageData + CurrentPageBytesOffset, USB_Buffer, MAX_BUFFER_SIZE);
		CurrentPageBytesOffset += MAX_BUFFER_SIZE;
		if (CurrentPageBytesOffset == PageSize ) writePage = true;
	}

	// Write a page to the flash memory
	if ( writePage ) {
			LED1_ON;
			if (Simul) SLEEP_M(100);
			// if an offset was required, copy the MSP and the vector table
			// to the first user page
			if ( PageOffset > 0 ) {
				if (!Simul) FLASH_WritePage((uint16_t *)(FLASH_BASE_ADDR + FirstUserPage * PageSize),
				 				(uint16_t *) PageData, 8 / 2);
				SLEEP_M(1);
				PageOffset = 0;
			}

			// Flash the page at the required address
			if (!Simul) FLASH_WritePage((uint16_t *)(FLASH_BASE_ADDR + CurrentPage * PageSize),
			 				(uint16_t *) PageData, CurrentPageBytesOffset / 2);
			CurrentPage++;
			CurrentPageBytesOffset = 0;
			writePage = false;
			LED1_OFF;
	}

	// Send block ACK (at the end of all activities)
	if ( CurrentPageBytesOffset % 1024 == 0) USB_SendData(ENDP1, (uint16_t *) CMD_SIGN, MAX_PACKET_SIZE);

}

// USB ISR RESET HANDLER
void USB_Reset(void)
{
	BootloaderState     = BTL_WAITING ;
	Simul = false;

	// Set values depending on device density
	if ( IS_HIGH_DENSITY ) {
		PageSize = 2048;
		FirstUserPage	= BOOTLOADER_PAGE_SIZE_H ;
	} else {
		PageSize = 1024;
		FirstUserPage = BOOTLOADER_PAGE_SIZE_H * 2 ;
	}

	PageOffset = 0;
	CurrentPacketOffset = 0;
	CurrentPageBytesOffset   = 0;
	CurrentPage = FirstUserPage;

	/* Set buffer descriptor table offset in PMA memory */
	WRITE_REG(*BTABLE, BTABLE_OFFSET);

	/* Initialize Endpoint 0 */
	TOGGLE_REG(EP0REG[ENDP0],
		   EP_DTOG_RX | EP_T_FIELD | EP_KIND | EP_DTOG_TX | EPTX_STAT | EPADDR_FIELD,
		   0 | EP_CONTROL | 0,
		   EP_RX_VALID);

	/* Set transmission buffer address for endpoint 0 in buffer descriptor table */
	BTABLE_ADDR_FROM_OFFSET(ENDP0, BTABLE_OFFSET)[USB_ADDRn_TX] = ENDP0_TXADDR;

	/* Set reception buffer address for endpoint 0 in buffer descriptor table */
	BTABLE_ADDR_FROM_OFFSET(ENDP0, BTABLE_OFFSET)[USB_ADDRn_RX] = ENDP0_RXADDR;
	RxTxBuffer[0].MaxPacketSize = MAX_PACKET_SIZE;

	/* Initialize Endpoint 1 */
	TOGGLE_REG(EP0REG[ENDP1],
		   EP_DTOG_RX | EP_T_FIELD | EP_KIND | EP_DTOG_TX | EPADDR_FIELD,
		   1 | EP_INTERRUPT | 0,
		   EP_RX_DIS | EP_TX_NAK);

	/* Set transmission buffer address for endpoint 1 in buffer descriptor table */
	BTABLE_ADDR_FROM_OFFSET(ENDP1, BTABLE_OFFSET)[USB_ADDRn_TX] = ENDP1_TXADDR;

	/* Set transmission byte count for endpoint 1 in buffer descriptor table */
	BTABLE_ADDR_FROM_OFFSET(ENDP1, BTABLE_OFFSET)[USB_COUNTn_TX] = MAX_PACKET_SIZE;
	RxTxBuffer[1].MaxPacketSize = MAX_PACKET_SIZE;

	/* Clear device address and enable USB function */
	WRITE_REG(*DADDR, DADDR_EF | 0);
}

void USB_EPHandler(uint16_t status)
{
	uint8_t endpoint = READ_BIT(status, USB_ISTR_EP_ID);
	uint16_t endpoint_status = EP0REG[endpoint];
	USB_SetupPacket *setup_packet;

	/* OUT and SETUP packets (data reception) */
	if (READ_BIT(endpoint_status, EP_CTR_RX)) {

		/* Copy from packet area to user buffer */
		USB_PMA2Buffer(endpoint);
		if (endpoint == 0) {

			/* If control endpoint */
			if (READ_BIT(endpoint_status, USB_EP0R_SETUP)) {
				setup_packet = (USB_SetupPacket *) RxTxBuffer[endpoint].RXB;
				switch (setup_packet->bRequest) {

				case USB_REQUEST_SET_ADDRESS:
					DeviceAddress = setup_packet->wValue.L;
					USB_SendData(0, 0, 0);
					break;

				case USB_REQUEST_GET_DESCRIPTOR:
					HIDUSB_GetDescriptor(setup_packet);
					break;

				case USB_REQUEST_GET_STATUS:
					USB_SendData(0, (uint16_t *) &DeviceStatus, 2);
					break;

				case USB_REQUEST_GET_CONFIGURATION:
					USB_SendData(0, (uint16_t *) &DeviceConfigured, 1);
					break;

				case USB_REQUEST_SET_CONFIGURATION:
					DeviceConfigured = 1;
					USB_SendData(0, 0, 0);
					break;

				case USB_REQUEST_GET_INTERFACE:
					USB_SendData(0, 0, 0);
					break;

				default:
					USB_SendData(0, 0, 0);
					SET_TX_STATUS(ENDP0, EP_TX_STALL);
					break;
				}
			} else if (RxTxBuffer[endpoint].RXL) {

				/* OUT packet */
				HIDUSB_HandleData((uint8_t *) RxTxBuffer[endpoint].RXB);
			}

		}
		SET_RX_STATUS(endpoint, EP_RX_VALID);
	}
	if (READ_BIT(endpoint_status, EP_CTR_TX)) {

		/* Something transmitted */
		if (DeviceAddress) {

			/* Set device address and enable function */
			WRITE_REG(*DADDR, DADDR_EF | DeviceAddress);
			DeviceAddress = 0;
		}
		USB_Buffer2PMA(endpoint);
		SET_TX_STATUS(endpoint, (endpoint == ENDP1) ? EP_TX_NAK : EP_TX_VALID);
	}
}
