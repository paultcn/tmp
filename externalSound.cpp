/*
================================================================================

@file    ExternalAudioManagerTest.cpp
@brief   external audio manager function test

================================================================================
*/

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/pathmgr.h>
#include <string>
#include <mqueue.h>

/*
#include <iostream>
#include <ctime>
#include <ratio>
#include <chrono>
using namespace std::chrono;
*/
#include <time.h>
#include <pthread.h>
#include "ping-pong.h"
#include "externalSound.h"
#include "chime_file.h"
#include "avasSvr.h"

#define TEST_AVAS_FILE       "/logdata/wav_data/Chime04.wav"
#define TEST_POWEROFF_FILE   "/logdata/wav_data/boot.wav"
#define TEST_CHARGING_FILE   "/logdata/wav_data/Chime05.wav"
#define AVAS_GENDATA_BUFFSIZE (1536)

typedef struct
{
    const std::string param_str[30];
    const std::string help_str[30];
    int len;
}parameter_t;

static  parameter_t par_str[]={
        {
            {"play_type","pData","data_len","play_count","interval"},
            {"play_type:\n"
                  "1-external_sound_type_avas\n"
                  "2-external_sound_type_shutdown\n"
                  "3-external_sound_type_charge\n"
                  "others-invalid param\n",
              "pData:pointer to wav data buffer,can't set by test app",
              "data_len:the wav data buffer size,can't set by test app",
              "play_count:how many times this file will be play,0 means infinity",
              "interval:the time between current play finish and next time play start",
            },
            5
        },
        {
            {"play_type","stop_tyle"},
            {"play_type:\n"
                  "1-external_sound_type_avas\n"
                  "2-external_sound_type_shutdown\n"
                  "3-external_sound_type_charge\n"
                  "others-invalid param\n",
             "stop_tyle:\n"
                 "1-external_sound_stop_type_immediately\n"
                 "2-external_sound_stop_type_previous\n"
                 "others-invalid param\n",
            },
            2
        }
};


//声道数
static int s_channels = 1;
static int s_block_align = s_channels*2;
static int playState = 0;

static uint32_t s_bufIdx = 0;
//wav file size
static uint32_t s_bufLen = 0;
//wav file buffer
static char* s_buf = NULL;
//avas gendata buffer
static char* s_avasDataPing = NULL;
static char* s_avasDataPong = NULL;
static char* s_avasDataLastBackup = NULL;
static PingPongBuffer_t s_PPBuf;	
static int tmp_value[20]={0};
//static float tmp_f[10]={0};
static char *pData = NULL;
static uint32_t data_len = 0;
static ChimeFileBuffer chimeFileBuffer[3];
static pthread_mutex_t lastest_read_lock;
	
