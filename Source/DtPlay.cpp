//#*#*#*#*#*#*#*#*#*#*#*#*#*#*#*#*#*#*#* DtPlay.cpp *#*#*#*#*#*#*#*#* (C) 2000-2017 DekTec
//


/*
TODO: jjustman-2020-01-17:

file:///C:/Program%20Files%20(x86)/DekTec/SDKs/WinSDK/DTAPI/Doc/DTAPI%20Reference%20-%20Multi-PLP%20Extensions.pdf


DtAtsc3PlpPars
    int  m_Modulation;          // Modulation, see DTAPI_ATSC3_QAMx
    int  m_CodeRate;            // Code rate, see DTAPI_ATSC3_COD_x

    class DtAtsc3Pars;

    Atsc3()


    DtAtsc3ParamInfo


    The DtMplpOutpChannel::SetModControl() method sets the parameters for the ATSC 3.0 modulator. The multi-PLP modulator can be used for both single-PLP and multi-PLP parameter sets.
    
    The DtMplpOutpChannel::WriteMplpPacket method is used to write data to the output channel.

    DtMplpOutpChannel::SetModControl Set modulation-control parameters for modulator channels. There are four overloads defined for the multi-PLP modulator output: one for ATSC 3.0, one for DVB-C2, one for DVB-T2 and one for ISDBTmm.
// Overload to be used for ATSC 3.0 DTAPI_RESULT DtMplpOutpChannel::SetModControl(  [in] DtAtsc3Pars&  Atsc3Pars // ATSC 3.0 modulation parameters );
DtAtsc3Pars



    DtAtsc3Pars::CheckValidity


*/
//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Include files -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
#include <stdio.h>
#include <sstream>

#include <DTAPI.h>

#include "DtPlay.h"
#include "DtSdiFileFmt.h"
#include "DtOpt.h"


#ifdef WINBUILD
    #include <Windows.h>
    #include <conio.h>
#else
    #include <stdlib.h>
    #include <string.h>
    #include <unistd.h>
    #include <limits.h>
    #include <termios.h>
#endif

#ifndef WINBUILD
void ChangeTerminalMode(int dir)
{
    static struct termios  OldT, NewT;

    if (dir == 1)
    {
        tcgetattr(STDIN_FILENO, &OldT);
        NewT = OldT;
        NewT.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &NewT);
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &OldT);
    }
}
int  _kbhit()
{
    struct timeval  tv;
    fd_set  rdfs;

    tv.tv_sec = 0;
    tv.tv_usec = 0;

    FD_ZERO(&rdfs);
    FD_SET(STDIN_FILENO, &rdfs);

    select(STDIN_FILENO+1, &rdfs, NULL, NULL, &tv);
    return FD_ISSET(STDIN_FILENO, &rdfs);
}
#endif


void atsc3_stltp_timing_management_collection_callback(atsc3_stltp_timing_management_packet_tv* atsc3_stltp_timing_management_packet_v, void* context) {
    Player* player = (Player*)context;
    //player->atsc3StltpTimingManagementCollectionCallback(atsc3_stltp_timing_management_packet_v);
}

void atsc3_stltp_preamble_packet_collection_callback(atsc3_stltp_preamble_packet_tv* atsc3_stltp_preamble_packet_v, void* context) {
    Player* player = (Player*)context;
    //player->atsc3StltpPreamblePacketCollectionCallback(atsc3_stltp_preamble_packet_v);
}

void atsc3_stltp_baseband_alp_packet_collection_callback(atsc3_alp_packet_collection_t* atsc3_alp_packet_collection, void* context) {
    Player* player = (Player*)context;
    player->atsc3StltpBasebandAlpPacketCollectionCallback(atsc3_alp_packet_collection);
}

