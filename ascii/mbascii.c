/*
 * FreeModbus Libary: A portable Modbus implementation for Modbus ASCII/RTU.
 * Copyright (c) 2006 Christian Walter <wolti@sil.at>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * File: $Id: mbascii.c, v 1.17 2010/06/06 13:47:07 wolti Exp $
 */

/* ----------------------- System includes ----------------------------------*/
//#include <stdlib.h>
//#include <string.h>
/* ----------------------- Modbus includes ----------------------------------*/
#include <mb.h>

//#include <mbconfig.h>
//#include <mb_types.h>
//
//#include <serial_port.h>
//
//#include <mbport.h>
//#include <mbframe.h>
//#include <mbascii.h>
//#include <mb.h>
//#include <mbcrc.h>

#if MB_ASCII_ENABLED > 0

#define eSndState         inst->snd_state
#define eRcvState         inst->rcv_state

#define ucASCIIRcvBuf     inst->rcv_buf
#define ucASCIISndBuf     inst->snd_buf

#define usRcvBufferPos    inst->rcv_buf_pos
#define eBytePos          inst->byte_pos
#define xFrameIsBroadcast inst->frame_is_broadcast
#define eCurTimerMode     inst->cur_tmr_mode
#define usSendPDULength   inst->snd_pdu_len
#define pucSndBufferCur   inst->snd_buf_cur
#define usSndBufferCount  inst->snd_buf_cnt

#define asciiMaster       inst->is_master
#define ucMBLFCharacter   inst->mb_lf_char

const mb_tr_mtab mb_ascii_mtab =
{
    .frm_start   = (mb_frm_start_fp)  mb_ascii_start,
    .frm_stop    = (mb_frm_stop_fp)   mb_ascii_stop,
    .frm_send    = (mb_frm_snd_fp)   mb_ascii_send,
    .frm_rcv     = (mb_frm_rcv_fp)mb_ascii_receive,

    .get_rx_frm      = NULL,
    .get_tx_frm      = (mb_get_tx_frm_fp)mb_ascii_get_snd_buf
#   if MB_MASTER > 0
    , .rq_is_broadcast = (mb_mstr_rq_is_bcast_fp)mb_ascii_rq_is_bcast
#   endif //master
};

/* ----------------------- Static functions ---------------------------------*/
static UCHAR    prvucMBCHAR2BIN(UCHAR ucCharacter           );
static UCHAR    prvucMBBIN2CHAR(UCHAR byte_val                );
static UCHAR    prvucMBLRC     (UCHAR * frame_ptr, USHORT len_buf);
/* ----------------------- Start implementation -----------------------------*/
mb_err_enum mb_ascii_init(mb_ascii_tr_struct* inst, BOOL is_master, UCHAR slv_addr, ULONG baud, mb_port_ser_parity_enum parity)
{
    mb_err_enum    eStatus = MB_ENOERR;

    static const mb_port_cb_struct mb_ascii_cb =
    {
        .byte_rcvd   = (mb_port_cb_fp)mb_ascii_rcv_fsm,
        .tx_empty    = (mb_port_cb_fp)mb_ascii_snd_fsm,
        .tmr_expired = (mb_port_cb_fp)mb_ascii_tmr_1s_expired
    };

    (void)slv_addr;
    inst->base.port_obj->cb  = (mb_port_cb_struct *)&mb_ascii_cb;
    inst->base.port_obj->arg = inst;
    inst->is_master          = is_master;

    usSndBufferCount=0;

    ENTER_CRITICAL_SECTION();
    ucMBLFCharacter = MB_ASCII_DEFAULT_LF;

    if (mb_port_ser_init((mb_port_ser *)inst->base.port_obj, baud, 7, parity) != TRUE)
    {
        eStatus = MB_EPORTERR;
    }
    else if (mb_port_ser_tmr_init((mb_port_ser *)inst->base.port_obj, MB_ASCII_TIMEOUT_SEC * 20000UL) != TRUE)
    {
        eStatus = MB_EPORTERR;
    }

    eRcvState =  MB_ASCII_RX_STATE_IDLE;
    eSndState = MB_ASCII_TX_STATE_IDLE;
    //inst->serial_port.parent = (void*)(inst);

    EXIT_CRITICAL_SECTION();

    return eStatus;
}