void* avas_gendata(void * args)
{
	MQ_MSG msg;
	struct mq_attr mqstat;
    int msg_size;
	
	char *buffer = NULL;
	
	static AvasServer::AVAS_MODE s_cur_avas_mode = AvasServer::MODE_OFF;
	static int32_t s_cur_speed = 0;                        // vehicle speed.
	//static int32_t s_cur_speedValid = 0;                 // vehicle speed valid.
	static int32_t s_cur_gear = 0;                       // gear type.

    AVAS_CTL_PARAM param;
	AVAS_CTL_STATE state;
	uint32_t numberofsample = AvasLib_GetnumberOfSample();
	
    char* outdata = NULL;

	buffer = (char *)malloc(numberofsample*sizeof(int32_t));
	if ( buffer == NULL )
		return(NULL);
	
	// qnx message queue attributes
	mq_getattr(g_mqMsgQ, &mqstat);
	memset(&msg, 0, sizeof(MQ_MSG));
	while ( !sig_exit )
	{
		msg_size = mq_receive(g_mqMsgQ, (char *) &msg, (long) &mqstat.mq_msgsize,NULL);
		// check for error
		if (msg_size == -1) {
			LOG_OUT_ERR("MQ_RECEIVE fail\n");
		}
		if (msg_size != 0) {

			// only new valid vehicle signal received can update the vehicle signal variant,
			// otherwise, keep the static variant last value.
		    if ( msg.speed > 0 && msg.speedValid )
		    {
				s_cur_speed = msg.speed;
				//s_cur_speedValid = msg.speedValid;
				s_cur_avas_mode = msg.avas_mode;
				s_cur_gear = msg.gear;
		    }
		}

		PingPongBuffer_GetWriteBuf(&s_PPBuf, (void**)&outdata); 	
		if ( outdata == NULL )
			return(NULL);
		
		/* prepare for avas lib */
		param.nCurSpeed = s_cur_speed;
		//0:P,5:D,6:N,7:R,8:M
		switch(s_cur_gear)
		{
			case 0:
			param.nCurGear = 0;
			break;
			case 5:
			param.nCurGear = 4;
			break;
			case 6:
			param.nCurGear = 3;
			break;
			case 7:
			param.nCurGear = 2;
			break;
			case 8:
			param.nCurGear = 4;
			break;
			default:
			param.nCurGear = 0;
		}
		AvasLib_SetParam(&param, &state);
		if(param.nCurSpeed > 30 || (param.nCurGear != 4 && param.nCurGear != 2)
			|| s_cur_avas_mode == AvasServer::MODE_OFF)
			AvasLib_SetWorkState(AVAS_OFF);
		else
			AvasLib_SetWorkState(AVAS_ON);
		LOG_OUT_INFO("speed = %dkm/h, gear = %d\n", param.nCurSpeed, param.nCurGear);
		if ( DebugLevel > 4 )
		{
			printf("speed = %dkm/h, gear = %d\n", param.nCurSpeed, param.nCurGear);
		}
		
		/* avas data generate to play */
		for(unsigned int i=0; i< AVAS_GENDATA_BUFFSIZE/sizeof(int32_t) / numberofsample; i++)
		{
			if(s_bufLen - s_bufIdx >= numberofsample*sizeof(int32_t))
			{
				memcpy(buffer, (char*)s_buf+s_bufIdx, numberofsample*sizeof(int32_t));
				//buffer = (int32_t *)((char*)s_buf+s_bufIdx);
				s_bufIdx += numberofsample*sizeof(int32_t);
			}
			else
			{
				int len = s_bufLen - s_bufIdx;
				if(len > 0)
					memcpy(buffer, (char*)s_buf+s_bufIdx, len);
				memcpy(buffer + s_bufLen - s_bufIdx, (char*)s_buf, numberofsample*sizeof(int32_t) - len);
				
				s_bufIdx = numberofsample*sizeof(int32_t) - len;
				//buffer = (int32_t *)(char*)s_buf;
			}
			AvasLib_GenData((int32_t *)buffer, (int32_t *)outdata+i*numberofsample);
		}		
		PingPongBuffer_SetWriteDone(&s_PPBuf);
        // wait the lastest gen data be reading finished, then can overwrite.
		pthread_mutex_lock(&lastest_read_lock);
		memcpy(s_avasDataLastBackup, outdata, AVAS_GENDATA_BUFFSIZE);
		pthread_mutex_unlock(&lastest_read_lock);

	}
	if (buffer) free(buffer);
	return NULL;
}


//回调写入播放数据，需要根据实际情况进行逻辑处理
int writeData(int nframes, void *arg, int size)
{
    DataBuf *pBufs = (DataBuf *)arg;
    unsigned int bufSize = nframes * s_block_align;
    unsigned int nChMax = ( s_channels > 2 ? 2 : s_channels );
    char* pDestBuf[16];

	char *outdata = NULL;
	char *pSrcBuffer = NULL;
	unsigned int waitTime = 0;

	static int8_t s_lastest_readIndex = -1;
	bool bGotReadbuf = false;

    playState = 1;
    if(s_bufIdx == 0)
    {
        LOG_OUT_INFO("%s:%d:%d:%d:%d\n",__func__,nframes,size,s_channels,s_block_align);
		if ( DebugLevel > 4 )
		{
			printf("%s:%d:%d:%d:%d\n",__func__,nframes,size,s_channels,s_block_align);
		}
    }

	waitTime = 0;		
	while ( (bGotReadbuf = PingPongBuffer_GetReadBuf(&s_PPBuf, (void**)&outdata) ) == false
	        && s_lastest_readIndex == -1)
	{
	    delay( 1 ); 
		if ( DebugLevel > 4 )
		{
			printf("wait for available avas data %dms\n", waitTime++);
		}
	}
	// got new buffer falied, read the lastest data buff.
	if ( bGotReadbuf == false )
	{
		pthread_mutex_lock(&lastest_read_lock);
	    outdata = s_avasDataLastBackup;
	}
	else
	{
    	s_lastest_readIndex = s_PPBuf.readIndex;
	}
    pSrcBuffer = outdata ? (char*)outdata : s_buf+s_bufIdx;
	
    for(unsigned int ch = 0; ch < nChMax; ch++)
    {
        pDestBuf[ch] = (char*)(pBufs[ch].ioBuf);
    }
	
	for(unsigned int i = 0; i < bufSize; i+=s_block_align)
	{
		for(unsigned int ch = 0; ch < nChMax; ch++)
		{
			*pDestBuf[ch]++ = pSrcBuffer[ch*2+i];
			*pDestBuf[ch]++ = pSrcBuffer[ch*2+i+1];
		}
	}
	if ( DebugLevel > 4 )
	{
	  printf("%s: play total:%d ===> %d\n", __func__, s_bufLen, s_bufIdx);
	}
	if ( bGotReadbuf == false )
	{
		pthread_mutex_unlock(&lastest_read_lock);
	}
	else
	{
        PingPongBuffer_SetReadDone(&s_PPBuf);
	}

    return 0;
	
}

