/*
  *
  **************************************************************************
  **                        STMicroelectronics				**
  **************************************************************************
  **                        marco.cali@st.com			          **
  **************************************************************************
  *                                                                        *
  *		FTS Core functions					 *
  *                                                                        *
  **************************************************************************
  **************************************************************************
  *
  */

/*!
  * \file ftsCore.c
  * \brief Contains the implementation of the Core functions
  */

#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include "ftsCompensation.h"
#include "ftsCore.h"
#include "ftsError.h"
#include "ftsIO.h"
#include "ftsTest.h"
#include "ftsTime.h"
#include "ftsTool.h"
#include "ftsFrame.h"

/** @addtogroup system_info
  * @{
  */
SysInfo systemInfo;	/* Global System Info variable, accessible in all the
			 * driver */
/** @}*/

static int reset_gpio = GPIO_NOT_DEFINED;/* /< gpio number of the rest pin,
					  * the value is  GPIO_NOT_DEFINED
					  * if the reset pin is not connected */
static int system_reseted_up;	/* /< flag checked during resume to understand
				 * if there was a system reset
				 * and restore the proper state */
static int system_reseted_down; /* /< flag checked during suspend to understand
				 * if there was a system reset
				 *  and restore the proper state */

/**
  * Initialize core variables of the library.
  * Must be called during the probe before any other lib function
  * @param info pointer to fts_ts_info which contains info about the device and
  * its hw setup
  * @return OK if success or an error code which specify the type of error
  */
int initCore(struct fts_ts_info *info)
{
	int ret = OK;

	pr_info("%s: Initialization of the Core...\n", __func__);
	ret |= openChannel(info->client);
	ret |= resetErrorList();
	ret |= initTestToDo();
	setResetGpio(info->board->reset_gpio);
	if (ret < OK)
		pr_err("%s: Initialization Core ERROR %08X!\n",
			 __func__, ret);
	else
		pr_info("%s: Initialization Finished!\n",
			 __func__);
	return ret;
}

/**
  * Set the reset_gpio variable with the actual gpio number of the board link to
  * the reset pin
  * @param gpio gpio number link to the reset pin of the IC
  */
void setResetGpio(int gpio)
{
	reset_gpio = gpio;
	pr_info("setResetGpio: reset_gpio = %d\n", reset_gpio);
}

/**
  * Perform a system reset of the IC.
  * If the reset pin is associated to a gpio, the function execute an hw reset
  * (toggling of reset pin) otherwise send an hw command to the IC
  * @return OK if success or an error code which specify the type of error
  */
int fts_system_reset(void)
{
	u8 readData[FIFO_EVENT_SIZE];
	int event_to_search;
	int res = -1;
	int i;
	u8 data[1] = { SYSTEM_RESET_VALUE };

	event_to_search = (int)EVT_ID_CONTROLLER_READY;

	pr_info("System resetting...\n");
	for (i = 0; i < RETRY_SYSTEM_RESET && res < 0; i++) {
		resetErrorList();
		fts_enableInterrupt(false);
		/* disable interrupt before resetting to be able to get boot
		 * events */

		if (reset_gpio == GPIO_NOT_DEFINED)
			res = fts_writeU8UX(FTS_CMD_HW_REG_W, ADDR_SIZE_HW_REG,
					    ADDR_SYSTEM_RESET, data, ARRAY_SIZE(
						    data));
		else {
			gpio_set_value(reset_gpio, 0);
			msleep(10);
			gpio_set_value(reset_gpio, 1);
			res = OK;
		}
		if (res < OK)
			pr_err("fts_system_reset: ERROR %08X\n", ERROR_BUS_W);
		else {
			res = pollForEvent(&event_to_search, 1, readData,
					   GENERAL_TIMEOUT);
			if (res < OK)
				pr_err("fts_system_reset: ERROR %08X\n", res);
		}
	}
	if (res < OK) {
		pr_err("fts_system_reset...failed after 3 attempts: ERROR %08X\n",
			(res | ERROR_SYSTEM_RESET_FAIL));
		return res | ERROR_SYSTEM_RESET_FAIL;
	} else {
		pr_debug("System reset DONE!\n");
		system_reseted_down = 1;
		system_reseted_up = 1;
		return OK;
	}
}

/**
  * Return the value of system_resetted_down.
  * @return the flag value: 0 if not set, 1 if set
  */
int isSystemResettedDown(void)
{
	return system_reseted_down;
}

/**
  * Return the value of system_resetted_up.
  * @return the flag value: 0 if not set, 1 if set
  */
int isSystemResettedUp(void)
{
	return system_reseted_up;
}


/**
  * Set the value of system_reseted_down flag
  * @param val value to write in the flag
  */
void setSystemResetedDown(int val)
{
	system_reseted_down = val;
}

/**
  * Set the value of system_reseted_up flag
  * @param val value to write in the flag
  */
void setSystemResetedUp(int val)
{
	system_reseted_up = val;
}


/** @addtogroup events_group
  * @{
  */