void mb_ascii_start(mb_ascii_tr_struct* inst)
{
    ENTER_CRITICAL_SECTION();
    mb_port_ser_enable((mb_port_ser *)inst->base.port_obj, TRUE, FALSE);
    eRcvState = MB_ASCII_RX_STATE_IDLE;
    EXIT_CRITICAL_SECTION();

    /* No special startup required for ASCII. */
    (void)mb_port_ser_evt_post((mb_port_ser *)inst->base.port_obj, EV_READY);
}

void mb_ascii_stop(mb_ascii_tr_struct* inst)
{
    ENTER_CRITICAL_SECTION();
    mb_port_ser_enable((mb_port_ser *)inst->base.port_obj, FALSE, FALSE);
    mb_port_ser_tmr_disable((mb_port_ser *)inst->base.port_obj);
    EXIT_CRITICAL_SECTION();
}

mb_err_enum mb_ascii_receive(mb_ascii_tr_struct* inst,  UCHAR * rcv_addr_buf, UCHAR ** frame_ptr_buf, USHORT * len_buf)
{
    mb_err_enum    eStatus = MB_ENOERR;

    ENTER_CRITICAL_SECTION();
    assert(usRcvBufferPos < MB_ASCII_SER_PDU_SIZE_MAX);

    /* Length and CRC check */
    if ((usRcvBufferPos >= MB_ASCII_SER_PDU_SIZE_MIN)
            && (prvucMBLRC((UCHAR *) ucASCIIRcvBuf, usRcvBufferPos) == 0))
    {
        /* Save the address field. All frames are passed to the upper layed
         * and the decision if a frame is used is done there.
         */
        *rcv_addr_buf = ucASCIIRcvBuf[MB_ASCII_SER_PDU_ADDR_OFF];

        /* Total length of Modbus-PDU is Modbus-Serial-Line-PDU minus
         * size of address field and CRC checksum.
         */
        *len_buf = (USHORT)(usRcvBufferPos - MB_ASCII_SER_PDU_PDU_OFF - MB_ASCII_SER_PDU_SIZE_LRC);

        /* Return the start of the Modbus PDU to the caller. */
        *frame_ptr_buf = (UCHAR *) & ucASCIIRcvBuf[MB_ASCII_SER_PDU_PDU_OFF];
    }
    else
    {
        eStatus = MB_EIO;
    }
    EXIT_CRITICAL_SECTION();
    return eStatus;
}

mb_err_enum mb_ascii_send(mb_ascii_tr_struct* inst,  UCHAR slv_addr, const UCHAR * frame_ptr, USHORT len)
{
    mb_err_enum    eStatus = MB_ENOERR;
    UCHAR           usLRC;

    ENTER_CRITICAL_SECTION();
    /* Check if the receiver is still in idle state. If not we where too
     * slow with processing the received frame and the master sent another
     * frame on the network. We have to abort sending the frame.
     */
    if (eRcvState == MB_ASCII_RX_STATE_IDLE)
    {
        /* First byte before the Modbus-PDU is the slave address. */
        pucSndBufferCur = (UCHAR *) frame_ptr - 1;
        usSndBufferCount = 1;

        /* Now copy the Modbus-PDU into the Modbus-Serial-Line-PDU. */
        pucSndBufferCur[MB_ASCII_SER_PDU_ADDR_OFF] = slv_addr;
        usSndBufferCount += len;

        /* Calculate LRC checksum for Modbus-Serial-Line-PDU. */
        usLRC = prvucMBLRC((UCHAR *) pucSndBufferCur, usSndBufferCount);
        ucASCIISndBuf[usSndBufferCount++] = usLRC;

        /* Activate the transmitter. */
        eSndState = MB_ASCII_TX_STATE_START;
        mb_port_ser_enable((mb_port_ser *)inst->base.port_obj, FALSE, TRUE);
    }
    else
    {
        eStatus = MB_EIO;
    }
    EXIT_CRITICAL_SECTION();
    return eStatus;
}