void external_sound_init()
{
    pthread_mutex_init(&lastest_read_lock, NULL);
    // prepare ping-pong buffer for avas to gen data.
	s_avasDataPing = (char*)malloc(AVAS_GENDATA_BUFFSIZE);
	if ( s_avasDataPing == NULL )
	{
		if ( DebugLevel > 4 )
		{
		  printf("The s_avasDataPing malloc failed !!!\n");
		}
	    return;
	}
	else
	{
	    memset(s_avasDataPing, 0, AVAS_GENDATA_BUFFSIZE);
	}
	s_avasDataPong = (char*)malloc(AVAS_GENDATA_BUFFSIZE);
	if ( s_avasDataPong == NULL )
	{
		if ( DebugLevel > 4 )
		{
		  printf("The s_avasDataPong malloc failed !!!\n");
		}
	    return;
	}
	else
	{
	    memset(s_avasDataPong, 0, AVAS_GENDATA_BUFFSIZE);
	}
	s_avasDataLastBackup = (char*)malloc(AVAS_GENDATA_BUFFSIZE);
	if ( s_avasDataLastBackup == NULL )
	{
		if ( DebugLevel > 4 )
		{
		  printf("The s_avasDataLastBackup malloc failed !!!\n");
		}
	    return;
	}
	else
	{
	    memset(s_avasDataLastBackup, 0, AVAS_GENDATA_BUFFSIZE);
	}
	
	PingPongBuffer_Init(&s_PPBuf, s_avasDataPing, s_avasDataPong);
    initExternalSound(external_sound_type_avas);
}
void external_sound_destroy()
{
    destroyExternalSound(external_sound_type_avas);
	if (s_avasDataPing)
		free(s_avasDataPing);
	if (s_avasDataPong)
		free(s_avasDataPong);
	if (s_avasDataLastBackup)
		free(s_avasDataLastBackup);
	pthread_mutex_destroy(&lastest_read_lock);
}
void external_sound_play(char *wav_filename)
{
    int rc = -1;

	if( NULL == wav_filename )
	{
		LOG_OUT_ERR("The wav_filename can't been NULL !!!\n");
		if ( DebugLevel > 4 )
		{
		  printf("The wav_filename can't been NULL !!!\n");
		}
		return;
	}

	
	//SLOG_I("play_type = %d\n", tmp_value[0]);
	//SLOG_I("play_count = %d\n", tmp_value[3]);
	//SLOG_I("interval = %d\n",  tmp_f[0]);
	
    memset(&(chimeFileBuffer[0].file_info), 0, sizeof(ChimeFileInfo));
	strncpy(chimeFileBuffer[0].file_info.filename,wav_filename,FILE_NAME_SIZE);
	
	rc= read_file_to_buffer(&(chimeFileBuffer[0]));
	
	if (rc == NO_ERROR){
	  pData = (char *)chimeFileBuffer[0].wav_data_buffer;
	  data_len = chimeFileBuffer[0].file_info.data_size_total;
	
	  LOG_OUT_INFO("pData =0X%p data_len = %d\n", pData, data_len);
	  LOG_OUT_INFO("play\n");
	   if ( DebugLevel > 4 )
	   {
		 printf("pData =0X%p data_len = %d\n", pData, data_len);
	   }
	
	   //适用于一次性把wav数据传入pData，比如充电枪插入之类的音源
	 // play((play_type_enum)external_sound_type_avas, pData,data_len, tmp_value[3], tmp_f[0]);
	
	  //需要持续通过回调writeData写入播放数据,调用stop停止，否则回调会一直触发
	  s_buf = (char *)chimeFileBuffer[0].wav_data_buffer;
	  s_bufLen = chimeFileBuffer[0].file_info.data_size_total;
	  int ret = play((play_type_enum)external_sound_type_avas, writeData);
	  LOG_OUT_INFO("play ret:%d\n", ret);
	  if ( DebugLevel > 4 )
	  {
		printf("play ret:%d\n", ret);
	  }
	}
}

void external_sound_stop()
{
	LOG_OUT_INFO("play_type = %d\n", tmp_value[0]);
	LOG_OUT_INFO("stop_tyle = %d\n", tmp_value[1]);
	LOG_OUT_INFO("stop\n");
	
	stop(external_sound_type_avas,external_sound_stop_type_immediately);
}