/**
  * Poll the FIFO looking for a specified event within a timeout. Support a
  * retry mechanism.
  * @param event_to_search pointer to an array of int where each element
  * correspond to a byte of the event to find.
  * If the element of the array has value -1, the byte of the event,
  * in the same position of the element is ignored.
  * @param event_bytes size of event_to_search
  * @param readData pointer to an array of byte which will contain the event
  * found
  * @param time_to_wait time to wait before going in timeout
  * @return OK if success or an error code which specify the type of error
  */
int pollForEvent(int *event_to_search, int event_bytes, u8 *readData, int
		 time_to_wait)
{
	const u8 NO_RESPONSE = 0xFF;
	const int POLL_SLEEP_TIME_MS = 5;
	int i, find, retry, count_err;
	int time_to_count;
	int err_handling = OK;
	StopWatch clock;

	u8 cmd[1] = { FIFO_CMD_READONE };
	char temp[128] = { 0 };

	find = 0;
	retry = 0;
	count_err = 0;
	time_to_count = time_to_wait / POLL_SLEEP_TIME_MS;

	startStopWatch(&clock);
	while (find != 1 && retry < time_to_count &&
		fts_writeReadU8UX(cmd[0], 0, 0, readData, FIFO_EVENT_SIZE,
			DUMMY_FIFO)
	       >= OK) {
		if (readData[0] == NO_RESPONSE ||
		    readData[0] == EVT_ID_NOEVENT) {
			/* No events available, so sleep briefly */
			msleep(POLL_SLEEP_TIME_MS);
			retry++;
			continue;
		} else if (readData[0] == EVT_ID_ERROR) {
			/* Log of errors */
			pr_err("%s\n",
				 printHex("ERROR EVENT = ",
					  readData,
					  FIFO_EVENT_SIZE,
					  temp,
					  sizeof(temp)));
			memset(temp, 0, 128);
			count_err++;
			err_handling = errorHandler(readData, FIFO_EVENT_SIZE);
			if ((err_handling & 0xF0FF0000) ==
			    ERROR_HANDLER_STOP_PROC) {
				pr_err("pollForEvent: forced to be stopped! ERROR %08X\n",
					err_handling);
				return err_handling;
			}
		} else {
			pr_info("%s\n",
				 printHex("READ EVENT = ", readData,
					  FIFO_EVENT_SIZE,
					  temp,
					  sizeof(temp)));
			memset(temp, 0, 128);

			if (readData[0] == EVT_ID_CONTROLLER_READY &&
			    event_to_search[0] != EVT_ID_CONTROLLER_READY) {
				pr_err("pollForEvent: Unmanned Controller Ready Event! Setting reset flags...\n");
				setSystemResetedUp(1);
				setSystemResetedDown(1);
			}
		}

		find = 1;

		for (i = 0; i < event_bytes; i++) {
			if (event_to_search[i] != -1 && (int)readData[i] !=
			    event_to_search[i]) {
				find = 0;
				break;
			}
		}
	}
	stopStopWatch(&clock);
	if ((retry >= time_to_count) && find != 1) {
		pr_err("pollForEvent: ERROR %08X\n", ERROR_TIMEOUT);
		return ERROR_TIMEOUT;
	} else if (find == 1) {
		pr_info("%s\n",
			 printHex("FOUND EVENT = ",
				  readData,
				  FIFO_EVENT_SIZE,
				  temp,
				  sizeof(temp)));
		memset(temp, 0, 128);
		pr_debug("Event found in %d ms (%d iterations)! Number of errors found = %d\n",
			elapsedMillisecond(&clock), retry, count_err);
		return count_err;
	} else {
		pr_err("pollForEvent: ERROR %08X\n", ERROR_BUS_R);
		return ERROR_BUS_R;
	}
}

/** @}*/

/**
  * Check that the FW sent the echo even after a command was sent
  * @param cmd pointer to an array of byte which contain the command previously
  * sent
  * @param size size of cmd
  * @return OK if success or an error code which specify the type of error
  */