BOOL mb_ascii_rcv_fsm(mb_ascii_tr_struct* inst)
{
    BOOL            xNeedPoll = FALSE;
    UCHAR           byte_val;
    UCHAR           ucResult;

    assert((eSndState == MB_ASCII_TX_STATE_IDLE)|| (eSndState == MB_ASCII_TX_STATE_XFWR));

    (void)mb_port_ser_get_byte((mb_port_ser *)inst->base.port_obj, (CHAR *) & byte_val);
    switch (eRcvState)
    {
    /* A new character is received. If the character is a ':' the input
     * buffer is cleared. A CR-character signals the end of the data
     * block. Other characters are part of the data block and their
     * ASCII value is converted back to a binary representation.
     */
    case MB_ASCII_RX_STATE_RCV:
        /* Enable timer for character timeout. */
        mb_port_ser_tmr_enable((mb_port_ser *)inst->base.port_obj);
        if (byte_val == ':')
        {
            /* Empty receive buffer. */
            eBytePos = BYTE_HIGH_NIBBLE;
            usRcvBufferPos = 0;
        }
        else if (byte_val == MB_ASCII_DEFAULT_CR)
        {
            eRcvState = MB_ASCII_RX_STATE_WAIT_EOF;
        }
        else
        {
            ucResult = prvucMBCHAR2BIN(byte_val);
            switch (eBytePos)
            {
            /* High nibble of the byte comes first. We check for
             * a buffer overflow here. */
            case BYTE_HIGH_NIBBLE:
                if (usRcvBufferPos < MB_ASCII_SER_PDU_SIZE_MAX)
                {
                    ucASCIIRcvBuf[usRcvBufferPos] = (UCHAR)(ucResult << 4);
                    eBytePos = BYTE_LOW_NIBBLE;
                    break;
                }
                else
                {
                    /* not handled in Modbus specification but seems
                     * a resonable implementation. */
                    eRcvState = MB_ASCII_RX_STATE_IDLE;
                    /* Disable previously activated timer because of error state. */
                    mb_port_ser_tmr_disable((mb_port_ser *)inst->base.port_obj);
                }
                break;

            case BYTE_LOW_NIBBLE:
                ucASCIIRcvBuf[usRcvBufferPos] |= ucResult;
                usRcvBufferPos++;
                eBytePos = BYTE_HIGH_NIBBLE;
                break;
            }
        }
        break;

    case MB_ASCII_RX_STATE_WAIT_EOF:
        if (byte_val == ucMBLFCharacter)
        {
            /* Disable character timeout timer because all characters are
             * received. */
            mb_port_ser_tmr_disable((mb_port_ser *)inst->base.port_obj);
            /* Receiver is again in idle state. */
            eRcvState = MB_ASCII_RX_STATE_IDLE;

            /* Notify the caller of mb_ascii_receive that a new frame
             * was received. */
            xNeedPoll = mb_port_ser_evt_post((mb_port_ser *)inst->base.port_obj, EV_FRAME_RECEIVED);
        }
        else if (byte_val == ':')
        {
            /* Empty receive buffer and back to receive state. */
            eBytePos = BYTE_HIGH_NIBBLE;
            usRcvBufferPos = 0;
            eRcvState = MB_ASCII_RX_STATE_RCV;

            /* Enable timer for character timeout. */
            mb_port_ser_tmr_enable((mb_port_ser *)inst->base.port_obj);
        }
        else
        {
            /* Frame is not okay. Delete entire frame. */
            eRcvState = MB_ASCII_RX_STATE_IDLE;
        }
        break;

    case MB_ASCII_RX_STATE_IDLE:
        if (byte_val == ':')
        {
#if MB_MASTER > 0
            if (asciiMaster == TRUE)
            {
                mb_port_ser_tmr_disable((mb_port_ser *)inst->base.port_obj);
                eSndState = MB_ASCII_TX_STATE_IDLE;
            }
#endif
            /* Enable timer for character timeout. */
            mb_port_ser_tmr_enable((mb_port_ser *)inst->base.port_obj);
            /* Reset the input buffers to store the frame. */
            usRcvBufferPos = 0;;
            eBytePos = BYTE_HIGH_NIBBLE;
            eRcvState = MB_ASCII_RX_STATE_RCV;
        }
        break;
    }

    return xNeedPoll;
}

