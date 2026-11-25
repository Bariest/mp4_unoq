/* board_config.h -- this robot: Mini Pupper (MP4-ESP32-CORE servo boards) driven
 * by an Arduino UNO Q. The ESP build's file of the same name (main/robot/
 * board_config.h) describes the ESP32-S3 pins; the ported code only needs the
 * names below. */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* Servo bus: Arduino pins (spi_port.cpp). SPI2 of the STM32U585:
 * D13 = SCK, D11 = MOSI, D12 = MISO. Chip select per driver board, in the ESP
 * driver's board order (board 0 = servos 1-3 ... board 3 = servos 10-12).
 * On the carrier board (Arduino_UNO_Q_EXT): CS1 = D10 -> CN2, CS2 = D9 -> CN3,
 * CS3 = D8 -> CN4, CS4 = D7 -> CN5. Plug the legs in as named below. */
#define SERVO_SPI_HOST          2
#define SERVO_SPI_CLK_PIN       13
#define SERVO_SPI_MOSI_PIN      11
#define SERVO_SPI_MISO_PIN      12
#define SERVO_BOARD0_CS_PIN     9     /* servos 1-3   FR */
#define SERVO_BOARD1_CS_PIN     10    /* servos 4-6   FL */
#define SERVO_BOARD2_CS_PIN     7     /* servos 7-9   RR (CN5 on this robot's wiring) */
#define SERVO_BOARD3_CS_PIN     8     /* servos 10-12 RL (CN4) */
#define SERVO_BOARD0_CN         "CN3" /* the carrier's connector of each board, for the boot check */
#define SERVO_BOARD1_CN         "CN2"
#define SERVO_BOARD2_CN         "CN5"   /* rear legs swapped vs the first robot: */
#define SERVO_BOARD3_CN         "CN4"   /* hand test 2026-11-25, new UNO Q board */
/* the names driver_board.c still mentions (only in code spi_port.cpp replaced) */
#define SERVO_SPI_CS_FR_PIN     SERVO_BOARD0_CS_PIN
#define SERVO_SPI_CS_FL_PIN     SERVO_BOARD1_CS_PIN
#define SERVO_SPI_CS_RR_PIN     SERVO_BOARD2_CS_PIN
#define SERVO_SPI_CS_RL_PIN     SERVO_BOARD3_CS_PIN

/* Which leg is on which board (driver_board.h db_phys). 1 = no swap: the
 * wiggle test on this robot (2026-11-19) proved the chip-select wires already
 * reach the right legs; the ESP's default 2 swaps FR and RR. */
#define SERVO_BOARD             1

/* No servo power switch: the rail is wired to the battery. */
#define SERVO_HAS_POWER_EN      0
#define SERVO_POWER_EN_PIN      (-1)

/* IMU: BMI270 (U1) with the BMM150 (U2) on its aux bus, on the UNO Q's I2C2
 * (Wire): D20 = SDA, D21 = SCL, 1k pull-ups; INT1 = D3 (unused, as on the ESP).
 * imu.cpp is the ESP's robot/imu.cc; shim_i2c.cpp carries its i2c_master calls. */
#define IMU_ENABLE              1        /* 0: never touch Wire */
#define IMU_I2C_PORT            2
#define IMU_I2C_SDA_PIN         20
#define IMU_I2C_SCL_PIN         21
#define IMU_I2C_FREQ_HZ         400000
#define IMU_BMI270_I2C_ADDR     mpx_bmi270_addr()   /* 0x68 (SDO low), or 0x69 if only that answers */
#define IMU_BMM150_I2C_ADDR     0x10     /* 7-bit, on the BMI270 aux bus */
#define IMU_INT1_PIN            3
/* The BMI270's mounting on this carrier board, into firmware-dev's ESP board frame
 * (imu_remap.h): out = sign * raw[axis], per output axis x, y, z. This carrier holds
 * the chip turned 180 degrees about y (learned 2026-11-25 with /unoq/imu/learn: standing
 * flat raw z = +1 g, nose down raw y negative). A saved "imu.map" overrides it. */
#define IMU_MAP_X               (-1)
#define IMU_MAP_Y               2
#define IMU_MAP_Z               (-3)

/* LCD: ST7789 320x240 on the J1 FPC, SPI1 of the STM32U585 (display_hw.cpp):
 * A0 = PA4 (CS), A1 = PA5 (SCK), A2 = PA6 (MISO, unused), A3 = PA7 (MOSI);
 * DC = D5, RESET = D4, backlight = D6 through the P-FET Q1 (LOW = on). */
#define LCD_ENABLE              1
#define LCD_DC_PIN              5
#define LCD_RST_PIN             4
#define LCD_BL_PIN              6
#define LCD_BL_ACTIVE_LOW       1
#define LCD_H_RES               320
#define LCD_V_RES               240
#define LCD_SPI_HZ              40000000

#endif
