// SPIC / flash-margin diagnostics for the XIP-stall investigation.
// Self-contained: the SDK's ameba_soc.h does not survive this core's C++
// compile of sketch files, so the one struct we need (FLASH_InitTypeDef,
// copied VERBATIM from rtl8721d_flash.h 3.1.9 -- layout must match the ROM)
// and the two ROM entry points are declared here directly.
#include <stdio.h>
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

typedef struct {
	u32 FLASH_Id;				/*!< Specifies the flash vendor ID.
								This parameter can be a value of @ref FLASH_VENDOR_ID_definitions */
	u8 FLASH_cur_bitmode;		/*!< Specifies the current bitmode of SPIC.
								This parameter can be a value of @ref FLASH_BIT_Mode_definitions */
	u8 FLASH_baud_rate;			/*!< Specifies the spi_sclk divider value. The frequency of spi_sclk is derived from:
								Frequency of spi_sclk = Frequency of oc_clk / (2 * FLASH_baud_rate) */
	u8 FLASH_baud_boot;			/*!< Specifies the spi_sclk divider value for rom boot. The frequency of spi_sclk is derived from:
								Frequency of spi_sclk = Frequency of oc_clk / (2 * FLASH_baud_rate) */
	u32 FLASH_cur_cmd; 			/*!< Specifies the current read cmd which is used to read data from flash
								in current bitmode. */
						
	/* status bits define */
	u32 FLASH_QuadEn_bit;		/*!< Specifies the QE bit in status register which is used to enable Quad I/O mode . */
	u32 FLASH_Busy_bit;			/*!< Specifies the WIP(Write in Progress) bit in status register which indicates whether 
								the device is busy in program/erase/write status register progress. */
	u32 FLASH_WLE_bit; 			/*!< Specifies the WEL(Write Enable Latch) bit in status register which indicates whether
								the device will accepts program/erase/write status register instructions*/
	u32 FLASH_Status2_exist;		/*!< Specifies whether this flash chip has Status Register2 or not.
								This parameter can be 0/1. 0 means it doesn't have Status Register2, 1 means 
								it has Status Register2.*/
	
	/* calibration data */
	u8 FLASH_rd_sample_phase_cal;	/*!< Specifies the read sample phase obtained from calibration. this is cal sample phase get from high speed cal */
	u8 FLASH_rd_sample_phase;	/*!< Specifies the read sample phase obtained from calibration. this is current sample phase */
	u8 FLASH_rd_dummy_cyle[3];	/*!< Specifies the read dummy cycle of different bitmode according to 
								flash datasheet*/

	/* valid R/W command set */
	u32 FLASH_rd_dual_o; 			/*!< Specifies dual data read cmd */
	u32 FLASH_rd_dual_io; 			/*!< Specifies dual data/addr read cmd */
	u32 FLASH_rd_quad_o; 		/*!< Specifies quad data read cmd */
	u32 FLASH_rd_quad_io; 		/*!< Specifies quad data/addr read cmd */   
	u32 FLASH_wr_dual_i; 			/*!< Specifies dual data write cmd */
	u32 FLASH_wr_dual_ii;			/*!< Specifies dual data/addr write cmd */
	u32 FLASH_wr_quad_i; 			/*!< Specifies quad data write cmd */
	u32 FLASH_wr_quad_ii;			/*!< Specifies quad data/addr write cmd */
	u32 FALSH_dual_valid_cmd;		/*!< Specifies valid cmd of dual bitmode to program/read flash in auto mode */
	u32 FALSH_quad_valid_cmd;	/*!< Specifies valid cmd of quad bitmode to program/read flash in auto mode */

	/* other command set */
	u8 FLASH_cmd_wr_en;			/*!< Specifies the Write Enable(WREN) instruction which is for setting WEL bit*/
	u8 FLASH_cmd_rd_id;			/*!< Specifies the Read ID instruction which is for getting the identity of the flash divice.*/
	u8 FLASH_cmd_rd_status;		/*!< Specifies the Read Status Register instruction which is for getting the status of flash */
	u8 FLASH_cmd_rd_status2;		/*!< Specifies the Read Status Register2 instruction which is for getting the status2 of flash */
	u8 FLASH_cmd_wr_status;		/*!< Specifies the Write Status Register instruction which is for setting the status register of flash */
	u8 FLASH_cmd_wr_status2;		/*!< Specifies the Write Status Register2 instruction which is for setting the status register2 of flash. 
								 In some flash chips, status2 write cmd != status1 write cmd, 
								 like: GD25Q32C, GD25Q64C,GD25Q128C etc.*/
	u8 FLASH_cmd_chip_e;			/*!< Specifies the Erase Chip instruction which is for erasing the whole chip*/
	u8 FLASH_cmd_block_e;		/*!< Specifies the Erase Block instruction which is for erasing 64kB*/
	u8 FLASH_cmd_sector_e;		/*!< Specifies the Erase Sector instruction which is for erasing 4kB*/
	u8 FLASH_cmd_pwdn_release;	/*!< Specifies the Release from Deep Power Down instruction which is for exiting power down mode.*/
	u8 FLASH_cmd_pwdn;			/*!< Specifies the Deep Power Down instruction which is for entering power down mode.*/

	/* debug log */
	u8 debug;					/*!< Specifies whether or not to print debug log.*/

	/* new calibration */
	u8 phase_shift_idx;			/*!< Specifies the phase shift idx in new calibration.*/

	u8 FLASH_addr_phase_len;	/*!< Specifies the number of bytes in address phase (between command phase and write/read phase).
								This parameter can be 0/1/2/3. 0 means 4-byte address mode in SPI Flash.*/
	u8 FLASH_pseudo_prm_en;		/*!< Specifies whether SPIC enables SPIC performance read mode or not.*/
	u8 FLASH_pinmux;			/*!< Specifies which pinmux is used. PINMUX_S0 or PINMUX_S1*/			

	u32 FLASH_rd_fast_single;	/*!< Specifies fast read cmd in auto mode.*/
} FLASH_InitTypeDef;