int checkEcho(u8 *cmd, int size)
{
	int ret, i;
	int event_to_search[FIFO_EVENT_SIZE];
	u8 readData[FIFO_EVENT_SIZE];


	if (size < 1) {
		pr_err("checkEcho: Error Size = %d not valid!\n", size);
		return ERROR_OP_NOT_ALLOW;
	} else {
		if ((size + 4) > FIFO_EVENT_SIZE)
			size = FIFO_EVENT_SIZE - 4;
		/* Echo event 0x43 0x01 xx xx xx xx xx fifo_status
		 * therefore command with more than 4 bytes will be trunked */

		event_to_search[0] = EVT_ID_STATUS_UPDATE;
		event_to_search[1] = EVT_TYPE_STATUS_ECHO;
		for (i = 2; i < size + 2; i++)
			event_to_search[i] = cmd[i - 2];
		if ((cmd[0] == FTS_CMD_SYSTEM) &&
			(cmd[1] == SYS_CMD_SPECIAL) &&
			((cmd[2] == SPECIAL_FULL_PANEL_INIT) ||
			(cmd[2] == SPECIAL_PANEL_INIT)))
			ret = pollForEvent(event_to_search, size + 2, readData,
				   TIMEOUT_ECHO_FPI);
		else if ((cmd[0] == FTS_CMD_SYSTEM) &&
			(cmd[1] == SYS_CMD_CX_TUNING))
			ret = pollForEvent(event_to_search, size + 2, readData,
				   TIMEOUT_ECHO_SINGLE_ENDED_SPECIAL_AUTOTUNE);
		else if (cmd[0] == FTS_CMD_SYSTEM &&
			 cmd[1] == SYS_CMD_SPECIAL &&
			 cmd[2] == SPECIAL_FIFO_FLUSH)
			ret = pollForEvent(event_to_search, size + 2, readData,
				   TIMEOUT_ECHO_FLUSH);
		else
			ret = pollForEvent(event_to_search, size + 2, readData,
				   TIEMOUT_ECHO);
		if (ret < OK) {
			pr_err("checkEcho: Echo Event not found! ERROR %08X\n",
				ret);
			return ret | ERROR_CHECK_ECHO_FAIL;
		} else if (ret > OK) {
			pr_err("checkEcho: Echo Event found but with some error events before! num_error = %d\n",
				ret);
			return ERROR_CHECK_ECHO_FAIL;
		}

		pr_info("ECHO OK!\n");
		return ret;
	}
}


/** @addtogroup scan_mode
  * @{
  */
/**
  * Set a scan mode in the IC
  * @param mode scan mode to set; possible values @link scan_opt Scan Mode
  * Option @endlink
  * @param settings option for the selected scan mode
  * (for example @link active_bitmask Active Mode Bitmask @endlink)
  * @return OK if success or an error code which specify the type of error
  */