BOOL mb_ascii_snd_fsm(mb_ascii_tr_struct* inst)
{
    BOOL            xNeedPoll = FALSE;
    UCHAR           byte_val;

    assert(eRcvState == MB_ASCII_RX_STATE_IDLE);
    switch (eSndState)
    {
    /* Start of transmission. The start of a frame is defined by sending
     * the character ':'. */
    case MB_ASCII_TX_STATE_START:
        byte_val = ':';
        mb_port_ser_put_byte((mb_port_ser *)inst->base.port_obj, (CHAR)byte_val);
        eSndState = MB_ASCII_TX_STATE_DATA;
        eBytePos = BYTE_HIGH_NIBBLE;
        break;

    /* Send the data block. Each data byte is encoded as a character hex
     * stream with the high nibble sent first and the low nibble sent
     * last. If all data bytes are exhausted we send a '\r' character
     * to end the transmission. */
    case MB_ASCII_TX_STATE_DATA:
        if (usSndBufferCount > 0)
        {
            switch (eBytePos)
            {
            case BYTE_HIGH_NIBBLE:
                byte_val = prvucMBBIN2CHAR((UCHAR)(*pucSndBufferCur >> 4));
                mb_port_ser_put_byte((mb_port_ser *)inst->base.port_obj, (CHAR) byte_val);
                eBytePos = BYTE_LOW_NIBBLE;
                break;

            case BYTE_LOW_NIBBLE:
                byte_val = prvucMBBIN2CHAR((UCHAR)(*pucSndBufferCur & 0x0F));
                mb_port_ser_put_byte((mb_port_ser *)inst->base.port_obj, (CHAR)byte_val);
                pucSndBufferCur++;
                eBytePos = BYTE_HIGH_NIBBLE;
                usSndBufferCount--;
                break;
            }
        }
        else
        {
            mb_port_ser_put_byte((mb_port_ser *)inst->base.port_obj, MB_ASCII_DEFAULT_CR);
            eSndState = MB_ASCII_TX_STATE_END;
        }
        break;

    /* Finish the frame by sending a LF character. */
    case MB_ASCII_TX_STATE_END:
        mb_port_ser_put_byte((mb_port_ser *)inst->base.port_obj, (CHAR)ucMBLFCharacter);
        /* We need another state to make sure that the CR character has
         * been sent. */
        eSndState = MB_ASCII_TX_STATE_NOTIFY;
        break;

    /* Notify the task which called mb_ascii_send that the frame has
     * been sent. */
    case MB_ASCII_TX_STATE_NOTIFY:
#if MB_MASTER >0
        if (asciiMaster==TRUE)
        {

            xFrameIsBroadcast = (ucASCIISndBuf[MB_ASCII_SER_PDU_ADDR_OFF] == MB_ADDRESS_BROADCAST) ? TRUE : FALSE;
            /* Disable transmitter. This prevents another transmit buffer
             * empty interrupt. */
            mb_port_ser_enable((mb_port_ser *)inst->base.port_obj, TRUE, FALSE);
            eSndState = MB_ASCII_TX_STATE_XFWR;
            /* If the frame is broadcast , master will enable timer of convert delay,
             * else master will enable timer of respond timeout. */
            if (xFrameIsBroadcast == TRUE)
            {
                mb_port_ser_tmr_convert_delay_enable((mb_port_ser *)inst->base.port_obj);
            }
            else
            {
                mb_port_ser_tmr_respond_timeout_enable((mb_port_ser *)inst->base.port_obj);
            }

        }
        else
#endif
        {
            eSndState = MB_ASCII_TX_STATE_IDLE;
            xNeedPoll = mb_port_ser_evt_post((mb_port_ser *)inst->base.port_obj, EV_FRAME_SENT);

            /* Disable transmitter. This prevents another transmit buffer
             * empty interrupt. */
            mb_port_ser_enable((mb_port_ser *)inst->base.port_obj, TRUE, FALSE);
            eSndState = MB_ASCII_TX_STATE_IDLE;
        }
        break;

    /* We should not get a transmitter event if the transmitter is in
     * idle state.  */
    case MB_ASCII_TX_STATE_IDLE:
        /* enable receiver/disable transmitter. */
        mb_port_ser_enable((mb_port_ser *)inst->base.port_obj, TRUE, FALSE);
        break;
    default:
        break;
    }

    return xNeedPoll;
}