extern "C" {
extern FLASH_InitTypeDef flash_init_para;                 // shared with ROM/boot
extern void FLASH_RxCmdXIP(u8 cmd, u32 read_len, u8 *read_data);
extern void FLASH_SetSpiMode(FLASH_InitTypeDef *init, u8 SpicBitMode);

void flashDiagInfo(char *out, unsigned n) {
    u8 id[3] = {0, 0, 0};
    FLASH_RxCmdXIP(0x9F, 3, id);                          // JEDEC: vendor, type, capacity
    snprintf(out, n, "flash: jedec=%02x%02x%02x id=%08x bitmode=%u baud=%u phase=%u(cal=%u) dummy=%u,%u,%u",
             id[0], id[1], id[2], (unsigned)flash_init_para.FLASH_Id,
             flash_init_para.FLASH_cur_bitmode, flash_init_para.FLASH_baud_rate,
             flash_init_para.FLASH_rd_sample_phase, flash_init_para.FLASH_rd_sample_phase_cal,
             flash_init_para.FLASH_rd_dummy_cyle[0], flash_init_para.FLASH_rd_dummy_cyle[1],
             flash_init_para.FLASH_rd_dummy_cyle[2]);
}

void flashDiagBaud(int bd, char *out, unsigned n) {
    flash_init_para.FLASH_baud_rate = (u8)bd;
    flash_init_para.FLASH_baud_boot = (u8)bd;
    FLASH_SetSpiMode(&flash_init_para, flash_init_para.FLASH_cur_bitmode);
    snprintf(out, n, "flash: baud_rate -> %d (phase %u bitmode %u)", bd,
             flash_init_para.FLASH_rd_sample_phase, flash_init_para.FLASH_cur_bitmode);
}

extern int FLASH_ReadStream(u32 address, u32 len, u8 *pbuf);
extern unsigned long micros(void);   // Arduino core, C linkage

void flashDiagBench(char *out, unsigned n) {
    static u8 buf[512];
    unsigned long t0 = micros();
    for (int i = 0; i < 32; i++)                       // 16 KB total
        FLASH_ReadStream(0x100000 + i * 512, 512, buf);
    unsigned long us = micros() - t0;
    snprintf(out, n, "flash: bench 16KB in %lu us (%lu KB/s) baud=%u phase=%u",
             us, (unsigned long)(16384UL * 1000UL / (us ? us : 1)),
             flash_init_para.FLASH_baud_rate, flash_init_para.FLASH_rd_sample_phase);
}

// The durable mitigation: this unit's flash (Boya BY25Q64, JEDEC 68 40 17)
// cannot sustain the SDK's default SPIC clock under BLE-connect bus load —
// connects froze 100% at full speed and pass at divider 2 (verified by
// throughput benchmark + connect ritual, 2026-08-24). Something (KM0-side
// clock management) occasionally restores the fast divider, so the slow
// setting is re-asserted continuously rather than set once.
void flashClkSafe(void) {
    flash_init_para.FLASH_baud_rate = 2;
    flash_init_para.FLASH_baud_boot = 2;
    FLASH_SetSpiMode(&flash_init_para, flash_init_para.FLASH_cur_bitmode);
}

void flashDiagPhase(int ph, char *out, unsigned n) {
    flash_init_para.FLASH_rd_sample_phase = (u8)ph;
    FLASH_SetSpiMode(&flash_init_para, flash_init_para.FLASH_cur_bitmode);
    snprintf(out, n, "flash: rd_sample_phase -> %d (bitmode %u)", ph,
             flash_init_para.FLASH_cur_bitmode);
}
}