int setScanMode(u8 mode, u8 settings)
{
	u8 cmd[3] = { FTS_CMD_SCAN_MODE, mode, settings };
	u8 cmd1[7] = {0xFA, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
	int ret, size = 3;

	pr_debug("%s: Setting scan mode: mode = %02X settings = %02X !\n",
		__func__, mode, settings);
	if (mode == SCAN_MODE_LOW_POWER)
		size = 2;
	ret = fts_write(cmd1, 7);
	if(ret >= OK)
		ret = fts_write(cmd, size);
	/* use write instead of writeFw because can be called while the
	 * interrupt are enabled */
	if (ret < OK) {
		pr_err("%s: write failed...ERROR %08X !\n",
			 __func__, ret);
		return ret | ERROR_SET_SCAN_MODE_FAIL;
	}
	pr_debug("%s: Setting scan mode OK!\n", __func__);
	return OK;
}
/** @}*/


/** @addtogroup feat_sel
  * @{
  */
/**
  * Set a feature and its option in the IC
  * @param feat feature to set; possible values @link feat_opt Feature Selection
  * Option @endlink
  * @param settings pointer to an array of byte which store the options for
  * the selected feature (for example the gesture mask to activate
  * @link gesture_opt Gesture IDs @endlink)
  * @param size in bytes of settings
  * @return OK if success or an error code which specify the type of error
  */
int setFeatures(u8 feat, u8 *settings, int size)
{
	u8 *cmd;
	int i = 0;
	int ret;
	char *buff;
	int buff_len = ((2 + 1) * size + 1) * sizeof(char);
	int index = 0;
	u8 cmd1[7] = {0xFA, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};

	cmd = kzalloc((2 + size) * sizeof(u8), GFP_KERNEL);
	buff = kzalloc(buff_len, GFP_KERNEL);
	if ((buff == NULL) || (cmd == NULL)) {
		kfree(buff);
		kfree(cmd);
		return ERROR_ALLOC;
        }

	pr_info("%s: Setting feature: feat = %02X !\n", __func__, feat);
	cmd[0] = FTS_CMD_FEATURE;
	cmd[1] = feat;
	for (i = 0; i < size; i++) {
		cmd[2 + i] = settings[i];
		index += scnprintf(buff + index, buff_len - index,
					"%02X ", settings[i]);
	}
	pr_info("%s: Settings = %s\n", __func__, buff);
	ret = fts_write(cmd1, 7);
	if(ret >= OK)
		ret = fts_write(cmd, 2 + size);
	/* use write instead of writeFw because can be called while the
	 * interrupts are enabled */
	if (ret < OK) {
		pr_err("%s: write failed...ERROR %08X !\n", __func__, ret);
		kfree(buff);
		kfree(cmd);
		return ret | ERROR_SET_FEATURE_FAIL;
	}
	pr_info("%s: Setting feature OK!\n", __func__);
	kfree(cmd);
	kfree(buff);
	return OK;
}
/** @}*/

/** @addtogroup sys_cmd
  * @{
  */
/**
  * Write a system command to the IC
  * @param sys_cmd System Command to execute; possible values
  * @link sys_opt System Command Option @endlink
  * @param sett settings option for the selected system command
  * (@link sys_special_opt Special Command Option @endlink, @link ito_opt
  * ITO Test Option @endlink, @link load_opt Load Host Data Option @endlink)
  * @param size in bytes of settings
  * @return OK if success or an error code which specify the type of error
  */
int writeSysCmd(u8 sys_cmd, u8 *sett, int size)
{
	u8 *cmd;
	int ret;
	char *buff;
	int buff_len = ((2 + 1) * size + 1) * sizeof(char);
	int index = 0;
	u8 cmd1[7] = {0xFA, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};

	cmd = kzalloc((2 + size) * sizeof(u8), GFP_KERNEL);
	buff = kzalloc(buff_len, GFP_KERNEL);
	if ((buff == NULL) || (cmd == NULL)) {
	        kfree(buff);
		kfree(cmd);
		return ERROR_ALLOC;
        }

	cmd[0] = FTS_CMD_SYSTEM;
	cmd[1] = sys_cmd;

	for (ret = 0; ret < size; ret++) {
		cmd[2 + ret] = sett[ret];
		index += scnprintf(buff + index, buff_len - index,
					"%02X ", sett[ret]);
	}
	pr_info("%s: Command = %02X %02X %s\n", __func__, cmd[0],
		 cmd[1], buff);
	pr_info("%s: Writing Sys command...\n", __func__);
	if (sys_cmd != SYS_CMD_LOAD_DATA) {
		ret = fts_write(cmd1, 7);
		if(ret >= OK)
			ret = fts_writeFwCmd(cmd, 2 + size);
	}
	else {
		if (size >= 1)
			ret = requestSyncFrame(sett[0]);
		else {
			pr_err("%s: No setting argument! ERROR %08X\n",
				__func__, ERROR_OP_NOT_ALLOW);
			kfree(cmd);
			kfree(buff);
			return ERROR_OP_NOT_ALLOW;
		}
	}
	if (ret < OK)
		pr_err("%s: ERROR %08X\n", __func__, ret);
	else
		pr_info("%s: FINISHED!\n", __func__);

	kfree(cmd);
	kfree(buff);
	return ret;
}
/** @}*/

/** @addtogroup system_info
  * @{
  */
/**
  * Initialize the System Info Struct with default values according to the error
  * found during the reading
  * @param i2cError 1 if there was an I2C error while reading the System Info
  * data from memory, other value if another error occurred
  * @return OK if success or an error code which specify the type of error
  */
int defaultSysInfo(int i2cError)
{
	int i;

	pr_info("Setting default System Info...\n");

	if (i2cError == 1) {
		systemInfo.u16_fwVer = 0xFFFF;
		systemInfo.u16_cfgProjectId = 0xFFFF;
		for (i = 0; i < RELEASE_INFO_SIZE; i++)
			systemInfo.u8_releaseInfo[i] = 0xFF;
		systemInfo.u16_cxVer = 0xFFFF;
	} else {
		systemInfo.u16_fwVer = 0x0000;
		systemInfo.u16_cfgProjectId = 0x0000;
		for (i = 0; i < RELEASE_INFO_SIZE; i++)
			systemInfo.u8_releaseInfo[i] = 0x00;
		systemInfo.u16_cxVer = 0x0000;
	}

	systemInfo.u8_scrRxLen = 0;
	systemInfo.u8_scrTxLen = 0;

	pr_info("default System Info DONE!\n");
	return OK;
}

/**
  * Read the System Info data from memory. System Info is loaded automatically
  * after every system reset.
  * @param request if 1, will be asked to the FW to reload the data, otherwise
  * attempt to read it directly from memory
  * @return OK if success or an error code which specify the type of error
  */
int readSysInfo(int request)
{
	int ret, i, index = 0;
	u8 sett = LOAD_SYS_INFO;
	u8 data[SYS_INFO_SIZE] = { 0 };
	char temp[256] = { 0 };

	if (request == 1) {
		pr_info("%s: Requesting System Info...\n", __func__);

		ret = writeSysCmd(SYS_CMD_LOAD_DATA, &sett, 1);
		if (ret < OK) {
			pr_err("%s: error while writing the sys cmd ERROR %08X\n",
				__func__, ret);
			goto FAIL;
		}
	}

	pr_info("%s: Reading System Info...\n", __func__);
	ret = fts_writeReadU8UX(FTS_CMD_FRAMEBUFFER_R, BITS_16,
				ADDR_FRAMEBUFFER, data, SYS_INFO_SIZE,
				DUMMY_FRAMEBUFFER);
	if (ret < OK) {
		pr_err("%s: error while reading the system data ERROR %08X\n",
			__func__, ret);
		goto FAIL;
	}

	pr_info("%s: Parsing System Info...\n", __func__);

	if (data[0] != HEADER_SIGNATURE) {
		pr_err("%s: The Header Signature is wrong!  sign: %02X != %02X ERROR %08X\n",
			__func__, data[0], HEADER_SIGNATURE,
			ERROR_WRONG_DATA_SIGN);
		ret = ERROR_WRONG_DATA_SIGN;
		goto FAIL;
	}


	if (data[1] != LOAD_SYS_INFO) {
		pr_err("%s: The Data ID is wrong!  ids: %02X != %02X ERROR %08X\n",
			__func__, data[3], LOAD_SYS_INFO,
			ERROR_DIFF_DATA_TYPE);
		ret = ERROR_DIFF_DATA_TYPE;
		goto FAIL;
	}

	index += 4;
	u8ToU16(&data[index], &systemInfo.u16_apiVer_rev);
	index += 2;
	systemInfo.u8_apiVer_minor = data[index++];
	systemInfo.u8_apiVer_major = data[index++];
	u8ToU16(&data[index], &systemInfo.u16_chip0Ver);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_chip0Id);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_chip1Ver);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_chip1Id);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_fwVer);
	index += 2;
	pr_info("FW VER = %04X\n", systemInfo.u16_fwVer);
	u8ToU16(&data[index], &systemInfo.u16_svnRev);
	index += 2;
	pr_info("SVN REV = %04X\n", systemInfo.u16_svnRev);
	u8ToU16(&data[index], &systemInfo.u16_cfgVer);
	index += 2;
	pr_info("CONFIG VER = %04X\n", systemInfo.u16_cfgVer);
	u8ToU16(&data[index], &systemInfo.u16_cfgProjectId);
	index += 2;
	pr_info("CONFIG PROJECT ID = %04X\n", systemInfo.u16_cfgProjectId);
	u8ToU16(&data[index], &systemInfo.u16_cxVer);
	index += 2;
	pr_info("CX VER = %04X\n", systemInfo.u16_cxVer);
	u8ToU16(&data[index], &systemInfo.u16_cxProjectId);
	index += 2;
	pr_info("CX PROJECT ID = %04X\n", systemInfo.u16_cxProjectId);
	systemInfo.u8_cfgAfeVer = data[index++];
	systemInfo.u8_cxAfeVer =  data[index++];
	systemInfo.u8_panelCfgAfeVer = data[index++];
	pr_info("AFE VER: CFG = %02X - CX = %02X - PANEL = %02X\n",
		 systemInfo.u8_cfgAfeVer, systemInfo.u8_cxAfeVer,
		 systemInfo.u8_panelCfgAfeVer);
	systemInfo.u8_protocol = data[index++];
	pr_info("Protocol = %02X\n", systemInfo.u8_protocol);
	/* index+= 1; */
	/* skip reserved area */

	/* pr_err("Die Info =  "); */
	for (i = 0; i < DIE_INFO_SIZE; i++)
		systemInfo.u8_dieInfo[i] = data[index++];

	/* pr_err("\n"); */
	pr_info("%s\n",
		 printHex("Die Info =  ",
			  systemInfo.u8_dieInfo,
			  DIE_INFO_SIZE, temp, sizeof(temp)));
	memset(temp, 0, 256);


	/* pr_err("Release Info =  "); */
	for (i = 0; i < RELEASE_INFO_SIZE; i++)
		systemInfo.u8_releaseInfo[i] = data[index++];

	/* pr_err("\n"); */

	pr_info("%s\n",
		 printHex("Release Info =  ",
			  systemInfo.u8_releaseInfo,
			  RELEASE_INFO_SIZE,
			  temp,
			  sizeof(temp)));
	memset(temp, 0, 256);

	u8ToU32(&data[index], &systemInfo.u32_fwCrc);
	index += 4;
	u8ToU32(&data[index], &systemInfo.u32_cfgCrc);
	index += 4;

	index += 4;	/* skip reserved area */

	systemInfo.u8_mpFlag = data[index++];
	pr_info("MP FLAG = %02X\n", systemInfo.u8_mpFlag);

	index += 3 + 4; /* +3 remaining from mp flag address */

	systemInfo.u8_ssDetScanSet = data[index];
	pr_info("SS Detect Scan Select = %d\n",
		 systemInfo.u8_ssDetScanSet);
	index += 4;

	u8ToU16(&data[index], &systemInfo.u16_scrResX);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_scrResY);
	index += 2;
	pr_info("Screen Resolution = %d x %d\n",
		 systemInfo.u16_scrResX, systemInfo.u16_scrResY);
	systemInfo.u8_scrTxLen = data[index++];
	pr_info("TX Len = %d\n", systemInfo.u8_scrTxLen);
	systemInfo.u8_scrRxLen = data[index++];
	pr_info("RX Len = %d\n", systemInfo.u8_scrRxLen);
	systemInfo.u8_keyLen = data[index++];
	pr_info("Key Len = %d\n", systemInfo.u8_keyLen);
	systemInfo.u8_forceLen = data[index++];
	pr_info("Force Len = %d\n", systemInfo.u8_forceLen);
	index += 8;

	u8ToU32(&data[index], &systemInfo.u32_productionTimestamp);
	pr_info("Production Timestamp = %08X\n",
	systemInfo.u32_productionTimestamp);

	index += 32;	/* skip reserved area */

	u8ToU16(&data[index], &systemInfo.u16_dbgInfoAddr);
	index += 2;

	index += 6;	/* skip reserved area */

	u8ToU16(&data[index], &systemInfo.u16_msTchRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_msTchFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_msTchStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_msTchBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssTchTxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchTxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchTxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchTxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssTchRxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchRxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchRxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchRxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_keyRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_keyFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_keyStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_keyBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_frcRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_frcFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_frcStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_frcBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssDetRawAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssDetFilterAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssDetStrenAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssDetBaselineAddr);
	index += 2;

	pr_info("Parsed %d bytes!\n", index);


	if (index != SYS_INFO_SIZE) {
		pr_err("%s: index = %d different from %d ERROR %08X\n",
			__func__, index, SYS_INFO_SIZE,
			ERROR_OP_NOT_ALLOW);
		return ERROR_OP_NOT_ALLOW;
	}

	pr_info("System Info Read DONE!\n");
	return OK;