BOOL mb_ascii_tmr_1s_expired(mb_ascii_tr_struct* inst)
{
    BOOL            xNeedPoll = FALSE;
    switch (eRcvState)
    {
    /* If we have a timeout we go back to the idle state and wait for
     * the next frame.
     */
    case MB_ASCII_RX_STATE_RCV:
    case MB_ASCII_RX_STATE_WAIT_EOF:
        eRcvState = MB_ASCII_RX_STATE_IDLE;
        break;

    default:
        assert((eRcvState == MB_ASCII_RX_STATE_RCV) || (eRcvState == MB_ASCII_RX_STATE_WAIT_EOF));
        break;
    }
    mb_port_ser_tmr_disable((mb_port_ser *)inst->base.port_obj);


#if MB_MASTER >0
    if (asciiMaster == TRUE)
    {
        switch (eSndState)
        {
        /* A frame was send finish and convert delay or respond timeout expired.
         * If the frame is broadcast, The master will idle, and if the frame is not
         * broadcast.Notify the listener process error.*/
        case MB_ASCII_TX_STATE_XFWR:
            if (xFrameIsBroadcast == FALSE)
            {
                //((mb_instance*)(inst->parent))->master_err_cur = ERR_EV_ERROR_RESPOND_TIMEOUT;
                //vMBSetErrorType(ERR_EV_ERROR_RESPOND_TIMEOUT); //FIXME pass reference to instance
                //xNeedPoll = mb_port_ser_evt_post((mb_port_ser *)inst->base.port_obj, EV_ERROR_PROCESS);
                xNeedPoll = mb_port_ser_evt_post((mb_port_ser *)inst->base.port_obj, EV_MASTER_ERROR_RESPOND_TIMEOUT);
            }
            break;
        /* Function called in an illegal state. */
        default:
            assert(
                (eSndState == MB_ASCII_TX_STATE_XFWR) || (eSndState == MB_ASCII_TX_STATE_IDLE));
            break;
        }
        eSndState = MB_ASCII_TX_STATE_IDLE;

        mb_port_ser_tmr_disable((mb_port_ser *)inst->base.port_obj);
        /* If timer mode is convert delay, the master event then turns EV_MASTER_EXECUTE status. */
        if (eCurTimerMode == MB_TMODE_CONVERT_DELAY)
        {
            xNeedPoll = mb_port_ser_evt_post((mb_port_ser *)inst->base.port_obj, EV_EXECUTE);
        }
    }
#endif

    /* no context switch required. */
    return xNeedPoll;
}


static UCHAR prvucMBCHAR2BIN(UCHAR ucCharacter)
{
    if ((ucCharacter >= '0') && (ucCharacter <= '9'))
    {
        return (UCHAR)(ucCharacter - '0');
    }
    else if ((ucCharacter >= 'A') && (ucCharacter <= 'F'))
    {
        return (UCHAR)(ucCharacter - 'A' + 0x0A);
    }
    else
    {
        return 0xFF;
    }
}

static UCHAR prvucMBBIN2CHAR(UCHAR byte_val)
{
    if (byte_val <= 0x09)
    {
        return (UCHAR)('0' + byte_val);
    }
    else if ((byte_val >= 0x0A) && (byte_val <= 0x0F))
    {
        return (UCHAR)(byte_val - 0x0A + 'A');
    }
    else
    {
        /* Programming error. */
        assert(0);
    }
    return '0';
}


static UCHAR prvucMBLRC(UCHAR * frame_ptr, USHORT len_buf)
{
    UCHAR ucLRC = 0;  /* LRC char initialized */

    while (len_buf--)
    {
        ucLRC += *frame_ptr++;   /* Add buffer byte without carry */
    }

    /* Return twos complement */
    ucLRC = (UCHAR) (-((CHAR) ucLRC));
    return ucLRC;
}

/* Get Modbus send PDU's buffer address pointer.*/
void mb_ascii_get_snd_buf(mb_ascii_tr_struct* inst, UCHAR ** frame_ptr_buf)
{
    *frame_ptr_buf = (UCHAR *) &ucASCIISndBuf[MB_ASCII_SER_PDU_PDU_OFF];
}



/* Get Modbus Master send RTU's buffer address pointer.*/
void vMBASCIIMasterGetRTUSndBuf(mb_ascii_tr_struct* inst, UCHAR ** frame_ptr_buf)
{
    *frame_ptr_buf = (UCHAR *) ucASCIISndBuf;
}

/* Set Modbus Master send PDU's buffer length.*/
void mb_ascii_set_snd_len(mb_ascii_tr_struct* inst, USHORT snd_pdu_len)
{
    usSendPDULength = snd_pdu_len;
}

/* Get Modbus Master send PDU's buffer length.*/
USHORT mb_ascii_get_snd_len(mb_ascii_tr_struct* inst)
{
    return usSendPDULength;
}

/* Set Modbus Master current timer mode.*/
void mb_ascii_set_cur_tmr_mode(mb_ascii_tr_struct* inst, mb_tmr_mode_enum tmr_mode)
{
    eCurTimerMode = tmr_mode;
}

/* The master request is broadcast? */
BOOL mb_ascii_rq_is_bcast(mb_ascii_tr_struct* inst)
{
    return xFrameIsBroadcast;
}

#endif
