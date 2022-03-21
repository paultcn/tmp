#include <errno.h>
#include <fcntl.h>
#include <gulliver.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/termio.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/slogcodes.h>
#include <sys/slog2.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>

#include <sys/asoundlib.h>
#include "avasSvr.h"
#include "externalSound.h"

static pthread_t writer_thread;
static snd_pcm_t *pcm_handle;

static int err (char *msg)
{
    perror (msg);
    return -1;
}

static void *writer_thread_handler(void *data)
{
    snd_pcm_t *pcm_handle = (snd_pcm_t *)data;
    sigset_t signals;
    int     written = 0, n = 0;
    snd_pcm_channel_status_t status;
    unsigned int nChMax = ( g_channels > 2 ? 2 : g_channels );

    sigfillset (&signals);
    pthread_sigmask (SIG_BLOCK, &signals, NULL);

    char io_buf[nChMax][AVAS_GENDATA_BUFFSIZE];
	DataBuf outdata[nChMax];
 	memset(outdata, 0 , nChMax*AVAS_GENDATA_BUFFSIZE);
    for (int i=0; i<nChMax; i++)
    {
        outdata[i].ioBuf = (int32_t*)io_buf[i];
        outdata[i].bufType = DataBufGeneral;
    }

    while ( !sig_exit )
    {
        PRINT_TIMESTAMP("write frame begin ")
 	    memset(io_buf, 0 , nChMax*AVAS_GENDATA_BUFFSIZE);
        writeData(AVAS_GENDATA_BUFFSIZE/g_block_align, outdata, 1);
        for (int i=0;i<nChMax; i++)
        {
            written = snd_pcm_plugin_write (pcm_handle, outdata[i].ioBuf, AVAS_GENDATA_BUFFSIZE);
            if (written < AVAS_GENDATA_BUFFSIZE)
            {
                memset (&status, 0, sizeof (status));
                status.channel = SND_PCM_CHANNEL_PLAYBACK;
                if (snd_pcm_plugin_status (pcm_handle, &status) < 0)
                {
                    fprintf (stderr, "underrun: playback channel status error\n");
                    exit (1);
                }
                if (status.status == SND_PCM_STATUS_READY ||
                    status.status == SND_PCM_STATUS_UNDERRUN)
                {
                    if (snd_pcm_plugin_prepare (pcm_handle, SND_PCM_CHANNEL_PLAYBACK) < 0)
                    {
                        fprintf (stderr, "underrun: playback channel prepare error\n");
                        exit (1);
                    }
                }
                if (written < 0)
                    written = 0;
                written = snd_pcm_plugin_write (pcm_handle, outdata[i].ioBuf + written, AVAS_GENDATA_BUFFSIZE - written);
                debug_info(("%s -> write left %d\n",__func__, written));
            }
        }
		PRINT_TIMESTAMP("write frame end ")
    }
    snd_pcm_plugin_flush (pcm_handle, SND_PCM_CHANNEL_PLAYBACK);
    snd_pcm_close (pcm_handle);

    return NULL;
}