FAIL:
	defaultSysInfo(isI2cError(ret));
	return ret;
}
/** @}*/


/**
  * Read data from the Config Memory
  * @param offset Starting address in the Config Memory of data to read
  * @param outBuf pointer of a byte array which contain the bytes to read
  * @param len number of bytes to read
  * @return OK if success or an error code which specify the type of error
  */
int readConfig(u16 offset, u8 *outBuf, int len)
{
	int ret;
	u64 final_address = offset + ADDR_CONFIG_OFFSET;

	pr_info("%s: Starting to read config memory at %llx ...\n",
		__func__, final_address);
	ret = fts_writeReadU8UX(FTS_CMD_CONFIG_R, BITS_16, final_address,
				outBuf, len, DUMMY_CONFIG);
	if (ret < OK) {
		pr_err("%s: Impossible to read Config Memory... ERROR %08X!\n",
			__func__, ret);
		return ret;
	}

	pr_info("%s: Read config memory FINISHED!\n", __func__);
	return OK;
}

/**
  * Write data into the Config Memory
  * @param offset Starting address in the Config Memory where write the data
  * @param data pointer of a byte array which contain the data to write
  * @param len number of bytes to write
  * @return OK if success or an error code which specify the type of error
  */
int writeConfig(u16 offset, u8 *data, int len)
{
	int ret;
	u64 final_address = offset + ADDR_CONFIG_OFFSET;

	pr_info("%s: Starting to write config memory at %llx ...\n",
		__func__, final_address);
	ret = fts_writeU8UX(FTS_CMD_CONFIG_W, BITS_16, final_address, data,
			    len);
	if (ret < OK) {
		pr_err("%s: Impossible to write Config Memory... ERROR %08X!\n",
			__func__, ret);
		return ret;
	}

	pr_info("%s: Write config memory FINISHED!\n", __func__);
	return OK;
}