void Player::atsc3StltpBasebandAlpPacketCollectionCallback(atsc3_alp_packet_collection_t* atsc3_alp_packet_collection) {

    lock_guard<mutex> atsc3_alp_packet_collection_queue_guard(atsc3_alp_packet_collection_queue_mutex);
    
    for (int i = 0; i < atsc3_alp_packet_collection->atsc3_alp_packet_v.count; i++) {
        atsc3_alp_packet_t* atsc3_alp_packet = atsc3_alp_packet_collection->atsc3_alp_packet_v.data[i];
        //jjustman-2020-09-02 todo: clone as we need to acquire ownership of these packets
        atsc3_alp_packet_collection_queue.push(atsc3_alp_packet_clone(atsc3_alp_packet));
    }

    if (atsc3_alp_packet_collection_queue.size() > 64) {
        atsc3_alp_packet_collection_queue_condition.notify_one();
    }
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- DtPlay Version -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
#define DTPLAY_VERSION_MAJOR        4
#define DTPLAY_VERSION_MINOR        12
#define DTPLAY_VERSION_BUGFIX       0

const int c_BufSize = 65507; //MAX PHY layer IP packet size from A/331:2019
const int c_MinFifoLoad = 3*1024*1024;  // Minimum fifo load before starting DVB transmission

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Error messages -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.

// Command line errors
const char c_CleInvalidArgument[]       = "Invalid argument for command line option: -%s";
const char c_CleNoPlayFile[]            = "No play file specified";
const char c_CleTxRateNotDefined[]      = "Transport-Stream rate not specified";

// General errors
const char c_ErrDtapiDvcScanFailed[]    = "DtapiDeviceScan failed (ERROR: %s)";
const char c_ErrNoOutputFound[]         = "No output device in the system";
const char c_ErrNoSuchPort[]            = "The %s has no port %d";
const char c_ErrHwScanFailed[]          = "HwFuncScan failed (ERROR: %s)";
const char c_ErrNoOutputPort[]          = "The %s port %d it not an output port";
const char c_ErrNoSuitablePort[]        = "No suitable port found on the %s";
const char c_ErrFailToAttachToChan[]    = "Can't attach to the channel (ERROR: %s)";
const char c_ErrFailsafeEnabled[]       = "DtPlay doesn't support failsafe but it is enabled. Use DtInfo to disable failsafe first.";
const char c_ErrFailToAttachToType[]    = "Failed to attach to the %s (ERROR: %s)";
const char c_ErrDvcNumWithoutType[]     = "Specifying a device number without a type is not supported";
const char c_ErrDblbufSelf[]            = "Can't enabled buffered output on the port itself";
const char c_ErrFailGetFlags[]          = "GetFlags failed (ERROR: %s)";
const char c_ErrFailToSetIoConfig[]     = "Failed to set IO-configuration (ERROR: %s)";
const char c_ErrFailToGetIoConfig[]     = "Failed to get IO-configuration (ERROR: %s)";
const char c_ErrFailToGetIoStd[]        = "DtapiVidStd2IoStd failed (ERROR: %s)";
const char c_ErrFailToGetVidStdInfo[]   = "DtapiGetVidStdInfo failed (ERROR: %s)";
const char c_ErrFailToOpenFile[]        = "Can't open '%ls' for reading";
const char c_ErrReadFile[]              = "File read error";
const char c_ErrPcapFormat[]            = "PCAP format error";
const char c_ErrIsdbS3TlvFormat[]       = "Invalid ISDB-S3 TLV stream format";
const char c_ErrFailSetTxControl[]      = "SetTxControl failed (ERROR: %s)";
const char c_ErrFailSetTxMode[]         = "SetTxMode failed (ERROR: %s)";
const char c_ErrFailSetModControl[]     = "SetModControl failed (ERROR: %s)";
const char c_ErrFailSetRfControl[]      = "SetRfControl failed (ERROR: %s)";
const char c_ErrFailSetIpPars[]         = "SetIpPars failed (ERROR: %s)";
const char c_ErrFailSetTsRate[]         = "SetTsrateBps failed (ERROR: %s)";
const char c_ErrFailWrite[]             = "Write failed (ERROR: %s)";
const char c_ErrInvalidFileSize[]       = "Invalid file size";
const char c_ErrInvalidDtSdiFileHdr[]   = "Invalid DTSDI file header";
const char c_ErrInvalidPcapFileHdr[]    = "Invalid PCAP file header";
const char c_ErrInvalidPcapLinkType[]   = "Invalid PCAP link-type";
const char c_ErrFailedToSetOutputLevel[]= "Failed to set output level (ERROR: %s)";
const char c_ErrFailedToSetSNR[]        = "Failed to set SNR (ERROR: %s)";
const char c_FailedToInitIdsbtPars[]    = "Failed to initialise ISDB-T parameters (ERROR: %s)";
const char c_ErrFailGetFifoSize[]       = "Failed to get Fifo size (ERROR: %s)";
const char c_ErrCmmbTsRateFromTs[]      = "Cannot retrieve rate from stream (ERROR: %s)";
const char c_ErrCpuUnderflow[]          = "CPU underflow detected";
const char c_ErrDmaUnderflow[]          = "Dma underflow detected";
const char c_ErrFifoUnderflow[]         = "Fifo underflow detected";

const char c_ErrDriverInCompPci[] =
    "The current Dta1xx driver (V%d.%d.%d %d) is not compatible with this\n" \
    "version of DtPlay.\n" \
    "Please install the latest version of the Dta1xx driver.";

const char c_ErrDriverInCompUsb[] =
    "The current Dtu2xx driver (V%d.%d.%d %d) is not compatible with this\n" \
    "version of DtPlay.\n" \
    "Please install the latest version of the Dtu2xx driver.";

static DtOptItem*  g_CmdOptions;
static int  g_NumCmdOptions;

static int wtoi(wstring Str)
{
    wistringstream Stream(Str);
    int  Result;
    Stream >> Result;
    return Result;
}


//+=+=+=+=+=+=+=+=+=+=+=+=+=+ CommandLineParams implementation +=+=+=+=+=+=+=+=+=+=+=+=+=+

//-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::CommandLineParams -.-.-.-.-.-.-.-.-.-.-.-.-
//
CommandLineParams::CommandLineParams()
{
}

//.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::~CommandLineParams -.-.-.-.-.-.-.-.-.-.-.-.-
//
CommandLineParams::~CommandLineParams()
{
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::Init -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
void CommandLineParams::Init()
{
    m_PlayDtSdiFile = false;
    m_PlayPcapFile = true;
}

//-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::ParseCommandLine -.-.-.-.-.-.-.-.-.-.-.-.-.
//
void CommandLineParams::ParseCommandLine(int argc, char* argv[])
{
    /*static const DtEnumOptPair  TransmitModes[] = {
        {L"188",        DTAPI_TXMODE_188,      L"188-byte packets in file, 188-byte packets transmitted"},
        {L"ADD16",      DTAPI_TXMODE_ADD16,    L"188-byte packets in file, 204-byte packets transmitted\n"
                                               L"(16 dummy bytes are automatically added to the packets)"},
        {L"192",        DTAPI_TXMODE_192,      L"192-byte packets in file, 192-byte packets transmitted\n"
                                               L"Note: this mode is only supported by the DTA-102"},
        {L"204",        DTAPI_TXMODE_204,      L"204-byte packets in file, 204-byte packets transmitted"},
        {L"MIN16",      DTAPI_TXMODE_MIN16,    L"204-byte packets in file, 188-byte packets transmitted\n"
                                               L"(last 16 bytes are invalidated)"},
        {L"RAW",        DTAPI_TXMODE_RAW,      L"No notion of packets, data is transmitted as is"},
        {L"SDI8B_625",  DTAPI_TXMODE_SDI_FULL, L"8-Bit/625-line SDI in file, 8-bit SDI transmitted"},
        {L"SDI8B_525",  DTAPI_TXMODE_SDI_FULL, L"8-Bit/525-line SDI in file, 8-bit SDI transmitted"},
        {L"DTSDI",      0,                     L".dtsdi file, SDI format is auto detected"},
        {L"RAWASI",     DTAPI_TXMODE_RAWASI,   L"Play-out of RAW ASI symbols (270Mbit/s)"},
        OPT_PAIR_END,
    };*/

    static const DtEnumOptPair  ModTypes[] = {
        {L"ATSC",         DTAPI_MOD_ATSC,          L"ATSC modulation"},
        {L"CMMB",         DTAPI_MOD_CMMB,          L"CMMB modulation"},
        {L"DAB",          DTAPI_MOD_DAB,           L"DAB modulation"},
        {L"DTMB",         DTAPI_MOD_DMBTH,         L"DTMB modulation"},
        {L"DVBS",         DTAPI_MOD_DVBS_QPSK,     L"DVB-S QPSK modulation"},
        {L"DVBS2_16APSK", DTAPI_MOD_DVBS2_16APSK,  L"DVB-S.2 16APSK modulation"},
        {L"DVBS2_32APSK", DTAPI_MOD_DVBS2_32APSK,  L"32APSK modulation"},
        {L"DVBS2_8PSK",   DTAPI_MOD_DVBS2_8PSK,    L"DVB-S.2 8PSK modulation"},
        {L"DVBS2_QPSK",   DTAPI_MOD_DVBS2_QPSK,    L"DVB-S.2 QPSK modulation"},
        {L"DVBS2_L3",     DTAPI_MOD_DVBS2_L3,      L"DVB-S.2 L3 modulation"},
        {L"DVBT",         DTAPI_MOD_DVBT,          L"DVB-T/H modulation"},
        {L"ISDBS",        DTAPI_MOD_ISDBS,         L"ISDB-S modulation"},
        {L"ISDBS3",       DTAPI_MOD_ISDBS3,        L"ISDB-S3 modulation"},
        {L"ISDBT",        DTAPI_MOD_ISDBT,         L"ISDB-T modulation"},
        {L"IQ",           DTAPI_MOD_IQDIRECT,      L"IQ direct"},
        {L"QAM4",         DTAPI_MOD_QAM4,          L"QAM-4 modulation"},
        {L"QAM16",        DTAPI_MOD_QAM16,         L"QAM-16 modulation"},
        {L"QAM32",        DTAPI_MOD_QAM32,         L"QAM-32 modulation"},
        {L"QAM64",        DTAPI_MOD_QAM64,         L"QAM-64 modulation"},
        {L"QAM128",       DTAPI_MOD_QAM128,        L"QAM-128 modulation"},
        {L"QAM256",       DTAPI_MOD_QAM256,        L"QAM-256 modulation"},
        {L"T2MI",         DTAPI_MOD_T2MI,          L"T2MI modulation"},
        {L"ATSC3",        DTAPI_MOD_ATSC3,         L"ATSC3 modulation"},

        OPT_PAIR_END,
    };

    static const DtEnumOptPair  J83Annex[] = {
        {L"A",            DTAPI_MOD_J83_A,         L"J.83 annex A (DVB-C)"},
        {L"B",            DTAPI_MOD_J83_B,         L"J.83 annex B ('American QAM')"},
        {L"C",            DTAPI_MOD_J83_C,         L"J.83 annex C ('Japanese QAM')"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  OfdmTxMode[] = {
        {L"2k",            DTAPI_MOD_DVBT_2K,      L"2k"},
        {L"4k",            DTAPI_MOD_DVBT_4K,      L"4k"},
        {L"8k",            DTAPI_MOD_DVBT_8K,      L"8k"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  GuardInterval[] = {
        {L"1/4",           DTAPI_MOD_DVBT_G_1_4,   L"1/4"},
        {L"1/8",           DTAPI_MOD_DVBT_G_1_8,   L"1/8"},
        {L"1/16",          DTAPI_MOD_DVBT_G_1_16,  L"1/16"},
        {L"1/32",          DTAPI_MOD_DVBT_G_1_32,  L"1/32"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  DtmbFrameHeaderMode[] = {
        {L"PN420",         DTAPI_MOD_DTMB_PN420,   L"PN420"},
        {L"PN595",         DTAPI_MOD_DTMB_PN595,   L"PN595"},
        {L"PN945",         DTAPI_MOD_DTMB_PN945,   L"PN945"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  DvbS2Pilots[] = {
        {L"OFF",           DTAPI_MOD_S2_NOPILOTS,  L"Pilots disabled"},
        {L"ON",            DTAPI_MOD_S2_PILOTS,    L"Pilots enabled"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  DvbS2FecFrames[] = {
        {L"SHORT",         DTAPI_MOD_S2_SHORTFRM,  L"Short FECFRAME"},
        {L"LONG",          DTAPI_MOD_S2_LONGFRM,   L"Long FECFRAME"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  IqInterpolFilter[] = {
        {L"OFDM",         DTAPI_MOD_INTERPOL_OFDM, L"OFDM interpolation filter"},
        {L"QAM",          DTAPI_MOD_INTERPOL_QAM,  L"QAM interpolation filter"},
        OPT_PAIR_END,
    };
    
    static const DtEnumOptPair  IpProtocol[] = {
        {L"UDP",          DTAPI_PROTO_UDP,         L"UDP"},
        {L"RTP",          DTAPI_PROTO_RTP,         L"RTP"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  StuffingMode[] = {
        {L"OFF",           DTAPI_TXSTUFF_MODE_OFF, L"ASI Null packet / SDI Black frame stuffing disabled"},
        {L"ON",            DTAPI_TXSTUFF_MODE_ON,  L"ASI Null packet / SDI Black frame stuffing enabled"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  DmbthCodeRate[] = {
        {L"0.4",           DTAPI_MOD_DTMB_0_4,     L"0.4"},
        {L"0.6",           DTAPI_MOD_DTMB_0_6,     L"0.6"},
        {L"0.8",           DTAPI_MOD_DTMB_0_8,     L"0.8"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  DefaultCodeRate[] = {
        {L"1/2",           DTAPI_MOD_1_2,          L"1/2"},
        {L"2/3",           DTAPI_MOD_2_3,          L"2/3"},
        {L"3/4",           DTAPI_MOD_3_4,          L"3/4"},
        {L"4/5",           DTAPI_MOD_4_5,          L"4/5"},
        {L"5/6",           DTAPI_MOD_5_6,          L"5/6"},
        {L"6/7",           DTAPI_MOD_6_7,          L"6/7"},
        {L"7/8",           DTAPI_MOD_7_8,          L"7/8"},
        {L"1/3",           DTAPI_MOD_1_3,          L"1/3"},
        {L"2/5",           DTAPI_MOD_2_5,          L"2/5"},
        {L"3/5",           DTAPI_MOD_3_5,          L"3/5"},
        {L"8/9",           DTAPI_MOD_8_9,          L"8/9"},
        {L"9/10",          DTAPI_MOD_9_10,         L"9/10"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  ConstellationAtsc[] = {
        {L"VSB8",           DTAPI_MOD_ATSC_VSB8,   L"VSB8"},
        {L"VSB16",          DTAPI_MOD_ATSC_VSB16,  L"VSB16"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  ConstallationDmbth[] = {
        {L"QAM4NR",         DTAPI_MOD_DTMB_QAM4NR, L"QAM4NR"},
        {L"QAM4",           DTAPI_MOD_DTMB_QAM4,   L"QAM4"},
        {L"QAM16",          DTAPI_MOD_DTMB_QAM16,  L"QAM16"},
        {L"QAM32",          DTAPI_MOD_DTMB_QAM32,  L"QAM32"},
        {L"QAM64",          DTAPI_MOD_DTMB_QAM64,  L"QAM64"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  ConstallationDefault[] = {
        {L"QPSK",           DTAPI_MOD_DVBT_QPSK,   L"VSB8"},
        {L"QAM16",          DTAPI_MOD_DVBT_QAM16,  L"QAM16"},
        {L"QAM64",          DTAPI_MOD_DVBT_QAM64,  L"QAM64"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  BandwithDvbt[] = {
        {L"5",              DTAPI_MOD_DVBT_5MHZ,   L"5 Mhz"},
        {L"6",              DTAPI_MOD_DVBT_6MHZ,   L"6 Mhz"},
        {L"7",              DTAPI_MOD_DVBT_7MHZ,   L"7 Mhz"},
        {L"8",              DTAPI_MOD_DVBT_8MHZ,   L"8 Mhz"},
        OPT_PAIR_END,
    };

    static const DtEnumOptPair  BandwithDmbth[] = {
        {L"5",              DTAPI_MOD_DTMB_5MHZ,   L"5 Mhz"},
        {L"6",              DTAPI_MOD_DTMB_6MHZ,   L"6 Mhz"},
        {L"7",              DTAPI_MOD_DTMB_7MHZ,   L"7 Mhz"},
        {L"8",              DTAPI_MOD_DTMB_8MHZ,   L"8 Mhz"},
        OPT_PAIR_END,
    };

    static DtOptItem CmdOptions[] = {
        DtOptItem(L"l",   m_LoopCnt, 0, L"Number of times to loop the file (0=loop infinitely)", 0, INT_MAX),
        DtOptItem(L"r",   m_TxRate, -1, L"Transport-Stream Rate in bps or sample rate in case of IQ-modulation mode\n"
                L"  NOTE: set the rate to '0' to playout a file with timestamps", 0, INT_MAX),
        DtOptItem(L"t",   m_DvcType, -1, L"Device type to use (default: any output device)\n"
                L"  100, 102, 105, 107, 110, 111, 112, 115, 116, 117, 140, 145, 160, 205,\n"
                L"  215, 245, 315, 2107, 2111, 2115, 2136, 2137, 2144, 2145, 2152, 2154,\n"
                L"  2174, 2175, 2179, 2195, 2160 or 2162", 100, 3999),
        DtOptItem(L"n",   m_DvcNum, 1, L"Device number to use (default: 1)", 1, 99),
        DtOptItem(L"i",   m_Port, -1, L"Port number of the output channel to use", 1, 12),
        DtOptItem(L"db",  m_DblBuff, -1, L"Port to use as doubly buffered output"),
       // DtOptItem(L"m",   m_TxMode, DTAPI_TXMODE_188, L"Transmit Mode (default: 188)", TransmitModes),
        DtOptItem(L"mt",  m_ModType, DTAPI_MOD_ATSC3, L"Modulation type (default: ATSC3)", ModTypes),
        DtOptItem(L"mf",  m_CarrierFreq, 0.0, L"Modulation carrier frequency in MHz  (default: UHF:647MHz))", 20.0, 2300.0),

        DtOptItem(L"ml",  m_OutpLevel, -10.0, L"Output level in dBm (default: -10.0dBm)", -35.0, 0.0),
        DtOptItem(L"mc",  m_CodeRate, L"Convolutional rate (default: 1/2)\n"
                L"  General        : 1/2, 2/3, 3/4, 4/5, 5/6, 6/7 or 7/8\n"
                L"  DVB-S2 Specific: 1/3, 2/5, 3/5, 8/9 or 9/10\n"
                L"  DTMB Specific  : 0.4, 0.6 or 0.8"),
        DtOptItem(L"ma",  m_QamJ83Annex, DTAPI_MOD_J83_A, L"J.83 Annex (default: Annex A)", J83Annex),
        DtOptItem(L"mC",  m_Constellation, L"ATSC/DVB-H/DVB-T/DTMB constellation (default: QAM64)\n"
                L"  QPSK       QPSK modulation\n"
                L"  QAM16      QAM-16 modulation\n"
                L"  QAM64      QAM-64 modulation\n"
                L"  -- DTMB Specific\n"
                L"  QAM4NR     QAM-4NR modulation\n"
                L"  QAM4       QAM-4 modulation\n"
                L"  QAM32      QAM-32 modulation\n"
                L"  -- ATSC Specific\n"
                L"  VSB8       VSB-8 modulation\n"
                L"  VSB16      VSB-16 modulation"),
        DtOptItem(L"mB",  m_Bandwidth, L"DVB-H/DVB-T/DTMB bandwidth (default: 8MHz)\n" 
                L"5, 6, 7 or 8 (MHz)"),
        DtOptItem(L"mT",  m_OfdmTxMode, DTAPI_MOD_DVBT_8K, L"DVB-H/DVB-T transmission mode (default: 8k)", OfdmTxMode),
        DtOptItem(L"mG",  m_OfdmGuardItv, DTAPI_MOD_DVBT_G_1_4, L"DVB-H/DVB-T guard interval (default: 1/4)", GuardInterval),
        DtOptItem(L"mH",  m_DtmbFrameHdrMode, DTAPI_MOD_DTMB_PN420, L"DTMB frame-header-mode (default: PN420)", DtmbFrameHeaderMode),
        DtOptItem(L"mP",  m_DvbS2Pilots, DTAPI_MOD_S2_NOPILOTS, L"Enable Pilots in DVB-S2 (default: OFF)", DvbS2Pilots),
        DtOptItem(L"mF",  m_DvbS2FecFrameLength, DTAPI_MOD_S2_LONGFRM, L"Long/short FEC frame in DVB-S2 (default: LONG)", DvbS2FecFrames),
        DtOptItem(L"mI",  m_DvbS2GoldSeqInit, 0, L"Gold sequence initialisation value (default: 0)\n"
                L"  0 ... 262143", 0, 262143),
        DtOptItem(L"msi", m_SpecInvers, false, L"Invert spectrum"),
        DtOptItem(L"if",  m_IqInterpFilter, DTAPI_MOD_INTERPOL_OFDM, L"Interpolation filter used in IQ mode (default: OFDM)", IqInterpolFilter),
        DtOptItem(L"snr", m_Snr, -1.0, L"Enable noise generation and set SNR in dB (e.g. -snr 26.0)", 0.0, 36.0),
        DtOptItem(L"ipa", m_Ipa, L"IP address/port (e.g. 192.168.0.1[:5768], port is optional)"),
        DtOptItem(L"ipp", m_Ipp, DTAPI_PROTO_UDP, L"IP Protocol (default: UDP)", IpProtocol),
        DtOptItem(L"ipn", m_Ipn, 7, L"Number of TPs per IP packet (valid range: 1-7, default: 7)", 1, 7),
        DtOptItem(L"ipt", m_Ipt, 0, L"Time-To-Live value (for TX only)"),
        DtOptItem(L"mS",  m_Stuffing, DTAPI_TXSTUFF_MODE_OFF, L"Enable Stuffing (default: OFF)", StuffingMode),
        DtOptItem(L"dP",  m_DataPid, -1, L"PID of the data stream (T2MI/CCMB)", -1, 8191),
        DtOptItem(L"dP2", m_DataPid2, -1, L"Second PID of the data stream (T2MI)", -1, 8191),
        DtOptItem(L"s",   m_SilentMode, false, L"Silent mode. No messages printed"),
        DtOptItem(L"?",   m_ShowHelp, false, L"Display this help"),
    };
    g_CmdOptions = CmdOptions;
    g_NumCmdOptions = sizeof(CmdOptions) / sizeof(CmdOptions[0]);

    list<wstring>  FreeArgs;
    DtOptItem::ParseOpt(CmdOptions, argc, argv, FreeArgs);

    // Carrier defined?
    if (!m_CarrierFreq.IsSet())
    {        
        m_CarrierFreq = 635.0;      // UHF-band
    }


    if (m_Bandwidth.IsSet())
    {
        if (m_ModType == DTAPI_MOD_DVBT)
            m_Bandwidth.ParseEnum(BandwithDvbt, L"mB");
        else if (m_ModType == DTAPI_MOD_DMBTH)
            m_Bandwidth.ParseEnum(BandwithDmbth, L"mB");
    } else {
        if (m_ModType == DTAPI_MOD_DVBT)
            m_Bandwidth.MakeInt(DTAPI_MOD_DVBT_8MHZ);
        else if (m_ModType == DTAPI_MOD_DMBTH)
            m_Bandwidth.MakeInt(DTAPI_MOD_DTMB_8MHZ);
    }

    //// Check for required parameters
    //if (FreeArgs.size()==0 && !m_ShowHelp)
    //    throw Exc(c_CleNoPlayFile);

    if (FreeArgs.size() > 0)
        m_FileName = FreeArgs.front();
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::TxMode2Str -.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
const char* CommandLineParams::TxMode2Str() const
{
    if ( m_PlayDtSdiFile )
    {
        static char sz[256];
        strcpy(sz, "DTSDI (");

        // Full-frame or active video only?
        if ( ((m_TxMode & DTAPI_TXMODE_SDI_MASK) | DTAPI_TXMODE_SDI) == DTAPI_TXMODE_SDI_FULL )
            strcat(sz, "FULL");
        else
            strcat(sz, "ACTVID");
        
        switch (m_SdiSubValue)
        {
        case DTAPI_VIDSTD_1080P50:          strcat(sz, ", 1080p50"); break;
        case DTAPI_VIDSTD_1080P59_94:       strcat(sz, ", 1080p59.94"); break;
        case DTAPI_VIDSTD_1080P60:          strcat(sz, ", 1080p60"); break;
        case DTAPI_VIDSTD_1080I50:          strcat(sz, ", 1080i50"); break;
        case DTAPI_VIDSTD_1080I59_94:       strcat(sz, ", 1080i59.94"); break;
        case DTAPI_VIDSTD_1080I60:          strcat(sz, ", 1080i60"); break;
        case DTAPI_VIDSTD_1080P23_98:       strcat(sz, ", 1080p23.98"); break;
        case DTAPI_VIDSTD_1080P24:          strcat(sz, ", 1080p24"); break;
        case DTAPI_VIDSTD_1080P25:          strcat(sz, ", 1080p25"); break;
        case DTAPI_VIDSTD_1080P29_97:       strcat(sz, ", 1080p29.97"); break;
        case DTAPI_VIDSTD_1080P30:          strcat(sz, ", 1080p30"); break;
        case DTAPI_VIDSTD_720P23_98:        strcat(sz, ", 720p23.98"); break;
        case DTAPI_VIDSTD_720P24:           strcat(sz, ", 720p24"); break;
        case DTAPI_VIDSTD_720P25:           strcat(sz, ", 720p25"); break;
        case DTAPI_VIDSTD_720P29_97:        strcat(sz, ", 720p29.97"); break;
        case DTAPI_VIDSTD_720P30:           strcat(sz, ", 720p30"); break;
        case DTAPI_VIDSTD_720P50:           strcat(sz, ", 720p50"); break;
        case DTAPI_VIDSTD_720P59_94:        strcat(sz, ", 720p59.94"); break;
        case DTAPI_VIDSTD_720P60:           strcat(sz, ", 720p60"); break;
        case DTAPI_VIDSTD_525I59_94:        strcat(sz, ", 525i59.94"); break;
        case DTAPI_VIDSTD_625I50:           strcat(sz, ", 625i50"); break;
        }
          
        // 16bit, 10bit or 8bit?
        if ( 0!=(m_TxMode & DTAPI_TXMODE_SDI_16B) )
          strcat(sz, ", 16B");
        else if ( 0!=(m_TxMode & DTAPI_TXMODE_SDI_10B) )
          strcat(sz, ", 10B");
        else
          strcat(sz, ", 8B");

        // Huffman compressed?
        if ( 0!=(m_TxMode & DTAPI_TXMODE_SDI_HUFFMAN) )
            strcat(sz, ", HUFFMAN");

        if (m_Stuffing == DTAPI_TXSTUFF_MODE_ON)
            strcat(sz, ", STUFFING)");
        else
            strcat(sz, ")");

        return sz;
    }
    else
    {
        if ( m_TxMode & DTAPI_TXMODE_SDI )            
            if ( m_SdiSubValue == DTAPI_IOCONFIG_625I50)
               return "SDI8B_625";
            else
                return "SDI8B_525";     
        else
        {
            switch( m_TxMode & DTAPI_TXMODE_TS_MASK )
            {
            case DTAPI_TXMODE_188:      return "188";
            case DTAPI_TXMODE_ADD16:    return "ADD16";
            case DTAPI_TXMODE_192:      return "192";
            case DTAPI_TXMODE_204:      return "204";
            case DTAPI_TXMODE_RAW:      return "RAW";
            case DTAPI_TXMODE_MIN16:    return "MIN16";
            default:                    return "?";
            }
        }
    }
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::ModType2Str -.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
const char* CommandLineParams::ModType2Str() const
{
    switch( m_ModType )
    {
    case DTAPI_MOD_ATSC:            return "ATSC";
    case DTAPI_MOD_CMMB:            return "CMMB";
    case DTAPI_MOD_DAB:             return "DAB";
    case DTAPI_MOD_DVBS_BPSK:       return "BPSK";
    case DTAPI_MOD_DVBS_QPSK:       return "QPSK";
    case DTAPI_MOD_DMBTH:           return "DTMB";
    case DTAPI_MOD_DVBS2_QPSK:      return "QPSK (DVBS2)";
    case DTAPI_MOD_DVBS2_8PSK:      return "8PSK (DVBS2)";
    case DTAPI_MOD_DVBS2_16APSK:    return "16APSK (DVBS2)";
    case DTAPI_MOD_DVBS2_32APSK:    return "32APSK (DVBS2)";
    case DTAPI_MOD_DVBT:            return "DVB-T";
    case DTAPI_MOD_ISDBS:           return "ISDB-S";
    case DTAPI_MOD_ISDBS3:          return "ISDB-S3";
    case DTAPI_MOD_ISDBT:           return "ISDB-T";
    case DTAPI_MOD_QAM4:            return "QAM4";
    case DTAPI_MOD_QAM16:           return "QAM16";
    case DTAPI_MOD_QAM32:           return "QAM32";
    case DTAPI_MOD_QAM64:           return "QAM64";
    case DTAPI_MOD_QAM128:          return "QAM128";
    case DTAPI_MOD_QAM256:          return "QAM256";
    case DTAPI_MOD_T2MI:            return "T2MI";
    case DTAPI_MOD_IQDIRECT:        return "IQ";
    case DTAPI_MOD_ATSC3:            return "ATSC3";

    default:                        return "?";
    }
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::CodeRate2Str -.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
const char* CommandLineParams::CodeRate2Str() const
{
    if ( m_ModType == DTAPI_MOD_DMBTH )
    {
        switch( m_CodeRate )
        {
        case DTAPI_MOD_DTMB_0_4:    return "0.4";
        case DTAPI_MOD_DTMB_0_6:    return "0.6";
        case DTAPI_MOD_DTMB_0_8:    return "0.8";
        default:                    return "?";
        }
    }
    else
    {
        switch( m_CodeRate )
        {
        case DTAPI_MOD_1_2:     return "1/2";
        case DTAPI_MOD_2_3:     return "2/3";
        case DTAPI_MOD_3_4:     return "3/4";
        case DTAPI_MOD_4_5:     return "4/5";
        case DTAPI_MOD_5_6:     return "5/6";
        case DTAPI_MOD_6_7:     return "6/7";
        case DTAPI_MOD_7_8:     return "7/8";
        case DTAPI_MOD_1_3:     return "1/3";
        case DTAPI_MOD_2_5:     return "2/5";
        case DTAPI_MOD_3_5:     return "3/5";
        case DTAPI_MOD_8_9:     return "8/9";
        case DTAPI_MOD_9_10:    return "9/10";
        default:                return "?";
        }
    }
}

//.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::QamJ83Annex2Str -.-.-.-.-.-.-.-.-.-.-.-.-.
//
const char* CommandLineParams::QamJ83Annex2Str() const
{
    switch( m_QamJ83Annex )
    {
    case DTAPI_MOD_J83_A:       return "J.83 Annex A";
    case DTAPI_MOD_J83_B:       return "J.83 Annex B";
    case DTAPI_MOD_J83_C:       return "J.83 Annex C";
    default:                    return "?";
    }
}

//-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::OfdmBandwidth2Str -.-.-.-.-.-.-.-.-.-.-.-.-
//
const char* CommandLineParams::OfdmBandwidth2Str() const
{

    if ( m_ModType == DTAPI_MOD_DMBTH )
    {
        switch( m_Bandwidth )
        {
        case DTAPI_MOD_DTMB_5MHZ:   return "5MHz";
        case DTAPI_MOD_DTMB_6MHZ:   return "6MHz";
        case DTAPI_MOD_DTMB_7MHZ:   return "7MHz";
        case DTAPI_MOD_DTMB_8MHZ:   return "8MHz";
        default:                    return "?";
        }

    } 
    else
    {
        switch( m_Bandwidth )
        {
        case DTAPI_MOD_DVBT_5MHZ:   return "5MHz";
        case DTAPI_MOD_DVBT_6MHZ:   return "6MHz";
        case DTAPI_MOD_DVBT_7MHZ:   return "7MHz";
        case DTAPI_MOD_DVBT_8MHZ:   return "8MHz";
        default:                    return "?";
        }
    }
}

//-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::Constellation2Str -.-.-.-.-.-.-.-.-.-.-.-.-
//
const char* CommandLineParams::Constellation2Str() const
{
    if ( m_ModType == DTAPI_MOD_ATSC )
    {
        switch( m_Constellation )
        {
        case DTAPI_MOD_ATSC_VSB8:   return "VSB8";
        case DTAPI_MOD_ATSC_VSB16:  return "VSB16";
        default:                    return "?";
        }

    }
    else if ( m_ModType == DTAPI_MOD_DMBTH )
    {
        switch( m_Constellation )
        {
        case DTAPI_MOD_DTMB_QAM4NR: return "QAM4NR";
        case DTAPI_MOD_DTMB_QAM4:   return "QAM4";
        case DTAPI_MOD_DTMB_QAM16:  return "QAM16";
        case DTAPI_MOD_DTMB_QAM32:  return "QAM32";
        case DTAPI_MOD_DTMB_QAM64:  return "QAM64";
        default:                    return "?";
        }
    }
    else
    {
        switch( m_Constellation )
        {
        case DTAPI_MOD_DVBT_QPSK:   return "QPSK";
        case DTAPI_MOD_DVBT_QAM16:  return "QAM16";
        case DTAPI_MOD_DVBT_QAM64:  return "QAM64";
        default:                    return "?";
        }
    }
}

//-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::OfdmGaurdItv2Str -.-.-.-.-.-.-.-.-.-.-.-.-.
//
const char* CommandLineParams::OfdmGaurdItv2Str() const
{
    switch( m_OfdmGuardItv )
    {
    case DTAPI_MOD_DVBT_G_1_4:  return "1/4";
    case DTAPI_MOD_DVBT_G_1_8:  return "1/8";
    case DTAPI_MOD_DVBT_G_1_16: return "1/16";
    case DTAPI_MOD_DVBT_G_1_32: return "1/32";
    default:                    return "?";
    }
}

//.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::OfdmTxMode2Str -.-.-.-.-.-.-.-.-.-.-.-.-.-
//
const char* CommandLineParams::OfdmTxMode2Str() const
{
    switch( m_OfdmTxMode )
    {
    case DTAPI_MOD_DVBT_2K:     return "2k";
    case DTAPI_MOD_DVBT_4K:     return "4k";
    case DTAPI_MOD_DVBT_8K:     return "8k";
    default:                    return "?";
    }
}


//.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::DvbS2Pilots2Str -.-.-.-.-.-.-.-.-.-.-.-.-.
//
const char* CommandLineParams::DvbS2Pilots2Str() const
{
    switch ( m_DvbS2Pilots )
    {
    case DTAPI_MOD_S2_NOPILOTS: return "OFF";
    case DTAPI_MOD_S2_PILOTS:   return "ON";
    default:                    return "?";
    }
}

//.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::DvbS2FecFrameLength2Str -.-.-.-.-.-.-.-.-.-.-.
//
const char* CommandLineParams::DvbS2FecFrameLength2Str() const
{
    switch ( m_DvbS2FecFrameLength )
    {
    case DTAPI_MOD_S2_SHORTFRM: return "SHORT";
    case DTAPI_MOD_S2_LONGFRM:  return "LONG";
    default:                    return "?";
    }
}

//-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::DtmbFrameHdrMode2Str -.-.-.-.-.-.-.-.-.-.-.-.
//
const char* CommandLineParams::DtmbFrameHdrMode2Str() const
{
    switch( m_DtmbFrameHdrMode )
    {
    case DTAPI_MOD_DTMB_PN420:  return "PN420";
    case DTAPI_MOD_DTMB_PN595:  return "PN595";
    case DTAPI_MOD_DTMB_PN945:  return "PN945";
    default:                    return "?";
    }
}

//.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::IqInterpFilter2Str -.-.-.-.-.-.-.-.-.-.-.-.-
//
const char* CommandLineParams::IqInterpFilter2Str() const
{
    switch ( m_IqInterpFilter )
    {
    case DTAPI_MOD_INTERPOL_OFDM: return "OFDM";
    case DTAPI_MOD_INTERPOL_QAM:  return "QAM";
    default:                      return "?";
    }
}

//.-.-.-.-.-.-.-.-.-.-.-.-.- CommandLineParams::IpProtocol2Str -.-.-.-.-.-.-.-.-.-.-.-.-.-
//
const char* CommandLineParams::IpProtocol2Str() const
{
    switch ( m_IpPars.m_Protocol )
    {
    case DTAPI_PROTO_UDP:       return "UDP";
    case DTAPI_PROTO_RTP:       return "RTP";
    default:                    return "?";
    }
}

//=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+ Player implementation +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::Player -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
Player::Player() : m_pFile(NULL), m_pBuf(NULL)
{
#ifndef WINBUILD
    ChangeTerminalMode(1);
#endif
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::~Player -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
Player::~Player()
{
    // Free our buffer
    if ( m_pBuf != NULL )
        delete [] m_pBuf;

    // donot forget to close our file
    if ( m_pFile != NULL )
        ::fclose(m_pFile);

#ifndef WINBUILD
    ChangeTerminalMode(0);
#endif
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::HasOutputPort -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
bool  Player::HasOutputPort()
{
    DtHwFuncDesc HwFuncs[16];
    int  NumHwFuncs;
    DTAPI_RESULT  dr = m_DtDvc.HwFuncScan(sizeof(HwFuncs)/sizeof(HwFuncs[0]),
                                                                     NumHwFuncs, HwFuncs);
    if (dr != DTAPI_OK)
        return false;
    for (int i=0; i<NumHwFuncs; i++)
    {
        if ((HwFuncs[i].m_Flags&DTAPI_CAP_OUTPUT)!=0 
                                                  || (HwFuncs[i].m_Flags&DTAPI_CAP_IP)!=0)
            return true;
    }
    return false;
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Type2Name -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
static const char* Type2Name(int TypeNum)
{
    static char buf[32];
    if (TypeNum<200 || (TypeNum>=2000&&TypeNum<3000))
        sprintf(buf, "DTA-%d", TypeNum);
    else if (TypeNum>=200 && TypeNum <400)
        sprintf(buf, "DTU-%d", TypeNum);
    else if (TypeNum>3000)
        sprintf(buf, "DTE-%d", TypeNum);
    else
        sprintf(buf, "%d", TypeNum);
    return buf;
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::IsMatchingPortType -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
bool  Player::IsMatchingPortType(PortType  WantedPortType, DtHwFuncDesc*  pHwFunc)
{
    switch (WantedPortType)
    {
    case PT_UNKNOWN:
        return true;
    case PT_ASI:
        return (pHwFunc->m_Flags&DTAPI_CAP_ASI) != 0;
    case PT_IP:
        return (pHwFunc->m_Flags&DTAPI_CAP_IP) != 0;
    case PT_MOD:
        return (pHwFunc->m_Flags&DTAPI_CAP_MOD) != 0;
    case PT_SDI:
        return (pHwFunc->m_Flags&DTAPI_CAP_SDI) != 0;
    }
    return false;
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::AttachToOutput -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
void Player::AttachToOutput()
{
    DTAPI_RESULT dr;
    
    if (m_CmdLineParams.m_DvcType.IsSet())
    {
        dr = m_DtDvc.AttachToType(m_CmdLineParams.m_DvcType, m_CmdLineParams.m_DvcNum-1);
        if (dr != DTAPI_OK)
            throw Exc(c_ErrFailToAttachToType, Type2Name(m_CmdLineParams.m_DvcType),
                                                                     DtapiResult2Str(dr));
    } else {
        if (m_CmdLineParams.m_DvcNum.IsSet())
            throw Exc(c_ErrDvcNumWithoutType);
        DtDeviceDesc  DvcDescs[10];
        int  NumDvcs;
        dr = DtapiDeviceScan(sizeof(DvcDescs)/sizeof(DvcDescs[0]), NumDvcs, DvcDescs);
        if (dr != DTAPI_OK)
            throw Exc(c_ErrDtapiDvcScanFailed, DtapiResult2Str(dr));
        bool  FoundDvc = false;
        for (int i=0; i<NumDvcs; i++)
        {
            dr = m_DtDvc.AttachToSerial(DvcDescs[i].m_Serial);
            if (dr != DTAPI_OK)
                throw Exc(c_ErrFailToAttachToType, Type2Name(DvcDescs[i].m_TypeNumber),
                                                                     DtapiResult2Str(dr));
            if (!HasOutputPort())
            {
                m_DtDvc.Detach();
                continue;
            }
            FoundDvc = true;
            break;
        }
        if (!FoundDvc)
            throw Exc(c_ErrNoOutputFound);
    }
    // We've found a device with output-ports and are attached to it. Now check if we can
    // find the proper port.
    int  Port = -1;
    
    DtHwFuncDesc HwFuncs[16];
    int  NumHwFuncs;
    dr = m_DtDvc.HwFuncScan(sizeof(HwFuncs)/sizeof(HwFuncs[0]), NumHwFuncs, HwFuncs);
    if (dr != DTAPI_OK)
        throw Exc(c_ErrHwScanFailed, DtapiResult2Str(dr));
    if (m_CmdLineParams.m_Port.IsSet())
    {
        if (m_CmdLineParams.m_Port<1 || m_CmdLineParams.m_Port>NumHwFuncs)
        {
            throw Exc(c_ErrNoSuchPort, Type2Name(m_DtDvc.m_DvcDesc.m_TypeNumber),
                                                          m_CmdLineParams.m_Port.ToInt());
        }
        Port = m_CmdLineParams.m_Port;
        if ((HwFuncs[m_CmdLineParams.m_Port-1].m_Flags&DTAPI_CAP_OUTPUT) != 0)
        {
            // Uses has explicitly selected a port, make sure it's configured as output.
            dr = m_DtDvc.SetIoConfig(Port, DTAPI_IOCONFIG_IODIR, DTAPI_IOCONFIG_OUTPUT,
                                                                   DTAPI_IOCONFIG_OUTPUT);
            if (dr != DTAPI_OK)
                throw Exc(c_ErrFailToSetIoConfig, DtapiResult2Str(dr));
        } else if ((HwFuncs[m_CmdLineParams.m_Port-1].m_Flags&DTAPI_CAP_IP) == 0)
            throw Exc(c_ErrNoOutputPort, Type2Name(m_DtDvc.m_DvcDesc.m_TypeNumber),
                                                          m_CmdLineParams.m_Port.ToInt());
    } else {
        PortType  WantedPortType = GuessPortTypeFromPars();
        for (int i=0; i<NumHwFuncs; i++)
        {
            if ((HwFuncs[i].m_Flags&DTAPI_CAP_OUTPUT) != 0) 
            {
                int  Value=-1;
                dr = m_DtDvc.GetIoConfig(i+1, DTAPI_IOCONFIG_IODIR, Value);
                if (dr != DTAPI_OK)
                    throw Exc(c_ErrFailToGetIoConfig, DtapiResult2Str(dr));
                if (Value != DTAPI_IOCONFIG_OUTPUT)
                    continue;
            } else if  ((HwFuncs[i].m_Flags&DTAPI_CAP_IP) == 0)
                continue;
            // Port is an IP or output-port, check port type
            if (IsMatchingPortType(WantedPortType, &HwFuncs[i]))
            {
                Port = i+1;
                break;
            }
        }
        if (Port == -1)
            throw Exc(c_ErrNoSuitablePort, Type2Name(m_DtDvc.m_DvcDesc.m_TypeNumber));
    }
    // At this point we're sure the chosen port is valid and configured as output.

    if ((HwFuncs[Port-1].m_Flags & DTAPI_CAP_FRACMODE)!=0 &&
                                                          m_CmdLineParams.m_PlayDtSdiFile)
    {
        DtVidStdInfo  Info;
        dr = DtapiGetVidStdInfo(m_CmdLineParams.m_SdiSubValue, Info);
        if (dr != DTAPI_OK)
            throw Exc(c_ErrFailToGetVidStdInfo, ::DtapiResult2Str(dr));
        int  Value = Info.m_IsFractional ? DTAPI_IOCONFIG_TRUE : DTAPI_IOCONFIG_FALSE;
        int  NumPorts = m_DtDvc.m_DvcDesc.m_NumPorts;
        DtIoConfig*  pIoConfigs = new DtIoConfig[NumPorts*2];
        int  NumIoConfig = 0;
        for (int i=0; i<NumPorts; i++)
        {
            if ((m_DtDvc.m_pHwf[i].m_Flags & DTAPI_CAP_FRACMODE) != 0)
            {
                pIoConfigs[NumIoConfig].m_Port = i + 1;
                pIoConfigs[NumIoConfig].m_Group = DTAPI_IOCONFIG_FRACMODE;
                pIoConfigs[NumIoConfig].m_Value = Value;
                pIoConfigs[NumIoConfig].m_SubValue = -1;
                pIoConfigs[NumIoConfig].m_ParXtra[0] = -1;
                pIoConfigs[NumIoConfig].m_ParXtra[1] = -1;
                NumIoConfig++;
                pIoConfigs[NumIoConfig].m_Port = i + 1;
                pIoConfigs[NumIoConfig].m_Group = DTAPI_IOCONFIG_IOSTD;
                pIoConfigs[NumIoConfig].m_Value = DTAPI_IOCONFIG_SDI;
                if (Value == DTAPI_IOCONFIG_TRUE)
                    pIoConfigs[NumIoConfig].m_SubValue = DTAPI_IOCONFIG_525I59_94;
                else
                    pIoConfigs[NumIoConfig].m_SubValue = DTAPI_IOCONFIG_625I50;
                pIoConfigs[NumIoConfig].m_ParXtra[0] = -1;
                pIoConfigs[NumIoConfig].m_ParXtra[1] = -1;
                NumIoConfig++;
            }
        }
        dr = m_DtDvc.SetIoConfig(pIoConfigs, NumIoConfig);
        delete [] pIoConfigs;
        if (dr != DTAPI_OK)
            throw Exc(c_ErrFailToSetIoConfig, ::DtapiResult2Str(dr));
    }
    
    if (m_CmdLineParams.m_DblBuff.IsSet())
    {
        if (m_CmdLineParams.m_DblBuff == Port)
            throw Exc(c_ErrDblbufSelf);

        dr = m_DtDvc.SetIoConfig(m_CmdLineParams.m_DblBuff, DTAPI_IOCONFIG_IODIR,
                                      DTAPI_IOCONFIG_OUTPUT, DTAPI_IOCONFIG_DBLBUF, Port);
        if (dr != DTAPI_OK)
            LogF("Failed to set port %d in double-buffered mode (ERROR: %s)",
                                  m_CmdLineParams.m_DblBuff.ToInt(), DtapiResult2Str(dr));
    }

    // Attach to the output channel
    dr = m_DtOutp.AttachToPort(&m_DtDvc, Port);
    if (dr == DTAPI_OK_FAILSAFE)
        throw Exc(c_ErrFailsafeEnabled);
    if (dr != DTAPI_OK)
        throw Exc(c_ErrFailToAttachToChan, ::DtapiResult2Str(dr));

    // Check for modulator card (assume no modulator)
    m_Modulator = (m_DtOutp.m_HwFuncDesc.m_Flags&DTAPI_CAP_MOD) !=0;
    // Check for IP output
    m_Ip = (m_DtOutp.m_HwFuncDesc.m_Flags&DTAPI_CAP_IP) !=0;
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::AutoDetectSdiFormat -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
void Player::AutoDetectSdiFormat()
{
    m_SizeOfDtSdiHdr = 0;

    // Load file header from file
    DtSdiFileHdrV2 FileHdr;
    int NumBytesRead = (int)fread(&FileHdr, 1, sizeof(FileHdr), m_pFile);
    // Sanity check
    if ( NumBytesRead != sizeof(FileHdr) )
        throw Exc(c_ErrInvalidFileSize);

    // Check if we have a valid header
    if (   FileHdr.m_BaseHdr.m_MagicCode1!=DTSDI_MAGIC_CODE1 
        || FileHdr.m_BaseHdr.m_MagicCode2!=DTSDI_MAGIC_CODE2
        || FileHdr.m_BaseHdr.m_MagicCode3!=DTSDI_MAGIC_CODE3 )
        throw Exc(c_ErrInvalidDtSdiFileHdr);
    // Version 1 or version 2
    if (FileHdr.m_BaseHdr.m_FmtVersion == DTSDI_FMT_VERSION)
    {
        m_SizeOfDtSdiHdr = sizeof(DtSdiFileHdr);
        fseek(m_pFile, sizeof(DtSdiFileHdr), SEEK_SET);
    }
    else if (FileHdr.m_BaseHdr.m_FmtVersion == DTSDI_FMT_VERSION2)
    {
        m_SizeOfDtSdiHdr = sizeof(DtSdiFileHdrV2);
    }
    else
        throw Exc(c_ErrInvalidDtSdiFileHdr);
    
    // Init tx-mode from settings in header
    m_CmdLineParams.m_TxMode = 0;
    if ( (FileHdr.m_BaseHdr.m_Flags & 0x3) == DTSDI_SDI_FULL )
        m_CmdLineParams.m_TxMode = DTAPI_TXMODE_SDI_FULL;
    else
        m_CmdLineParams.m_TxMode = DTAPI_TXMODE_SDI_ACTVID;

    if ( 0!=(FileHdr.m_BaseHdr.m_Flags & DTSDI_SDI_16B) )
        m_CmdLineParams.m_TxMode = m_CmdLineParams.m_TxMode | DTAPI_TXMODE_SDI_16B;
    if ( 0!=(FileHdr.m_BaseHdr.m_Flags & DTSDI_SDI_10B) )
        m_CmdLineParams.m_TxMode = m_CmdLineParams.m_TxMode | DTAPI_TXMODE_SDI_10B;

    if ( 0!=(FileHdr.m_BaseHdr.m_Flags & DTSDI_SDI_HUFFMAN) )
        m_CmdLineParams.m_TxMode = m_CmdLineParams.m_TxMode | DTAPI_TXMODE_SDI_HUFFMAN;
    
    m_CmdLineParams.m_SdiSubValue
        = DtSdiDataType2VidStd(FileHdr.m_BaseHdr.m_DataType);
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::DisplayPlayInfo -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
void Player::DisplayPlayInfo()
{
    char sz[256];

    LogF("");
    LogF("Start playing");
    LogF(L"- Play file name        : %ls", m_CmdLineParams.m_FileName.c_str());
    LogF("- Loop file             : %dx", m_CmdLineParams.m_LoopCnt.ToInt());

    // Skip bit-rate for DVB-H/DVB-T/ISDBT/IQ/CMMB/DTMB modulator and in case of SDI
    if ( !(   ( m_Modulator
                && (   m_CmdLineParams.m_ModType==DTAPI_MOD_DVBT
                    || m_CmdLineParams.m_ModType==DTAPI_MOD_CMMB
                    || m_CmdLineParams.m_ModType==DTAPI_MOD_DAB
                    || m_CmdLineParams.m_ModType==DTAPI_MOD_ISDBS
                    || m_CmdLineParams.m_ModType==DTAPI_MOD_ISDBS3
                    || m_CmdLineParams.m_ModType==DTAPI_MOD_ISDBT
                    || m_CmdLineParams.m_ModType==DTAPI_MOD_IQDIRECT
                    || m_CmdLineParams.m_ModType==DTAPI_MOD_DMBTH) )
           || ( !m_Modulator && 0!=(m_CmdLineParams.m_TxMode&DTAPI_TXMODE_SDI)) ) )
    {
        if ( m_CmdLineParams.m_TxRate==0 )
            LogF("- Transport-Stream rate : TX-on-time");
        else
            LogF("- Transport-Stream rate : %d bps", m_CmdLineParams.m_TxRate.ToInt());
    }
    else if ( m_Modulator && m_CmdLineParams.m_ModType==DTAPI_MOD_IQDIRECT )
    {
        LogF("- Sample rate           : %d Hz", m_CmdLineParams.m_TxRate.ToInt());
    }
    //// Don't log Transmit Mode for ISDB-S3
    //if (m_CmdLineParams.m_ModType!=DTAPI_MOD_ISDBS3)
    //LogF("- Transmit Mode         : %s", m_CmdLineParams.TxMode2Str() );
    //
    if ( m_DtDvc.Category() == DTAPI_CAT_USB )
        sprintf(sz, "DTU-%d", m_DtDvc.TypeNumber() );
    else /*if ( m_DtDvc.Category() == DTAPI_CAT_PCI )*/
        sprintf(sz, "DTA-%d", m_DtDvc.TypeNumber() );
    
    if ( m_CmdLineParams.m_Port != -1 )
    {
        LogF("- Output device         : %s port %d (#%d)", sz, m_CmdLineParams.m_Port.ToInt(),
             m_CmdLineParams.m_DvcNum.ToInt() );
    }
    else
        LogF("- Output device         : %s (#%d)", sz, m_CmdLineParams.m_DvcNum.ToInt() );

    // TODO
    //if (m_IoConfig == DTAPI_IOCONFIG_GENLOCKED)
    //  LogF("- Genlock reference     : %s port %d (#%d)", sz, m_ParXtra == -1 ? 1 : m_ParXtra,
    //       m_CmdLineParams.m_DvcNum );
    //  

    // Do we have a modulator
    if ( m_Modulator )
    {
        LogF("- Modulation Type       : %s", m_CmdLineParams.ModType2Str() );
        LogF("- Carrier Frequency     : %.2f MHz", m_CmdLineParams.m_CarrierFreq.ToDouble() );
        // Show output level (if supported)
        if ( (m_DtOutp.m_HwFuncDesc.m_Flags & DTAPI_CAP_ADJLVL)!=0 )
            LogF("- Output Level          : %.1f dBm", m_CmdLineParams.m_OutpLevel.ToDouble() );

        // Show SNR (if supported)
        if (   (m_DtOutp.m_HwFuncDesc.m_Flags & DTAPI_CAP_SNR)!=0
            || (m_DtOutp.m_HwFuncDesc.m_Flags & DTAPI_CAP_CM)!=0 )
        {
            if ( m_CmdLineParams.m_Snr.ToDouble() >= 0.0 )
                LogF("- SNR                   : %.1f dB", m_CmdLineParams.m_Snr.ToDouble() );
            else
                LogF("- SNR                   : no noise generation" );
        }
        if (m_CmdLineParams.m_SpecInvers.ToBool())
                LogF("- RF-Mode               : inverted spectrum" );
        else
                LogF("- RF-Mode               : normal spectrum" );
        // Code rate: only for DVB-H/DVB-T, DTMB, QPSK and BPSK
        if (   m_CmdLineParams.m_ModType==DTAPI_MOD_DVBT
            || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS_QPSK
            || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS_BPSK
            || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_QPSK
            || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_8PSK
            || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_16APSK
            || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_32APSK
            || m_CmdLineParams.m_ModType==DTAPI_MOD_DMBTH
            )
            LogF("- Convolutional Rate    : %s", m_CmdLineParams.CodeRate2Str() );

        // ATSC modulation parameters
        if ( m_CmdLineParams.m_ModType==DTAPI_MOD_ATSC )
        {
            LogF("- Constellation         : %s", m_CmdLineParams.Constellation2Str() );
        }
        // DVB-H/DVB-T modulation parameters
        else if ( m_CmdLineParams.m_ModType==DTAPI_MOD_DVBT )
        {
            LogF("- Bandwitdh             : %s", m_CmdLineParams.OfdmBandwidth2Str() );
            LogF("- Transmission-mode     : %s", m_CmdLineParams.OfdmTxMode2Str() );
            LogF("- Gaurd Interval        : %s", m_CmdLineParams.OfdmGaurdItv2Str() );
            LogF("- Constellation         : %s", m_CmdLineParams.Constellation2Str() );
        }
        // DVB-S2 modulation parameters
        else if (    m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_QPSK
                  || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_8PSK
                  || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_16APSK
                  || m_CmdLineParams.m_ModType==DTAPI_MOD_DVBS2_32APSK )
        {
            LogF("- Pilots                : %s", m_CmdLineParams.DvbS2Pilots2Str() );
            LogF("- FEC Frame Length      : %s", m_CmdLineParams.DvbS2FecFrameLength2Str() );
            LogF("- Gold Seq. Init Value  : %d", m_CmdLineParams.m_DvbS2GoldSeqInit.ToInt() );
        }
        // DTMB modulation parameters
        else if ( m_CmdLineParams.m_ModType==DTAPI_MOD_DMBTH )
        {
            LogF("- Bandwitdh             : %s", m_CmdLineParams.OfdmBandwidth2Str() );
            LogF("- Frame-Header-Mode     : %s", m_CmdLineParams.DtmbFrameHdrMode2Str() );
            LogF("- Constellation         : %s", m_CmdLineParams.Constellation2Str() );
        }
        // IQ modulation parameters
        else if ( m_CmdLineParams.m_ModType==DTAPI_MOD_IQDIRECT )
        {
            LogF("- Interpolation Filter  : %s", m_CmdLineParams.IqInterpFilter2Str() );
        }
        // QAM modulation parameters
        else if (    m_CmdLineParams.m_ModType!=DTAPI_MOD_DVBT
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_ISDBS
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_ISDBS3
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_ISDBT
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_IQDIRECT
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_CMMB
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_DAB
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_T2MI
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_DVBS_QPSK
                  && m_CmdLineParams.m_ModType!=DTAPI_MOD_DVBS_BPSK )
        {
            LogF("- J.83                  : %s", m_CmdLineParams.QamJ83Annex2Str() );
        }
    }

    // Do we have a IP output
    if ( m_Ip )
    {
        // Log IP parameters
        DtapiIpAddr2Str( sz, sizeof(sz), m_CmdLineParams.m_IpPars.m_Ip );
        LogF("- IP Address            : %s:%d", sz, m_CmdLineParams.m_IpPars.m_Port );
        LogF("- Protocol              : %s", m_CmdLineParams.IpProtocol2Str() );
        LogF("- Num Tp per IP         : %d", m_CmdLineParams.m_IpPars.m_NumTpPerIp );
    }

    Log("");
    Log("Press any key to stop playing");
    Log("");
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::InitIsdbtPars -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
void Player::InitIsdbtPars(DtIsdbtPars& IsdbtPars)
{
    DTAPI_RESULT dr;
    const int c_PckSize = 204;
    const int c_TempBufSize = 1*1024*1024;
    char* pTempBuf = new char [c_TempBufSize];

    // Read in a portion of the file
    int NumBytes = (int)::fread(pTempBuf, 1, c_TempBufSize, m_pFile);
    // Reset file pointer
    ::fseek(m_pFile, 0, SEEK_SET);

    // Look for synchronisation
    for ( int i=0; i<NumBytes-(c_PckSize*4); i++ )
    {
        // If we find four syncs in a row => we are in sync
        if (   pTempBuf[i]==0x47
            && pTempBuf[i+(c_PckSize*1)]==0x47
            && pTempBuf[i+(c_PckSize*2)]==0x47
            && pTempBuf[i+(c_PckSize*3)]==0x47 )
        {
            // Set sync position
            pTempBuf += i; NumBytes -= i;
            break;
        }
    }
    
    // No muxing wanted, we have a 204 byte stream (with TMCC data) 
    IsdbtPars.m_DoMux = false;
    // Init structure by retrieveing paramters from TMCC information in packets
    dr = IsdbtPars.RetrieveParsFromTs(pTempBuf, NumBytes);
    // Free temp buffer
    delete [] pTempBuf;
    if ( dr != DTAPI_OK )
        throw Exc(c_FailedToInitIdsbtPars, ::DtapiResult2Str(dr));
}

//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::InitOutput -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
void Player::InitOutput()
{
    DTAPI_RESULT dr;

    // Init channel to initial 'safe' state
    dr = m_DtOutp.SetTxControl(DTAPI_TXCTRL_IDLE);  // Start in IDLE mode
    if ( dr != DTAPI_OK )
        throw Exc( c_ErrFailSetTxControl, ::DtapiResult2Str(dr) );


    // Apply modulation settings (if we have a modulator)
    if ( m_Modulator )
    {
        __int64 Freq = (__int64)(m_CmdLineParams.m_CarrierFreq.ToDouble()*1E6);
        dr = m_DtOutp.SetRfControl( Freq );
        if ( dr != DTAPI_OK )
            throw Exc( c_ErrFailSetRfControl, ::DtapiResult2Str(dr) );

        //jjustman-2020-01-17 - adding in test atsc3 mod support

            /* int XtraPar0 = m_CmdLineParams.m_Constellation
                | m_CmdLineParams.m_Bandwidth
                | m_CmdLineParams.m_DtmbFrameHdrMode
                | m_CmdLineParams.m_CodeRate
                | DTAPI_MOD_DTMB_IL_2
                | DTAPI_MOD_DTMB_USE_FRM_NO;*/

        //jjustman-2020-09-02 - TODO - wire this up to our preamble packet
        m_Atsc3Pars.m_Bandwidth = DTAPI_ATSC3_6MHZ;
        m_Atsc3Pars.m_MinorVersion = 0;
        m_Atsc3Pars.m_EasWakeup = 0;

        /**
        DTAPI_ATSC3_FFT_8K 8K FFT DTAPI_ATSC3_FFT_16K 16K FFT DTAPI_ATSC3_FFT_32K 32K FFT */
        m_Atsc3Pars.m_PreambleFftSize = DTAPI_ATSC3_FFT_8K;


        m_Atsc3Pars.m_PreambleGuardInterval = DTAPI_ATSC3_GI_6_1536;
        m_Atsc3Pars.m_PreamblePilotDx = DTAPI_ATSC3_PP_DX_4;
        m_Atsc3Pars.m_PreambleReducedCarriers = 0;
        m_Atsc3Pars.m_L1BasicFecMode = 1;
        m_Atsc3Pars.m_L1DetailFecMode = 1;
        m_Atsc3Pars.m_L1DetailAddParity = 0;
        /*
        DTAPI_ATSC3_TIME_NONE No time information is generated DTAPI_ATSC3_TIME_MS Time information in millisecond precision is generated DTAPI_ATSC3_TIME_US Time information in microsecond precision is generated DTAPI_ATSC3_TIME_NS Time information in nanosecond precision is generated
        */
        m_Atsc3Pars.m_TimeInfoFlag = DTAPI_ATSC3_TIME_NONE;
        m_Atsc3Pars.m_LlsFlag = true;
        /*
        DTAPI_ATSC3_PAPR_NONE None DTAPI_ATSC3_PAPR_ACE ACE - Active Constellation Extension DTAPI_ATSC3_PAPR_TR TR - Power reduction with reserved carriers DTAPI_ATSC3_PAPR_ACE_TR ACE and TR
*/
        m_Atsc3Pars.m_Papr = DTAPI_ATSC3_PAPR_NONE;
        m_Atsc3Pars.m_FrameLengthMode = DTAPI_ATSC3_ALIGN_SYMBOL; // DTAPI_ATSC3_ALIGN_TIME 
        m_Atsc3Pars.m_FrameLength = 0;

        

        //DtAtsc3PlpPars 
        //jjustman - mocked for now
        int num_plps = 2;
        for (int i = 0; i < num_plps; i++) {

            //subframes config
            DtAtsc3SubframePars mySubframeParams;
            mySubframeParams.Init();

            mySubframeParams.m_Miso = DTAPI_ATSC3_MISO_NONE;
            mySubframeParams.m_MisoNumTx = 0;
            mySubframeParams.m_MisoTxIndex = 0;

            /*
            DTAPI_ATSC3_FFT_8K 8K FFT DTAPI_ATSC3_FFT_16K 16K FFT DTAPI_ATSC3_FFT_32K 32K FFT */
            mySubframeParams.m_FftSize = (i == 0) ? DTAPI_ATSC3_FFT_8K : DTAPI_ATSC3_FFT_16K;

            mySubframeParams.m_ReducedCarriers = 0;
            mySubframeParams.m_GuardInterval = (i == 0) ? DTAPI_ATSC3_GI_6_1536 : DTAPI_ATSC3_GI_6_1536;
            mySubframeParams.m_PilotPattern = (i == 0) ? DTAPI_ATSC3_PP_4_2 : DTAPI_ATSC3_PP_4_4;

            mySubframeParams.m_PilotBoost = 0;
            mySubframeParams.m_SbsFirst = true;
            mySubframeParams.m_SbsLast = true;

            mySubframeParams.m_NumOfdmSymbols = (i == 0) ? 43 : 71;
            mySubframeParams.m_FreqInterleaver = true;

            DtAtsc3PlpPars m_atsc3PlpPars;
            m_atsc3PlpPars.Init();
            m_atsc3PlpPars.m_Id = i; //plp_id
            m_atsc3PlpPars.m_LlsFlag = (i == 0) ? true : false;

            /*
            DTAPI_ATSC3_LAYER_CORE Core layer DTAPI_ATSC3_LAYER_ENHANCED Enhanced layer */
            m_atsc3PlpPars.m_Layer = DTAPI_ATSC3_LAYER_CORE;
            /*
            m_Modulation Modulation used by the PLP.  Value Meaning
                DTAPI_ATSC3_QPSK QPSK
                DTAPI_ATSC3_QAM16 16-QAM
                DTAPI_ATSC3_QAM64 64-QAM
                DTAPI_ATSC3_QAM256 256-QAM
                DTAPI_ATSC3_QAM1024 1024-QAM
                DTAPI_ATSC3_QAM4096 4096-QAM */
            m_atsc3PlpPars.m_Modulation = (i == 0) ? DTAPI_ATSC3_QAM16 : DTAPI_ATSC3_QAM256;
            m_atsc3PlpPars.m_CodeRate = (i == 0) ? DTAPI_ATSC3_COD_11_15 : DTAPI_ATSC3_COD_11_15; //DTAPI_ATSC3_COD_9_15

            /*
            DTAPI_ATSC3_LDPC_16K 16K LDPC
            DTAPI_ATSC3_LDPC_64K 64K LDPC
            */
            m_atsc3PlpPars.m_FecCodeLength = DTAPI_ATSC3_LDPC_64K; //DTAPI_ATSC3_LDPC_16K;
            /*
            DTAPI_ATSC3_OUTER_BCH BCH outer code
            DTAPI_ATSC3_OUTER_CRC CRC outer code
            DTAPI_ATSC3_OUTER_NONE No outer code
            */
            m_atsc3PlpPars.m_FecOuterCode = DTAPI_ATSC3_OUTER_BCH;

            m_atsc3PlpPars.m_LdmInjectLevel = 0;

            /*
            DTAPI_ATSC3_PLPTYPE_NONDISP Non-dispersed PLP-type
            DTAPI_ATSC3_PLPTYPE_DISP Dispersed PLP-type*/
            m_atsc3PlpPars.m_PlpType = DTAPI_ATSC3_PLPTYPE_NONDISP;

            /*
            ignoring w/ DTAPI_ATSC3_PLPTYPE_NONDISP
                int  m_NumSubslices;        // Number of subslices: 1...16384, if PlpType is dispersed
                int  m_SubsliceInterval;    // Interval: 1.. 2^24-1, if PlpType is dispersed

            */

            /*
            DTAPI_ATSC3_TIMODE_NONE No time interleaving
            DTAPI_ATSC3_TIMODE_CTI Convolutional time interleaver (CTI) mode
            DTAPI_ATSC3_TIMODE_HTI Hybrid time interleaver (HTI) mode */

            m_atsc3PlpPars.m_TiMode = DTAPI_ATSC3_TIMODE_NONE;

            /**
                int  m_CtiDepth;            // Convolutional time interleaver depth,
                                    // see DTAPI_ATSC3_CTIDEPTH_xx, if TiMode=CTI
                bool  m_TiExtInterleaving;  // Enable extended interleaving

                // HTI interleaving parameters (only applicable if TiMode=HTI)
                bool  m_HtiInterSubframe;   // Enable inter-subframe interleaving
                int  m_HtiNumTiBlocks;      // If inter-subframe interleaving is disabled: the
                                            // number of TI blocks per interleaving frame.
                                            // If inter-subframe interleaving is enabled:
                                            // the number of subframes over which cells from
                                            // one TI block are carried. Range: 1..16
                int  m_HtiNumFecBlocksMax;  // The maximum number of FEC blocks per interleaving frame
                                            // for the current PLP: 1..4096
                bool  m_HtiCellInterleaver; // Enable the cell-interleaver

            */

            m_atsc3PlpPars.m_NumChannelBonded = 0;

            /*

                int  m_CoreLayerPlpId;      // If enhanced layer, the PLP ID of the corresponding
                                              // core layer. Currently the enhanced layer is scheduled
                                               // with the same number of cells as the core layer.
                int  m_HtiNumFecBlocks;     // Used when TiMode = HTI and core layer.
                                             // The number of FEC blocks per subframe,
                                             // range: 1..m_HtiNumFecBlocksMax
            */

            /*
                // For core layer: used when TiMode = NONE or CTI.
                                    // The number of cells per subframe, -1 means to use the
                                    // full subframe.
                                    // For enhanced layer: -1 means the complete size of the
                                    // first core PLP (identified by m_CoreLayerPlpId).
                                    // Otherwise it is the number of cells of the
                                    // enhanced layer PLP.*/
            m_atsc3PlpPars.m_PlpSize = -1;


            /*
                // If -1, plp_start is automatically set by allocating
                                    // PLPs by increasing PLP index assuming each PLP uses
                                    // m_PlpSize cells (plp_type=non-dispersed) or
                                    // ceil(m_PlpSize/m_NumSubslices) cells
                                    // (plp_type=dispersed).
                                    // For complex FDM allocations the previous automatic
                                    // algorithm is not sufficient so m_PlpStart must be set
                                    // manually.
                                    // For  enhanced layer: It is defined as the starting
                                    // cell counting from the start of core PLP
                                    // (identified by m_CoreLayerPlpId).   */

            m_atsc3PlpPars.m_PlpStart = -1;

            mySubframeParams.m_Plps.push_back(m_atsc3PlpPars);

            DtPlpInpPars myPlpInpPars;
            myPlpInpPars.Init();
            myPlpInpPars.m_DataType = myPlpInpPars.ALP;

            /**
            m_FifoIdx The index of the FIFO used by the associated PLP.

            PLPs in the same group that have �Big-TS� splitting enabled can share the same input FIFO.

            The index will be used in several methods that operate on a specific FIFO (e.g. DtMplpOutpChannel::WriteMplpPacket()).

            The default value of m_FifoIdx is equal to the index in the array of DtPlpInpPars structs. For writing data to the nth PLP
            (which is specified at index n in the array of DtPlpInpPars) you have to use FIFO index n.
            The valid range of m_FifoIdx is 0 � 255.
            */

            myPlpInpPars.m_FifoIdx = i;
            m_Atsc3Pars.m_PlpInputs[i] = myPlpInpPars;

            m_Atsc3Pars.m_Subframes.push_back(mySubframeParams);

        }


        //plp config for DT I/O

        m_Atsc3Pars.m_NumPlpInputs = num_plps;

        DTAPI_RESULT atsc3ParsValid = m_Atsc3Pars.CheckValidity();

        if (atsc3ParsValid != DTAPI_OK) {
            printf("m_Atsc3Pars.CheckValidity returned: %d\n", atsc3ParsValid);
            exit(1);
        }
        else {
            printf("m_Atsc3Pars.CheckValidity returned: DTAPI_OK, continuing\n");
        }


        dr = m_DtOutp.SetModControl(m_Atsc3Pars);
    
        if ( dr != DTAPI_OK )
            throw Exc( c_ErrFailSetModControl, ::DtapiResult2Str(dr) );
    }

    // Set output level of main output (if supported)
    if ( (m_DtOutp.m_HwFuncDesc.m_Flags & DTAPI_CAP_ADJLVL)!=0 )
    {
        // The SetOutputLevel method expects a level expressed in 0.1dBm units
        int LeveldBm = int(m_CmdLineParams.m_OutpLevel.ToDouble() * 10.0);
        dr = m_DtOutp.SetOutputLevel(LeveldBm);
        if ( dr != DTAPI_OK )
            throw Exc( c_ErrFailedToSetOutputLevel, ::DtapiResult2Str(dr) );
    }

    // Set spectral inversion
    if (m_Modulator && m_CmdLineParams.m_SpecInvers.ToBool())
    {
        dr = m_DtOutp.SetRfMode(DTAPI_UPCONV_SPECINV);
        if ( dr != DTAPI_OK )
            throw Exc( c_ErrFailedToSetOutputLevel, ::DtapiResult2Str(dr) );
    }

    // Final initialisation
    dr = m_DtOutp.ClearFifo();          // Clear FIFO (i.e. start with zero load)

    // Set exit load. 4k for all none modulator
    m_ExitLoad = m_Modulator ? 1*1024*1024 : 4*1024;
}

Player::PortType Player::GuessPortTypeFromPars()
{
    if (m_CmdLineParams.m_ModType.IsSet() || m_CmdLineParams.m_CarrierFreq.IsSet())
        return PT_MOD;
    if (m_CmdLineParams.m_Ipa.IsSet())
        return PT_IP;
    if (m_CmdLineParams.m_PlayDtSdiFile)
        return PT_SDI;
    return PT_UNKNOWN;
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::Log -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
//
void Player::Log(const char* pMessage,  bool IgnoreSilence)
{
    // Under linux we can't mix printf and wprintf, so convert
    // to wstring and always use wprintf
    string  Str(pMessage);
    Log(wstring(Str.begin(), Str.end()).c_str(), IgnoreSilence);
}
void Player::Log(const wchar_t* pMessage,  bool IgnoreSilence)
{
    // Check for silent mode
    if ( !IgnoreSilence && m_CmdLineParams.m_SilentMode )
        return;
    // Print message and add new line
    wprintf(pMessage); wprintf(L"\n");
}
// Overload with optional parameters
void Player::LogF(const char* pMessage, ... )
{
    char sz[512];

    va_list  ArgList;
    va_start(ArgList, pMessage);
    _vsnprintf(sz, sizeof(sz)-1, pMessage, ArgList);
    va_end(ArgList);

    Log(sz, false);
}
// Overload with optional parameters
void Player::LogF(const wchar_t* pMessage, ... )
{
    wchar_t sz[512];

    va_list  ArgList;
    va_start(ArgList, pMessage);
    _vsnwprintf(sz, (sizeof(sz)/sizeof(wchar_t))-1, pMessage, ArgList);
    va_end(ArgList);

    Log(sz, false);
}


//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::processDemuxedALPQueue -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
void Player::processPreambleSettings() {

    while (this->processPreambleSettingsThreadShouldRun) {
        //wait condition
         //critical section
        {
            unique_lock<mutex> condition_lock(atsc3_stltp_preamble_packet_queue_mutex);
            atsc3_stltp_preamble_packet_queue_condition.wait(condition_lock);

            while (atsc3_stltp_preamble_packet_queue.size()) {
                //run with the acquired lock, less hassle and low PPS, do not free as that is the depacketizers ownership of preamble packet
                atsc3_stltp_preamble_packet_tv* atsc3_stltp_preamble_packet_tv = atsc3_stltp_preamble_packet_queue.front();
                for (int i = 0; i < atsc3_stltp_preamble_packet_tv->count; i++) {
                    atsc3_stltp_preamble_packet_t* atsc3_stltp_preamble_packet = atsc3_stltp_preamble_packet_tv->data[i];
                    atsc3_preamble_packet_t* atsc3_preamble_packet = atsc3_stltp_preamble_packet->preamble_packet;

                    atsc3_preamble_packet_dump(atsc3_preamble_packet);
                }
                atsc3_stltp_preamble_packet_queue.pop();
            }
            condition_lock.unlock();
        }
    }
}

void Player::processDemuxedALPQueue()
{
    queue<atsc3_alp_packet_t*> to_dispatch_queue; //perform a shallow copy so we can exit critical section asap
    queue<atsc3_alp_packet_t*> to_purge_queue; //perform a shallow copy so we can exit critical section asap

    int total_alp_packet_count = 0;
    block_t* bbp_block = block_Alloc(64000);

    bool EoF(false), TxStarted(false);
    DTAPI_RESULT dr;
    int NumBytesRead, FifoLoad, FifoSize;
    int MinFifoLoad;

    uint32_t num_packets_written = 0;
    //m_DtOutp.SetFifoSize(16384000);
    // Init channel to hold mode
    dr = m_DtOutp.SetTxControl(DTAPI_TXCTRL_HOLD);
    if ( dr != DTAPI_OK )
        throw Exc(c_ErrFailSetTxControl, ::DtapiResult2Str(dr));

    // Get current FIFO-size
    dr = m_DtOutp.GetFifoSize(FifoSize);
    if ( dr != DTAPI_OK )
        throw Exc(c_ErrFailGetFifoSize, ::DtapiResult2Str(dr));

    // Set Minimum Fifoload for SDI playout to FifoSize *3/4 
    MinFifoLoad = c_MinFifoLoad; // (FifoSize * 3) / 4;

    while (this->processDemuxedALPQueueThreadShouldRun)
    {
        // Check for free space in hardware FIFO.
//        dr = m_DtOutp.GetFifoLoad(FifoLoad);
//        if ( (FifoLoad + c_BufSize)>=FifoSize )
//        {
//            // Sleep to wait for some free space
//#ifdef WINBUILD
//            ::Sleep(2);
//#else
//            usleep(2000);
//#endif 
//            continue;
//        }

      
        ///**
        //only read into m_pBuf + sizeof(ALP_packet_header_t)
        //as we also need to set payload length

        //atsc3_alp_packet_header {
        //                            bits
        //    packet_type             3
        //    payload_configuration   1
        //    header_mode             1

        //    length                  11   (16 bits, 2 bytes)
        //*/
           
        //// PCAP Ethernet , IP and UDP-header lengths
        //static const int  ETH_HDR_LENGTH = 14;
        //static const int  IP_HDR_LENGTH = 20;
        //static const int  UDP_HDR_LENGTH = 8;
        //int  SkipLength = 0; //do not skip ip/udp for ATSC3 ALP IP_HDR_LENGTH + UDP_HDR_LENGTH;
        //unsigned short EthHdr[ETH_HDR_LENGTH + IP_HDR_LENGTH + UDP_HDR_LENGTH];
        //// Read PCAP-packet header
        //PcapPckHeader  PckHdr;
        //if (fread(&PckHdr, 1, sizeof(PckHdr), m_pFile) != sizeof(PckHdr))
        //    EoF = true;

        //    if (!EoF)
        //    {
        //        // Sanity check
        //        if (PckHdr.m_InclLen>(unsigned int)c_BufSize)
        //            throw Exc(c_ErrPcapFormat);
        //        int Length = PckHdr.m_InclLen;
        //        // Skip the Ethernet header
        //        if (m_PcapEthernetLinkType)
        //            SkipLength += ETH_HDR_LENGTH;
        //        if (Length <= SkipLength)
        //            throw Exc(c_ErrPcapFormat);
        //        // Skip Ethernet and IP-header
        //        if (fread(EthHdr, 1, SkipLength, m_pFile) != SkipLength)
        //            EoF = true;
        //        if (!EoF)
        //        {
        //            // Read the data part of the packet
        //            NumBytesRead = Length - SkipLength;
        //            if (fread(m_pBuf+2, 1, NumBytesRead, m_pFile) != NumBytesRead)
        //                EoF = true;
        //        }
        //    }

        //    // Only read complete packets of right length
        //    if (EoF)
        //        NumBytesRead = 0;
        //    //// Check length
        //    //else if (NumBytesRead != ISDBS3_TLV_PACKET_SIZE)
        //    //    throw Exc(c_ErrIsdbS3TlvFormat);
        //}
        //else
        //{ 
        //    printf("bulk read code should not be run for atsc3");
        //    exit(-1);

        //    // Read as much bytes as possible into our buffer
        //NumBytesRead = (int)fread( m_pBuf, sizeof(char), c_BufSize, m_pFile );
        //if ( NumBytesRead < c_BufSize )
        //{
        //    // Read error or simple end-of-file
        //    if ( 0 != ferror(m_pFile) )
        //        throw Exc(c_ErrReadFile);
        //    else
        //    {
        //        EoF = true;
        //        // Account for possibility of the file size not being a multiple of 4
        //        NumBytesRead &= ~3;
        //    }
        //}
       
        // There is data write it to our output
        //
        // NOTE: Write blocks until all data has been transferred to the output
        // dr = m_DtOutp.Write( m_pBuf, NumBytesRead );
        //don't confuse this with WriteMplp  for TS188/TS204 / GRE... use WriteMplpPacket

        //pack our ALP header here

        //bits:
        /* +packet_type (000)
            | 
            |  +payload_configuration (0)
            |  |
            |  | +header_mode (0)
            |  | |+length (11 bits)
            |--| ||--     ---- ---| 
            0000 0xxx     xxxx xxxx 
            */

        //critical section
        {
            unique_lock<mutex> condition_lock(atsc3_alp_packet_collection_queue_mutex);
            atsc3_alp_packet_collection_queue_condition.wait(condition_lock);

            while (atsc3_alp_packet_collection_queue.size()) {
                to_dispatch_queue.push(atsc3_alp_packet_collection_queue.front());
                atsc3_alp_packet_collection_queue.pop();
            }
            condition_lock.unlock();
        }

        /*        jjustman-2020-09-02 - TODO: chunk this up into bootstrap_ref emission time and PLP*/

        //                if (last_plp != -1) { // && last_plp != atsc3_alp_packet_to_process->plp_num) {

        //printf("PcapSTLTPVirtualPHY::PcapConsumerThreadRun - pushing %d packets", to_dispatch_queue.size());
        while (to_dispatch_queue.size()) {
            atsc3_alp_packet_t* atsc3_alp_packet_to_process = to_dispatch_queue.front();
            block_Rewind(bbp_block);
            int currentPLP = atsc3_alp_packet_to_process->plp_num;

            if (atsc3_alp_packet_to_process->alp_packet_header.packet_type == 0x00 || atsc3_alp_packet_to_process->alp_packet_header.packet_type == 0x04) {

                block_Rewind(atsc3_alp_packet_to_process->alp_payload);
                uint16_t packet_size = 0;
                uint16_t alp_packet_size = 0;
                uint8_t* ptr = nullptr;

                if (atsc3_alp_packet_to_process->alp_packet_header.packet_type == 0x00) {
                    packet_size = block_Remaining_size(atsc3_alp_packet_to_process->alp_payload);
                    alp_packet_size = 2 + packet_size;
                    ptr = block_Get(bbp_block);

                    ptr[0] = 0x00 | ((packet_size >> 8) & 0x7);
                    ptr[1] = packet_size & 0xFF;
                    bbp_block->i_pos += 2;

                    block_Write(bbp_block, block_Get(atsc3_alp_packet_to_process->alp_payload), packet_size);
                } else if (atsc3_alp_packet_to_process->alp_packet_header.packet_type == 0x04) {
                    //LMT packet
                    block_Rewind(atsc3_alp_packet_to_process->alp_packet_header.alp_header_payload);
                    packet_size = block_Remaining_size(atsc3_alp_packet_to_process->alp_payload);
                    uint16_t alp_header_size =  block_Remaining_size(atsc3_alp_packet_to_process->alp_packet_header.alp_header_payload);
                    alp_packet_size = alp_header_size + packet_size;

                    printf("LMT: packet_num: %d, PLP: %d, with alp_packet_header: %p, header: 0x%02x 0x%02x, alp_packet_header size: %d, and ALP packet size is: %d",
                        num_packets_written,
                        atsc3_alp_packet_to_process->plp_num,
                        block_Get(atsc3_alp_packet_to_process->alp_packet_header.alp_header_payload),
                        atsc3_alp_packet_to_process->alp_packet_header.alp_header_payload->p_buffer[0],
                        atsc3_alp_packet_to_process->alp_packet_header.alp_header_payload->p_buffer[1],
                        alp_header_size,
                        packet_size);

                    block_Write(bbp_block, block_Get(atsc3_alp_packet_to_process->alp_packet_header.alp_header_payload), alp_header_size);
                    block_Write(bbp_block, block_Get(atsc3_alp_packet_to_process->alp_payload), packet_size);
                }

                int bbp_block_len = bbp_block->i_pos; // +(4 - bbp_block->i_pos % 4);
                int free = 0;
                block_Rewind(bbp_block);
                ptr = block_Get(bbp_block);

                if (true || total_alp_packet_count % 500 == 0) {
                    printf("WriteMplpPacket: packet_num: %d, PLP: %d, with m_pBuf: %p, and bbp_block_len packet size is: %d, ALP header is: 0x%02x 0x%02x 0x%02x 0x%02x, bootsrap_timing_ref seconds: 0x%06x, a_milli: 0x%04x \n",
                        num_packets_written,
                        atsc3_alp_packet_to_process->plp_num,
                        ptr,
                        bbp_block_len,
                        ptr[0],
                        ptr[1],
                        ptr[2],
                        ptr[3],
                        atsc3_alp_packet_to_process->bootstrap_timing_data_timestamp_short_reference.seconds_pre,
                        atsc3_alp_packet_to_process->bootstrap_timing_data_timestamp_short_reference.a_milliseconds_pre);
                }

             
                m_DtOutp.GetMplpFifoFree(currentPLP, free);
                if (free < bbp_block_len) {
                    printf("WARNING: FIFO free size less than 1500! m_DtOutp.WriteMplp: currentPLP: %d, block size: %d, fifo free: %d, to_dispatch_queue size: %d\n\n",
                        currentPLP, bbp_block_len, free, to_dispatch_queue.size()
                    );
                }
                else   if (total_alp_packet_count % 500 == 0) {
                    printf("\n\n\Before: m_DtOutp.WriteMplp: currentPLP: %d, block size: %d, fifo free: %d, to_dispatch_queue size: %d\n\n",
                        currentPLP, bbp_block_len, free, to_dispatch_queue.size()
                    );
                }
                dr = m_DtOutp.WriteMplpPacket(currentPLP, (char*)bbp_block->p_buffer, bbp_block_len, DtFractionInt(1, 5000));
                //dr = m_DtOutp.WriteMplp(currentPLP, (char*)bbp_block->p_buffer, block_len);
           
                total_alp_packet_count++;

                if (dr != DTAPI_OK) {
                    throw Exc(c_ErrFailWrite, ::DtapiResult2Str(dr));
                }
            }
            to_purge_queue.push(atsc3_alp_packet_to_process);
            to_dispatch_queue.pop();
        }

        while (to_purge_queue.size()) {
            atsc3_alp_packet_t* atsc3_alp_packet_to_to_purge = to_purge_queue.front();
            to_purge_queue.pop();
            atsc3_alp_packet_free(&atsc3_alp_packet_to_to_purge);
        }
        
        if (TxStarted)
        {
            int  StatusFlags, Latched;
            dr = m_DtOutp.GetFlags(StatusFlags, Latched);
            if (dr != DTAPI_OK)
                throw Exc(c_ErrFailGetFlags, ::DtapiResult2Str(dr));

            if ((Latched & DTAPI_TX_CPU_UFL) != 0)
                throw Exc(c_ErrCpuUnderflow);
            if ((Latched & DTAPI_TX_DMA_UFL) != 0)
                throw Exc(c_ErrDmaUnderflow);
            if ((Latched & DTAPI_TX_FIFO_UFL) != 0) {
                //throw Exc(c_ErrFifoUnderflow);
                printf("Error: FIFO underflow detected, sleeping 10s and continuing...");
                Sleep(10 * 1000);
            }
        }


        // We wait with starting actual transmission until we have build up a minimum FIFO
        // load. If we reach the end-of-file and only one loop left to go start immediately.
        dr = m_DtOutp.GetFifoLoad(FifoLoad);
        if ( !TxStarted && FifoLoad>=MinFifoLoad)
        {
            // Start transmission
            dr = m_DtOutp.SetTxControl(DTAPI_TXCTRL_SEND);
            if ( dr != DTAPI_OK )
                throw Exc(c_ErrFailSetTxControl, ::DtapiResult2Str(dr));

            TxStarted = true;
        }
    }
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::Play -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
int Player::Play(int argc, char* argv[])
{
    int RetValue=0;
    try
    {
        try
        {
            // Parse command line options
            m_CmdLineParams.Init();
            m_CmdLineParams.ParseCommandLine(argc, argv);
        }
        catch ( Exc e ) {
            Log(e, true);
            Log("", true);
            Log("Use -? option for help", true);
            return -1;
        }
        catch (DtOptException e) {
            Log(e, true);
            Log("", true);
            Log("Use -? option for help", true);
            return -1;
        }
        catch ( ... ) {
            return -1;
        }
      
        //-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Print start message -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
        int  Maj=-1,Min=-1,BugFix=-1,Build=-1;
        DtapiGetVersion(Maj,Min,BugFix,Build);
        LogF("DTAPI link version: V%d.%d.%d.%d\n",
              Maj, Min, BugFix, Build);


        //.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Open the SRT+STLTP emission
        
        const char* SRT_HOST_CONNECTION_STRING = "srt://bna.srt.atsc3.com:31348?passphrase=88731837-0EB5-4951-83AA-F515B3BEBC20";

        SRTRxSTLTPVirtualPHY* srtRxSTLTPVirtualPHY = new SRTRxSTLTPVirtualPHY(SRT_HOST_CONNECTION_STRING);
        atsc3_stltp_depacketizer_context_t* atsc3_stltp_depacketizer_context = srtRxSTLTPVirtualPHY->get_atsc3_stltp_depacketizer_context();

        atsc3_stltp_depacketizer_context->atsc3_stltp_timing_management_packet_collection_callback_with_context = atsc3_stltp_timing_management_collection_callback;
        atsc3_stltp_depacketizer_context->atsc3_stltp_timing_management_packet_collection_callback_context = (void*)this;

        atsc3_stltp_depacketizer_context->atsc3_stltp_preamble_packet_collection_callback_with_context = atsc3_stltp_preamble_packet_collection_callback;
        atsc3_stltp_depacketizer_context->atsc3_stltp_preamble_packet_collection_callback_context = (void*)this;

        atsc3_stltp_depacketizer_context->atsc3_stltp_baseband_alp_packet_collection_callback_with_context = atsc3_stltp_baseband_alp_packet_collection_callback;
        atsc3_stltp_depacketizer_context->atsc3_stltp_baseband_alp_packet_collection_callback_context = (void*)this;
        
        srtRxSTLTPVirtualPHY->run();

        double srt_thread_run_start_time = gt();

        //srtRxSTLTPVirtualPHY->stop();

        //delete srtRxSTLTPVirtualPHY;

      
        //-.-.-.-.-.-.-.-.-.-.-.-.- Attach to the output channel -.-.-.-.-.-.-.-.-.-.-.-.-
        AttachToOutput();

        //-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Final initialisation -.-.-.-.-.-.-.-.-.-.-.-.-.-.-

        // Init output
        InitOutput();

        // Create our transfer buffer
        m_pBuf = new char [c_BufSize];

        //.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Start playing -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-

        // Print start info
        DisplayPlayInfo();

        processPreambleSettingsThreadShouldRun = true;
        processPreambleSettingsThread = std::thread([this]() {
            processPreambleSettingsThreadShutdown = false;
            processPreambleSettings();
            processPreambleSettingsThreadShutdown = true;
        });

        processDemuxedALPQueueThreadShouldRun = true;
        processDemuxedALPQueueThread = std::thread([this]() {
            processDemuxedALPQueueThreadShutdown = false;
            processDemuxedALPQueue();
            processDemuxedALPQueueThreadShutdown = true;
       });

      //spin
        while (true) {
            usleep(100000);
        }
    }
    catch( Exc e )
    {
        Log(e, true);
        RetValue = -1;
    }

    // Detach from channel and device
    m_DtOutp.SetTxControl(DTAPI_TXCTRL_IDLE);
    m_DtOutp.Detach(DTAPI_INSTANT_DETACH);
    m_DtDvc.Detach();

    // Free our buffer
    if ( m_pBuf != NULL ) {
        delete [] m_pBuf;
        m_pBuf = NULL;
    }

    // donot forget to close our file
    if ( m_pFile != NULL ) {
        ::fclose(m_pFile);
        m_pFile = NULL;
    }

    return RetValue;
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Player::ShowHelp -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
void Player::ShowHelp()
{
    Log( "" );
    Log( "Usage:");
    Log( "   DtPlay playfile [-l count] [-r rate] [-t type] [-n number] [-i port]\n" \
         "          [-m mode] [-mt type] [-mf freq] [-mc coderate] [-ma annex]\n" \
         "          [-mC constellation] [-mB bandwidth] [-mT txmode] [-mG gaurditv]\n" \
         "          [-mH hdrmode] [-mP Pilots] [-mF fecframe] [-mI initval]\n" \
         "          [-ml level_dbm] [-snr snr_db]\n" \
         "          [-ipa ip_address_pair] [-ipp protocol] [-ipn num_tp_per_ip]\n" \
         "          [-mS Stuffing]\n" \
         "          [-s] [-?]");
    Log( "");
    Log("Where:");
    Log("   playfile is the name of the file to output");
    Log( "");
    Log("Options:");
    DtOptItem::PrintOptionsHelp(g_CmdOptions, g_NumCmdOptions);
    Log("Examples:");
    Log("   DtPlay myfile.ts -r 38000000");
    Log("   DtPlay myfile.ts -r 38000000 -t 100 -n 2 -m RAW ");
    Log("   DtPlay myfile.ts -r 38000000 -t 107 -mt DVBS -mf 1915.0 -mc 7/8 -snr 26.0");
    Log("   DtPlay myfile.dtsdi -t 2144 -i 1 -m DTSDI -l 0 -mS ON");
    Log("   DtPlay myfile.pcap -t 2115 -mt ISDBS3 -mf 1578");
    LogF("DtPlay version: %d.%d.%d\n", DTPLAY_VERSION_MAJOR, DTPLAY_VERSION_MINOR,
         DTPLAY_VERSION_BUGFIX);
}

//+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+ Application entry point +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- main -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
int main(int argc, char* argv[])
{
    int RetValue(0);
    Player ThePlayer;
    try
    {   
        RetValue = ThePlayer.Play(argc, argv);
    }
    catch(...)
    {
        RetValue = -1;
    }
    return RetValue;
}