int ioaudio_sound_init(char *soundDevName, ChimeFileBuffer *chimeFileBuffer)
{

    int     mSamples;
    int     rtn;
    snd_pcm_channel_info_t pi;
    snd_pcm_channel_params_t pp;
    snd_pcm_channel_setup_t setup;
    int     num_frags = -1;

    char*   name = soundDevName;
    int     card = -1;
    int     dev = 0;

	
    //两种方式打开音频设备; 首选 打开设备 /dev/snd/pcmC0D0p , 设备名根据实际情况变化;
    if (name[0] != '\0')
        printf ("Using device /dev/snd/%s\n", name);
    else
        printf ("Using card %d device %d \n", card, dev);

    if (name[0] != '\0')
    {
        snd_pcm_info_t info;

        if ((rtn = snd_pcm_open_name (&pcm_handle, name, SND_PCM_OPEN_PLAYBACK)) < 0)
        {
            return err ((char *)"open_name");
        }
        rtn = snd_pcm_info (pcm_handle, &info);
        card = info.card;
        printf("snd_pcm_open_name\n");
    }
    else
    {
        if (card == -1)
        {
            if ((rtn = snd_pcm_open_preferred (&pcm_handle, &card, &dev, SND_PCM_OPEN_PLAYBACK)) < 0)
                return err ((char *)"device open");
            printf("snd_pcm_open_preferred\n");
        }
        else
        {
            if ((rtn = snd_pcm_open (&pcm_handle, card, dev, SND_PCM_OPEN_PLAYBACK)) < 0)
                return err ((char *)"device open");
            printf("snd_pcm_open\n");
        }
    }


    memset (&pi, 0, sizeof (pi));
    pi.channel = SND_PCM_CHANNEL_PLAYBACK;
    if ((rtn = snd_pcm_channel_info (pcm_handle, &pi)) < 0)
    {
        fprintf (stderr, "snd_pcm_channel_info failed: %s\n", snd_strerror (rtn));
        return -1;
    }

    memset (&pp, 0, sizeof (pp));

    pp.mode = SND_PCM_MODE_BLOCK;
    pp.channel = SND_PCM_CHANNEL_PLAYBACK;
    pp.start_mode = SND_PCM_START_FULL;
    pp.stop_mode = SND_PCM_STOP_STOP;

    pp.buf.block.frag_size = pi.max_fragment_size;
    //pp.buf.block.frag_size = 64*4;

    pp.buf.block.frags_max = num_frags;
    pp.buf.block.frags_min = 1;

    pp.format.interleave = 1;
    pp.format.rate = chimeFileBuffer->file_info.sample_rate;//mSampleRate;
    pp.format.voices = chimeFileBuffer->file_info.sample_channel;//mSampleChannels;

#if 0
    if (ENDIAN_LE16 (wavHdr1.FormatTag) == 6)
        pp.format.format = SND_PCM_SFMT_A_LAW;
    else if (ENDIAN_LE16 (wavHdr1.FormatTag) == 7)
        pp.format.format = SND_PCM_SFMT_MU_LAW;
    else if (mSampleBits == 8)
        pp.format.format = SND_PCM_SFMT_U8;
    else if (mSampleBits == 24)
        pp.format.format = SND_PCM_SFMT_S24;
    else
        pp.format.format = SND_PCM_SFMT_S16_LE;
#endif

	/* Support 8, 16, and 24 bit linear PCM audio */
	if (chimeFileBuffer->file_info.sample_bits == 8)
		pp.format.format = SND_PCM_SFMT_U8;
	else if (chimeFileBuffer->file_info.sample_bits == 24)
		pp.format.format = SND_PCM_SFMT_S24;
	else
		pp.format.format = SND_PCM_SFMT_S16_LE;

    strcpy (pp.sw_mixer_subchn_name, "Wave playback channel");
    if ((rtn = snd_pcm_plugin_params (pcm_handle, &pp)) < 0)
    {
        fprintf (stderr, "snd_pcm_plugin_params failed: %s\n", snd_strerror (rtn));
        return -1;
    }
    if ((rtn = snd_pcm_playback_prepare (pcm_handle)) < 0)
        fprintf (stderr, "snd_pcm_playback_prepare failed: %s\n", snd_strerror (rtn));

    memset (&setup, 0, sizeof (setup));
    setup.channel = SND_PCM_CHANNEL_PLAYBACK;
    if ((rtn = snd_pcm_plugin_setup (pcm_handle, &setup)) < 0)
    {
        fprintf (stderr, "snd_pcm_plugin_setup failed: %s\n", snd_strerror (rtn));
        return -1;
    }
    printf ("Format %s \n", snd_pcm_get_format_name (setup.format.format));
    printf ("Frag Size %d \n", setup.buf.block.frag_size);
    printf ("Total Frags %d \n", setup.buf.block.frags);
    printf ("Rate %d \n", setup.format.rate);
    printf ("Voices %d \n", setup.format.voices);

    mSamples = chimeFileBuffer->file_info.data_size_total;
    printf("wave length:%d\n", mSamples);


    return (0);
}
void ioaudio_sound_play()
{
	pthread_create( &writer_thread, NULL, writer_thread_handler, pcm_handle );
}