/* Set the interrupt state
 * @param enable Indicates whether interrupts should enabled.
 * @return OK if success
 */
int fts_enableInterrupt(bool enable)
{
	struct fts_ts_info *info = NULL;
	unsigned long flag;

	if (getClient() == NULL) {
		pr_err("Cannot get client irq. Error = %08X\n",
			ERROR_OP_NOT_ALLOW);
		return ERROR_OP_NOT_ALLOW;
	}
	info = dev_get_drvdata(&getClient()->dev);

	spin_lock_irqsave(&info->fts_int, flag);

	if (enable == info->irq_enabled)
		pr_debug("Interrupt is already set (enable = %d).\n", enable);
	else {
		info->irq_enabled = enable;
		if (enable) {
			enable_irq(getClient()->irq);
			pr_debug("Interrupt enabled.\n");
		} else {
			disable_irq_nosync(getClient()->irq);
			pr_debug("Interrupt disabled.\n");
		}
	}

	spin_unlock_irqrestore(&info->fts_int, flag);
	return OK;
}

/**
  * Check if there is a crc error in the IC which prevent the fw to run.
  * @return  OK if no CRC error, or a number >OK according the CRC error found
  */
int fts_crc_check(void)
{
	u8 val;
	u8 crc_status;
	int res;
	u8 error_to_search[6] = { EVT_TYPE_ERROR_CRC_CFG_HEAD,
				  EVT_TYPE_ERROR_CRC_CFG,
				  EVT_TYPE_ERROR_CRC_CX,
				  EVT_TYPE_ERROR_CRC_CX_HEAD,
				  EVT_TYPE_ERROR_CRC_CX_SUB,
				  EVT_TYPE_ERROR_CRC_CX_SUB_HEAD };


	res = fts_writeReadU8UX(FTS_CMD_HW_REG_R, ADDR_SIZE_HW_REG, ADDR_CRC,
				&val, 1, DUMMY_HW_REG);
	/* read 2 bytes because the first one is a dummy byte! */
	if (res < OK) {
		pr_err("%s Cannot read crc status ERROR %08X\n", __func__, res);
		return res;
	}

	crc_status = val & CRC_MASK;
	if (crc_status != OK) {	/* CRC error if crc_status!=0 */
		pr_err("%s CRC ERROR = %02X\n", __func__, crc_status);
		return CRC_CODE;
	}

	pr_info("%s: Verifying if Config CRC Error...\n", __func__);
	res = fts_system_reset();
	if (res >= OK) {
		res = pollForErrorType(error_to_search, 2);
		if (res < OK) {
			pr_info("%s: No Config CRC Error Found!\n", __func__);
			pr_info("%s: Verifying if Cx CRC Error...\n", __func__);
			res = pollForErrorType(&error_to_search[2], 4);
			if (res < OK) {
				pr_info("%s: No Cx CRC Error Found!\n",
					__func__);
				return OK;
			} else {
				pr_err("%s: Cx CRC Error found! CRC ERROR = %02X\n",
					__func__, res);
				return CRC_CX;
			}
		} else {
			pr_err("%s: Config CRC Error found! CRC ERROR = %02X\n",
				__func__, res);
			return CRC_CONFIG;
		}
	} else {
		pr_err("%s: Error while executing system reset! ERROR %08X\n",
			__func__, res);
		return res;
	}

	return OK;
}

/**
  * Request a host data and use the sync method to understand when the FW load
  * it
  * @param type the type ID of host data to load (@link load_opt Load Host Data
  * Option  @endlink)
  * @return OK if success or an error code which specify the type of error
  */
int requestSyncFrame(u8 type)
{
	u8 request[3] = { FTS_CMD_SYSTEM, SYS_CMD_LOAD_DATA, type };
	u8 readData[DATA_HEADER] = { 0 };
	int ret, retry = 0, retry2 = 0, time_to_count;
	int count, new_count;
	u8 cmd[7] = {0xFA, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};

	pr_info("%s: Starting to get a sync frame...\n", __func__);

	while (retry2 < RETRY_MAX_REQU_DATA) {
		pr_info("%s: Reading count...\n", __func__);

		ret = fts_writeReadU8UX(FTS_CMD_FRAMEBUFFER_R, BITS_16,
					ADDR_FRAMEBUFFER, readData, DATA_HEADER,
					DUMMY_FRAMEBUFFER);
		if (ret < OK) {
			pr_err("%s: Error while reading count! ERROR %08X\n",
				__func__, ret | ERROR_REQU_DATA);
			ret |= ERROR_REQU_DATA;
			retry2++;
			continue;
		}

		if (readData[0] != HEADER_SIGNATURE)
			pr_err("%s: Invalid Signature while reading count! ERROR %08X\n",
				__func__, ret | ERROR_REQU_DATA);

		count = (readData[3] << 8) | readData[2];
		new_count = count;
		pr_info("%s: Base count = %d\n", __func__, count);

		pr_info("%s: Requesting frame %02X  attempt = %d\n",
			__func__,  type, retry2 + 1);
		ret = fts_write(cmd, 7);
		if(ret >= OK)
			ret = fts_write(request, ARRAY_SIZE(request));
		if (ret >= OK) {
			pr_info("%s: Polling for new count...\n", __func__);
			time_to_count = TIMEOUT_REQU_DATA / TIMEOUT_RESOLUTION;
			while (count == new_count && retry < time_to_count) {
				ret = fts_writeReadU8UX(FTS_CMD_FRAMEBUFFER_R,
							BITS_16,
							ADDR_FRAMEBUFFER,
							readData,
							DATA_HEADER,
							DUMMY_FRAMEBUFFER);
				if ((ret >= OK) && (readData[0] ==
						    HEADER_SIGNATURE) &&
				    (readData[1] == type))
					new_count = ((readData[3] << 8) |
						     readData[2]);
				else
					pr_err("%s: invalid Signature or can not read count... ERROR %08X\n",
						__func__, ret);
				retry++;
				mdelay(TIMEOUT_RESOLUTION);
			}

			if (count == new_count) {
				pr_err("%s: New count not received! ERROR %08X\n",
					__func__,
					ERROR_TIMEOUT | ERROR_REQU_DATA);
				ret = ERROR_TIMEOUT | ERROR_REQU_DATA;
			} else {
				pr_info("%s: New count found! count = %d! Frame ready!\n",
					__func__, new_count);
				return OK;
			}
		}
		retry2++;
	}
	pr_err("%s: Request Data failed! ERROR %08X\n", __func__, ret);
	return ret;
}

/**
  * Set the Active Scanning Frequency to a defined value
  * @param freq scanning frequency to set in Hz
  * @return OK if success or an error code which specify the type of error
  * @warning The scan frequency can be set only for the MS scan!
  */
int setActiveScanFrequency(u32 freq)
{
	int res;
	u8 temp[2] = { 0 };
	u16 t_cycle;

	pr_info("%s: Setting the scanning frequency to %uHz...\n",
		__func__, freq);

	/* read MRN register */
	res = readConfig(ADDR_CONFIG_MRN, temp, 1);
	if (res < OK) {
		pr_err("%s: error while reading mrn count! ERROR %08X\n",
			__func__, res);
		return res;
	}

	/* setting r count to 0 (= 1 R cycle used) and write it back */
	temp[0] &= (~0x03);
	res = writeConfig(ADDR_CONFIG_MRN, temp, 1);
	if (res < OK) {
		pr_err("%s: error while writing mrn count! ERROR %08X\n",
			__func__, res);
		return res;
	}

	/* set first R cycle slot according the specified frequency */
	/* read T cycle */
	res = readConfig(ADDR_CONFIG_T_CYCLE, temp, 2);
	if (res < OK) {
		pr_err("%s: error while reading T cycle! ERROR %08X\n",
			__func__, res);
		return res;
	}
	t_cycle = ((u16)(temp[1] << 8)) | temp[0];


	/* compute the value of R cycle according the formula
	  * scan_freq = 30Mhz/(2*(T_cycle+R_cycle)) */
	temp[0] = (30000000) / (freq * 2) - t_cycle;
	/* write R cycle in Config Area */
	pr_info("%s: T cycle  = %d (0x%04X) => R0 cycle = %d (0x%02X)\n",
		__func__, t_cycle, t_cycle, temp[0], temp[0]);
	res = writeConfig(ADDR_CONFIG_R0_CYCLE, temp, 1);
	if (res < OK) {
		pr_err("%s: error while writing R0 cycle! ERROR %08X\n",
			__func__, res);
		return res;
	}

	pr_info("%s: Saving Config into the flash ...\n", __func__);
	/* save config */
	temp[0] = SAVE_FW_CONF;
	res = writeSysCmd(SYS_CMD_SAVE_FLASH, temp, 1);
	if (res < OK) {
		pr_err("%s: error while saving config into the flash! ERROR %08X\n",
			__func__, res);
		return res;
	}

	/* system reset */
	res = fts_system_reset();
	if (res < OK) {
		pr_err("%s: error at system reset! ERROR %08X\n",
			__func__, res);
		return res;
	}

	pr_info("%s: Setting the scanning frequency FINISHED!\n", __func__);
	return OK;
}

/**
  * Write Host Data Memory
  * @param type type of data to write
  * @param data pointer to the data which are written
  * @param msForceLen number of force (Tx) channels used with Mutual
  * @param msSenseLen number of sense (Rx) channels used with Mutual
  * @param ssForceLen number of force (Tx) channel used with Self
  * @param ssSenseLen number of sense (Rx) channel used with Self
  * @param save if =1 will save the host data written into the flash
  * @return OK if success or an error code which specify the type of error
  */
int writeHostDataMemory(u8 type, u8 *data, u8 msForceLen, u8 msSenseLen,
			u8 ssForceLen, u8 ssSenseLen, int save)
{
	int res;
	int size = (msForceLen * msSenseLen) + (ssForceLen + ssSenseLen);
	u8 sett = SPECIAL_WRITE_HOST_MEM_TO_FLASH;
	u8 *temp;

	temp = kzalloc((size + SYNCFRAME_DATA_HEADER) * sizeof(u8), GFP_KERNEL);
	if (temp == NULL)
                return ERROR_ALLOC;

	pr_info("%s: Starting to write Host Data Memory\n", __func__);

	temp[0] = 0x5A;
	temp[1] = type;
	temp[5] = msForceLen;
	temp[6] = msSenseLen;
	temp[7] = ssForceLen;
	temp[8] = ssSenseLen;

	memcpy(&temp[16], data, size);

	pr_info("%s: Write Host Data Memory in buffer...\n", __func__);
	res = fts_writeU8UX(FTS_CMD_FRAMEBUFFER_W, BITS_16,
			    ADDR_FRAMEBUFFER, temp, size +
			    SYNCFRAME_DATA_HEADER);

	if (res < OK) {
		pr_err("%s: error while writing the buffer! ERROR %08X\n",
			__func__, res);
		kfree(temp);
		return res;
	}

	/* save host data memory into the flash */
	if (save == 1) {
		pr_info("%s: Trigger writing into the flash...\n", __func__);
		res = writeSysCmd(SYS_CMD_SPECIAL, &sett, 1);
		if (res < OK) {
			pr_err("%s: error while writing into the flash! ERROR %08X\n",
				__func__, res);
			kfree(temp);
			return res;
		}
	}


	pr_info("%s: write Host Data Memory FINISHED!\n", __func__);
	kfree(temp);
	return OK;
}

/*
 * Save MP flag value into the flash
 * @param mpflag Value to write in the MP Flag field
 * @return OK if success or an error code which specify the type of error
 */
int saveMpFlag(u8 mpflag)
{
	int ret = OK;
	u8 panelCfg = SAVE_PANEL_CONF;

	pr_info("%s: Saving MP Flag = %02X\n", __func__, mpflag);
	ret |= writeSysCmd(SYS_CMD_MP_FLAG, &mpflag, 1);
	if (ret < OK)
		pr_err("%s: Error while writing MP flag on ram... ERROR %08X\n",
			__func__, ret);

	ret |= writeSysCmd(SYS_CMD_SAVE_FLASH, &panelCfg, 1);
	if (ret < OK)
		pr_err("%s: Error while saving MP flag on flash... ERROR %08X\n",
			__func__, ret);

	ret |= readSysInfo(1);
	if (ret < OK) {
		pr_err("%s: Error while refreshing SysInfo... ERROR %08X\n",
			__func__, ret);
		return ret;
	}

	pr_info("%s: Saving MP Flag OK!\n", __func__);
	return OK;
}
